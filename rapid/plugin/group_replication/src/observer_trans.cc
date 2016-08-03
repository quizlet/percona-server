/* Copyright (c) 2013, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include <string>
#include <vector>

#include "observer_trans.h"
#include "plugin_log.h"
#include <mysql/service_rpl_transaction_ctx.h>
#include <mysql/service_rpl_transaction_write_set.h>
#include "sql_command_test.h"
#include "sql_service_interface.h"
#include "sql_service_command.h"
#include "base64.h"

/*
  Buffer to read the write_set value as a string.
  Since we support up to 64 bits hashes, 8 bytes are enough to store the info.
*/
#define BUFFER_READ_PKE 8

/*
  Map to store all open IO_CACHE, each server session does have one cache.
  Since each session, after first transaction, has its cache open, its
  handling it is much faster.
*/
typedef std::map<my_thread_id, IO_CACHE*> IO_CACHE_map;
static IO_CACHE_map io_cache_map;

/*
  Read/write lock to protect map find operations against new cache inserts.
*/
static Checkable_rwlock *io_cache_map_lock= NULL;

/*
  Read/write lock to protect map clear operation against find and insert
  operations.
*/
static Checkable_rwlock *io_cache_map_clear_lock= NULL;

void observer_trans_initialize()
{
  DBUG_ENTER("observer_trans_initialize");

  io_cache_map_lock= new Checkable_rwlock(
#ifdef HAVE_PSI_INTERFACE
    key_GR_RWLOCK_io_cache_map
#endif /* HAVE_PSI_INTERFACE */
  );

  io_cache_map_clear_lock= new Checkable_rwlock(
#ifdef HAVE_PSI_INTERFACE
    key_GR_RWLOCK_io_cache_map_clear
#endif /* HAVE_PSI_INTERFACE */
  );

  DBUG_VOID_RETURN;
}

void observer_trans_terminate()
{
  DBUG_ENTER("observer_trans_terminate");

  delete io_cache_map_lock;
  io_cache_map_lock= NULL;
  delete io_cache_map_clear_lock;
  io_cache_map_clear_lock= NULL;

  DBUG_VOID_RETURN;
}

void observer_trans_clear_io_cache_map()
{
  DBUG_ENTER("observer_trans_clear_io_cache_map");
  io_cache_map_clear_lock->wrlock();

  for (IO_CACHE_map::iterator it= io_cache_map.begin();
       it != io_cache_map.end();
       ++it)
  {
    IO_CACHE *cache= it->second;
    close_cached_file(cache);
    my_free(cache);
  }

  io_cache_map.clear();

  io_cache_map_clear_lock->unlock();
  DBUG_VOID_RETURN;
}

/*
  Internal auxiliary functions signatures.
*/
static bool reinit_cache(IO_CACHE *cache,
                         enum cache_type type,
                         my_off_t position);

IO_CACHE* observer_trans_get_io_cache(my_thread_id thread_id,
                                      ulonglong cache_size);

void cleanup_transaction_write_set(Transaction_write_set *transaction_write_set)
{
  DBUG_ENTER("cleanup_transaction_write_set");
  if (transaction_write_set != NULL)
  {
    my_free (transaction_write_set->write_set);
    my_free (transaction_write_set);
  }
  DBUG_VOID_RETURN;
}

int add_write_set(Transaction_context_log_event *tcle,
                  Transaction_write_set *set)
{
  DBUG_ENTER("add_write_set");
  int iterator= set->write_set_size;
  for (int i = 0; i < iterator; i++)
  {
    uchar buff[BUFFER_READ_PKE];
    int8store(buff, set->write_set[i]);
    uint64 const tmp_str_sz= base64_needed_encoded_length((uint64) BUFFER_READ_PKE);
    char *write_set_value= (char *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              tmp_str_sz, MYF(MY_WME));
    if (!write_set_value)
    {
      log_message(MY_ERROR_LEVEL, "No memory to generate write identification hash");
      DBUG_RETURN(1);
    }

    if (base64_encode(buff, (size_t) BUFFER_READ_PKE, write_set_value))
    {
      log_message(MY_ERROR_LEVEL,
                  "Base 64 encoding of the write identification hash failed");
      DBUG_RETURN(1);
    }

    tcle->add_write_set(write_set_value);
  }
  DBUG_RETURN(0);
}

/*
  Transaction lifecycle events observers.
*/

int group_replication_trans_before_dml(Trans_param *param, int& out)
{
  DBUG_ENTER("group_replication_trans_before_dml");

  out= 0;

  //If group replication has not started, then moving along...
  if (!plugin_is_group_replication_running())
  {
    DBUG_RETURN(0);
  }

  /*
   The first check to be made is if the session binlog is active
   If it is not active, this query is not relevant for the plugin.
   */
  if(!param->trans_ctx_info.binlog_enabled)
  {
    DBUG_RETURN(0);
  }

  /*
   In runtime, check the global variables that can change.
   */
  if( (out+= (param->trans_ctx_info.binlog_format != BINLOG_FORMAT_ROW)) )
  {
    log_message(MY_ERROR_LEVEL, "Binlog format should be ROW for Group Replication");

    DBUG_RETURN(0);
  }

  if( (out+= (param->trans_ctx_info.binlog_checksum_options !=
                                                   binary_log::BINLOG_CHECKSUM_ALG_OFF)) )
  {
    log_message(MY_ERROR_LEVEL, "binlog_checksum should be NONE for Group Replication");

    DBUG_RETURN(0);
  }

  if ((out+= (param->trans_ctx_info.transaction_write_set_extraction ==
              HASH_ALGORITHM_OFF)))
  {
    log_message(MY_ERROR_LEVEL,
                "A transaction_write_set_extraction algorithm "
                "should be selected when running Group Replication");
    DBUG_RETURN(0);
  }

  if ((out+= (param->trans_ctx_info.tx_isolation == ISO_SERIALIZABLE)))
  {
    log_message(MY_ERROR_LEVEL, "Transaction isolation level (tx_isolation) "
                "is set to SERIALIZABLE, which is not compatible with Group "
                "Replication");
    DBUG_RETURN(0);
  }
  /*
    Cycle through all involved tables to assess if they all
    comply with the plugin runtime requirements. For now:
    - The table must be from a transactional engine
    - It must contain at least one primary key
   */
  for(uint table=0; out == 0 && table < param->number_of_tables; table++)
  {
    if (param->tables_info[table].db_type != DB_TYPE_INNODB)
    {
      log_message(MY_ERROR_LEVEL, "Table %s does not use the InnoDB storage "
                                  "engine. This is not compatible with Group "
                                  "Replication",
                  param->tables_info[table].table_name);
      out++;
    }

    if(param->tables_info[table].number_of_primary_keys == 0)
    {
      log_message(MY_ERROR_LEVEL, "Table %s does not have any PRIMARY KEY. This is not compatible with Group Replication",
                  param->tables_info[table].table_name);
      out++;
    }
  }

  DBUG_RETURN(0);
}

int group_replication_trans_before_commit(Trans_param *param)
{
  DBUG_ENTER("group_replication_trans_before_commit");
  int error= 0;

  DBUG_EXECUTE_IF("group_replication_force_error_on_before_commit_listener",
                  DBUG_RETURN(1););

  DBUG_EXECUTE_IF("group_replication_before_commit_hook_wait",
                  {
                    const char act[]= "now wait_for continue_commit";
                    DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
                  });

  /*
    If the originating id belongs to a thread in the plugin, the transaction
    was already certified. Channel operations can deadlock against
    plugin/applier thread stops so they must remain outside the plugin stop
    lock below.
  */
  Replication_thread_api channel_interface;
  if (channel_interface.is_own_event_applier(param->thread_id,
                                             "group_replication_applier") ||
      channel_interface.is_own_event_applier(param->thread_id,
                                             "group_replication_recovery"))
  {
    DBUG_RETURN(0);
  }

  plugin_stop_lock->rdlock();

  /* If the plugin is not running, before commit should return success. */
  if (!plugin_is_group_replication_running())
  {
    plugin_stop_lock->unlock();
    DBUG_RETURN(0);
  }

  DBUG_ASSERT(applier_module != NULL && recovery_module != NULL);
  Group_member_info::Group_member_status member_status=
      local_member_info->get_recovery_status();

  if (member_status == Group_member_info::MEMBER_IN_RECOVERY)
  {
    log_message(MY_ERROR_LEVEL,
                "Transaction cannot be executed while Group Replication is recovering."
                " Try again when the server is ONLINE.");
    plugin_stop_lock->unlock();
    DBUG_RETURN(1);
  }

  if (member_status == Group_member_info::MEMBER_ERROR)
  {
    log_message(MY_ERROR_LEVEL,
                "Transaction cannot be executed while Group Replication is on ERROR state."
                " Check for errors and restart the plugin");
    plugin_stop_lock->unlock();
    DBUG_RETURN(1);
  }

  if (member_status == Group_member_info::MEMBER_OFFLINE)
  {
    log_message(MY_ERROR_LEVEL,
                "Transaction cannot be executed while Group Replication is OFFLINE."
                " Check for errors and restart the plugin");
    plugin_stop_lock->unlock();
    DBUG_RETURN(1);
  }

  // Transaction information.
  const bool is_gtid_specified= param->gtid_info.type == GTID_GROUP;
  Gtid gtid= { param->gtid_info.sidno, param->gtid_info.gno };
  if (!is_gtid_specified)
  {
    // Dummy values that will be replaced after certification.
    gtid.sidno= 1;
    gtid.gno= 1;
  }

  const Gtid_specification gtid_specification= { GTID_GROUP, gtid };
  Gtid_log_event *gle= NULL;

  Transaction_context_log_event *tcle= NULL;

  // group replication cache.
  IO_CACHE *cache= NULL;

  // Todo optimize for memory (IO-cache's buf to start with, if not enough then trans mem-root)
  // to avoid New message create/delete and/or its implicit MessageBuffer.
  Transaction_Message transaction_msg;

  enum enum_gcs_error send_error= GCS_OK;

  // Binlog cache.
  bool is_dml= true;
  IO_CACHE *cache_log= NULL;
  my_off_t cache_log_position= 0;
  bool reinit_cache_log_required= false;
  const my_off_t trx_cache_log_position= my_b_tell(param->trx_cache_log);
  const my_off_t stmt_cache_log_position= my_b_tell(param->stmt_cache_log);

  if (trx_cache_log_position > 0 && stmt_cache_log_position == 0)
  {
    cache_log= param->trx_cache_log;
    cache_log_position= trx_cache_log_position;
  }
  else if (trx_cache_log_position == 0 && stmt_cache_log_position > 0)
  {
    cache_log= param->stmt_cache_log;
    cache_log_position= stmt_cache_log_position;
    is_dml= false;
  }
  else
  {
    log_message(MY_ERROR_LEVEL, "We can only use one cache type at a "
                                "time on session %u", param->thread_id);
    plugin_stop_lock->unlock();
    DBUG_RETURN(1);
  }

  DBUG_ASSERT(cache_log->type == WRITE_CACHE);
  DBUG_PRINT("cache_log", ("thread_id: %u, trx_cache_log_position: %llu,"
                           " stmt_cache_log_position: %llu",
                           param->thread_id, trx_cache_log_position,
                           stmt_cache_log_position));

  /*
    Open group replication cache.
    Reuse the same cache on each session for improved performance.
  */
  // Protect against observer_trans_clear_io_cache_map().
  io_cache_map_clear_lock->rdlock();
  cache= observer_trans_get_io_cache(param->thread_id,
                                     param->cache_log_max_size);
  if (cache == NULL)
  {
    error= 1;
    goto err;
  }

  // Reinit binlog cache to read.
  if (reinit_cache(cache_log, READ_CACHE, 0))
  {
    log_message(MY_ERROR_LEVEL, "Failed to reinit binlog cache log for read "
                                "on session %u", param->thread_id);
    error= 1;
    goto err;
  }

  /*
    After this, cache_log should be reinit to old saved value when we
    are going out of the function scope.
  */
  reinit_cache_log_required= true;

  // Create transaction context.
  tcle= new Transaction_context_log_event(param->server_uuid,
                                          is_dml,
                                          param->thread_id,
                                          is_gtid_specified);
  if (!tcle->is_valid())
  {
    log_message(MY_ERROR_LEVEL,
                "Failed to create the context of the current "
                "transaction on session %u", param->thread_id);
    error= 1;
    goto err;
  }

  if (is_dml)
  {
    Transaction_write_set* write_set= get_transaction_write_set(param->thread_id);
    /*
      When GTID is specified we may have empty transactions, that is,
      a transaction may have not write set at all because it didn't
      change any data, it will just persist that GTID as applied.
    */
    if ((write_set == NULL) && (!is_gtid_specified))
    {
      log_message(MY_ERROR_LEVEL, "Failed to extract the set of items written "
                                  "during the execution of the current "
                                  "transaction on session %u", param->thread_id);
      error= 1;
      goto err;
    }

    if (write_set != NULL)
    {
      if (add_write_set(tcle, write_set))
      {
        cleanup_transaction_write_set(write_set);
        log_message(MY_ERROR_LEVEL, "Failed to gather the set of items written "
                                    "during the execution of the current "
                                    "transaction on session %u", param->thread_id);
        error= 1;
        goto err;
      }
      cleanup_transaction_write_set(write_set);
      DBUG_ASSERT(is_gtid_specified || (tcle->get_write_set()->size() > 0));
    }
  }

  // Write transaction context to group replication cache.
  tcle->write(cache);

  // Write Gtid log event to group replication cache.
  gle= new Gtid_log_event(param->server_id, is_dml, 0, 1, gtid_specification);
  gle->write(cache);

  // Reinit group replication cache to read.
  if (reinit_cache(cache, READ_CACHE, 0))
  {
    log_message(MY_ERROR_LEVEL, "Error while re-initializing an internal "
                                "cache, for read operations, on session %u",
                                param->thread_id);
    error= 1;
    goto err;
  }

  // Copy group replication cache to buffer.
  if (transaction_msg.append_cache(cache))
  {
    log_message(MY_ERROR_LEVEL, "Error while appending data to an internal "
                                "cache on session %u", param->thread_id);
    error= 1;
    goto err;
  }

  // Copy binlog cache content to buffer.
  if (transaction_msg.append_cache(cache_log))
  {
    log_message(MY_ERROR_LEVEL, "Error while writing binary log cache on "
                                "session %u", param->thread_id);
    error= 1;
    goto err;
  }


  DBUG_ASSERT(certification_latch != NULL);
  if (certification_latch->registerTicket(param->thread_id))
  {
    log_message(MY_ERROR_LEVEL, "Unable to register for getting notifications "
                                "regarding the outcome of the transaction on "
                                "session %u", param->thread_id);
    error= 1;
    goto err;
  }

#ifndef DBUG_OFF
  DBUG_EXECUTE_IF("test_basic_CRUD_operations_sql_service_interface",
                  {
                    DBUG_SET("-d,test_basic_CRUD_operations_sql_service_interface");
                    DBUG_ASSERT(!sql_command_check());
                  };);

  DBUG_EXECUTE_IF("group_replication_before_message_broadcast",
                  {
                    const char act[]= "now wait_for waiting";
                    DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
                  });
#endif

  //Broadcast the Transaction Message
  send_error= send_transaction_message(&transaction_msg);
  if (send_error == GCS_MESSAGE_TOO_BIG)
  {
    log_message(MY_ERROR_LEVEL, "Error broadcasting transaction to the group "
                                "on session %u. Message is too big.",
                                param->thread_id);
    error= 1;
    goto err;
  }
  else if (send_error == GCS_NOK)
  {
    log_message(MY_ERROR_LEVEL, "Error while broadcasting the transaction to "
                                "the group on session %u", param->thread_id);
    error= 1;
    goto err;
  }

  DBUG_ASSERT(certification_latch != NULL);
  if (certification_latch->waitTicket(param->thread_id))
  {
    log_message(MY_ERROR_LEVEL, "Error while waiting for conflict detection "
                                "procedure to finish on session %u",
                                param->thread_id);
    error= 1;
    goto err;
  }

err:
  // Reinit binlog cache to write (revert what we did).
  if (reinit_cache_log_required &&
      reinit_cache(cache_log, WRITE_CACHE, cache_log_position))
  {
    log_message(MY_ERROR_LEVEL, "Error while re-initializing an internal "
                                "cache, for write operations, on session %u",
                                param->thread_id);
  }
  io_cache_map_clear_lock->unlock();
  delete gle;
  delete tcle;

  if (error)
  {
    DBUG_ASSERT(certification_latch != NULL);
    // Release and remove certification latch ticket.
    certification_latch->releaseTicket(param->thread_id);
    certification_latch->waitTicket(param->thread_id);
  }

  plugin_stop_lock->unlock();
  DBUG_RETURN(error);
}

int group_replication_trans_before_rollback(Trans_param *param)
{
  DBUG_ENTER("group_replication_trans_before_rollback");
  DBUG_RETURN(0);
}

int group_replication_trans_after_commit(Trans_param *param)
{
  DBUG_ENTER("group_replication_trans_after_commit");
  DBUG_RETURN(0);
}

int group_replication_trans_after_rollback(Trans_param *param)
{
  DBUG_ENTER("group_replication_trans_after_rollback");
  DBUG_RETURN(0);
}

Trans_observer trans_observer = {
  sizeof(Trans_observer),

  group_replication_trans_before_dml,
  group_replication_trans_before_commit,
  group_replication_trans_before_rollback,
  group_replication_trans_after_commit,
  group_replication_trans_after_rollback,
};

/*
  Internal auxiliary functions.
*/

/*
  Reinit IO_cache type.

  @param[in] cache     cache
  @param[in] type      type to which cache will change
  @param[in] position  position to which cache will seek
*/
static bool reinit_cache(IO_CACHE *cache,
                         enum cache_type type,
                         my_off_t position)
{
  DBUG_ENTER("reinit_cache");

  /*
    Avoid call flush_io_cache() before reinit_io_cache() if
    temporary file does not exist.
    Call flush_io_cache() forces the creation of the cache
    temporary file, even when it does not exist.
  */
  if (READ_CACHE == type && cache->file != -1 && flush_io_cache(cache))
    DBUG_RETURN(true);

  if (reinit_io_cache(cache, type, position, 0, 0))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}

/*
  Get already initialized cache or create a new cache for
  this session.

  @param[in] thread_id   the session
  @param[in] cache_size  the cache size

  @return The cache or NULL on error
*/
IO_CACHE* observer_trans_get_io_cache(my_thread_id thread_id,
                                      ulonglong cache_size)
{
  DBUG_ENTER("observer_trans_get_io_cache");
  IO_CACHE *cache= NULL;
  IO_CACHE_map::iterator io_cache_map_it;

  io_cache_map_lock->rdlock();
  io_cache_map_it= io_cache_map.find(thread_id);
  io_cache_map_lock->unlock();
  if (io_cache_map_it == io_cache_map.end())
  {
    // Open IO_CACHE file
    cache= (IO_CACHE*) my_malloc(PSI_NOT_INSTRUMENTED,
                                 sizeof(IO_CACHE),
                                 MYF(MY_ZEROFILL));
    if (!cache || (!my_b_inited(cache) &&
                   open_cached_file(cache, mysql_tmpdir,
                                    "group_replication_trans_before_commit",
                                    cache_size, MYF(MY_WME))))
    {
      my_free(cache);
      log_message(MY_ERROR_LEVEL,
                  "Failed to create group replication commit cache on session %u",
                  thread_id);
      DBUG_RETURN(NULL);
    }

    // Add it to io_cache_map for use on future transaction on this session.
    io_cache_map_lock->wrlock();
    std::pair<IO_CACHE_map::iterator, bool> ret=
        io_cache_map.insert(std::pair<my_thread_id, IO_CACHE*>(thread_id,
                                                               cache));
    io_cache_map_lock->unlock();
    if (!ret.second)
    {
      log_message(MY_ERROR_LEVEL,
                  "Failed to store created group replication commit cache "
                  "on session %u", thread_id);
      DBUG_RETURN(NULL);
    }
  }
  else
  {
    // Reuse cache created previously.
    cache= io_cache_map_it->second;

    if (reinit_cache(cache, WRITE_CACHE, 0))
    {
      log_message(MY_ERROR_LEVEL,
                  "Failed to reinit group replication commit cache for write "
                  "on session %u", thread_id);
      DBUG_RETURN(NULL);
    }
  }

  DBUG_RETURN(cache);
}

enum enum_gcs_error send_transaction_message(Transaction_Message* msg)
{
  /*
    Ensure that group communication interfaces are initialized
    and ready to use, since plugin can leave the group on errors
    but continue to be active.
  */
  if (gcs_module == NULL || !gcs_module->is_initialized())
    return GCS_NOK;

  std::string group_name(group_name_var);
  Gcs_group_identifier group_id(group_name);

  Gcs_communication_interface *gcs_communication=
      gcs_module->get_communication_session(group_id);
  Gcs_control_interface *gcs_control=
      gcs_module->get_control_session(group_id);

  if (gcs_communication == NULL || gcs_control == NULL)
     return GCS_NOK;

  std::vector<uchar> transaction_message_data;
  msg->encode(&transaction_message_data);

  Gcs_member_identifier origin= gcs_control->get_local_member_identifier();
  Gcs_message to_send(origin,
                      new Gcs_message_data(0, transaction_message_data.size()));
  to_send.get_message_data().append_to_payload(&transaction_message_data.front(),
                                               transaction_message_data.size());

  return gcs_communication->send_message(to_send);
}

//Transaction Message implementation

Transaction_Message::Transaction_Message()
  :Plugin_gcs_message(CT_TRANSACTION_MESSAGE)
{
}

Transaction_Message::~Transaction_Message()
{
}

bool
Transaction_Message::append_cache(IO_CACHE *src)
{
  DBUG_ENTER("append_cache");
  DBUG_ASSERT(src->type == READ_CACHE);

  uchar *buffer= src->read_pos;
  size_t length= my_b_fill(src);
  if (src->file == -1)
  {
    // Read cache size directly when temporary file does not exist.
    length= my_b_bytes_in_cache(src);
  }

  while (length > 0 && !src->error)
  {
    data.insert(data.end(),
                buffer,
                buffer + length);

    src->read_pos= src->read_end;
    length= my_b_fill(src);
    buffer= src->read_pos;
  }

  DBUG_RETURN(src->error ? true : false);
}

void
Transaction_Message::encode_payload(std::vector<unsigned char>* buffer)
{
  DBUG_ENTER("Transaction_Message::encode_payload");

  encode_payload_item_type_and_length(buffer, PIT_TRANSACTION_DATA, data.size());
  buffer->insert(buffer->end(), data.begin(), data.end());

  DBUG_VOID_RETURN;
}

void
Transaction_Message::decode_payload(const unsigned char* buffer, size_t length)
{
  DBUG_ENTER("Transaction_Message::decode_payload");
  const unsigned char *slider= buffer;
  uint16 payload_item_type= 0;
  unsigned long long payload_item_length= 0;

  decode_payload_item_type_and_length(&slider,
                                      &payload_item_type,
                                      &payload_item_length);
  data.clear();
  data.insert(data.end(), slider, slider + payload_item_length);

  DBUG_VOID_RETURN;
}
