/* Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.

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

#include <signal.h>
#include "applier.h"
#include <mysql/group_replication_priv.h>
#include "plugin_log.h"
#include "plugin.h"

#define APPLIER_GTID_CHECK_TIMEOUT_ERROR -1
#define APPLIER_RELAY_LOG_NOT_INITED -2

char applier_module_channel_name[] = "group_replication_applier";
bool applier_thread_is_exiting= false;

static void *launch_handler_thread(void* arg)
{
  Applier_module *handler= (Applier_module*) arg;
  handler->applier_thread_handle();
  return 0;
}

Applier_module::Applier_module()
  :applier_running(false), applier_aborted(false), applier_error(0), suspended(false),
   waiting_for_applier_suspension(false), incoming(NULL), pipeline(NULL),
   fde_evt(BINLOG_VERSION), stop_wait_timeout(LONG_TIMEOUT),
   applier_channel_observer(NULL)
{
  mysql_mutex_init(key_GR_LOCK_applier_module_run, &run_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_GR_COND_applier_module_run, &run_cond);
  mysql_mutex_init(key_GR_LOCK_applier_module_suspend, &suspend_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_GR_COND_applier_module_suspend, &suspend_cond);
  mysql_cond_init(key_GR_COND_applier_module_wait, &suspension_waiting_condition);
}

Applier_module::~Applier_module(){
  if (this->incoming)
  {
    while (!this->incoming->empty())
    {
      Packet *packet= NULL;
      this->incoming->pop(&packet);
      delete packet;
    }
    delete incoming;
  }
  delete applier_channel_observer;

  mysql_mutex_destroy(&run_lock);
  mysql_cond_destroy(&run_cond);
  mysql_mutex_destroy(&suspend_lock);
  mysql_cond_destroy(&suspend_cond);
  mysql_cond_destroy(&suspension_waiting_condition);
}

int
Applier_module::setup_applier_module(Handler_pipeline_type pipeline_type,
                                     bool reset_logs,
                                     ulong stop_timeout,
                                     rpl_sidno group_sidno,
                                     ulonglong gtid_assignment_block_size)
{
  DBUG_ENTER("Applier_module::setup_applier_module");

  int error= 0;

  //create the receiver queue
  this->incoming= new Synchronized_queue<Packet*>();

  stop_wait_timeout= stop_timeout;

  pipeline= NULL;

  if ( (error= get_pipeline(pipeline_type, &pipeline)) )
  {
    DBUG_RETURN(error);
  }

  reset_applier_logs= reset_logs;
  group_replication_sidno= group_sidno;
  this->gtid_assignment_block_size= gtid_assignment_block_size;

  DBUG_RETURN(error);
}

int
Applier_module::setup_pipeline_handlers()
{
  DBUG_ENTER("Applier_module::setup_pipeline_handlers");

  int error= 0;

  //Configure the applier handler trough a configuration action
  Handler_applier_configuration_action *applier_conf_action=
    new Handler_applier_configuration_action(applier_module_channel_name,
                                             reset_applier_logs,
                                             stop_wait_timeout,
                                             group_replication_sidno);

  error= pipeline->handle_action(applier_conf_action);
  delete applier_conf_action;
  if (error)
    DBUG_RETURN(error);

  Handler_certifier_configuration_action *cert_conf_action=
    new Handler_certifier_configuration_action(group_replication_sidno,
                                               gtid_assignment_block_size);

  error = pipeline->handle_action(cert_conf_action);

  delete cert_conf_action;

  DBUG_RETURN(error);
}

void
Applier_module::set_applier_thread_context()
{
  my_thread_init();
  THD *thd= new THD;
  thd->set_new_thread_id();
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  thd->get_protocol_classic()->init_net(0);
  thd->slave_thread= true;
  //TODO: See of the creation of a new type is desirable.
  thd->system_thread= SYSTEM_THREAD_SLAVE_IO;
  thd->security_context()->skip_grants();

  global_thd_manager_add_thd(thd);

  thd->init_for_queries();
  set_slave_thread_options(thd);
#ifndef _WIN32
  THD_STAGE_INFO(thd, stage_executing);
#endif
  applier_thd= thd;
}

void
Applier_module::clean_applier_thread_context()
{
  applier_thd->get_protocol_classic()->end_net();
  applier_thd->release_resources();
  THD_CHECK_SENTRY(applier_thd);
  global_thd_manager_remove_thd(applier_thd);
}

int
Applier_module::inject_event_into_pipeline(Pipeline_event* pevent,
                                           Continuation* cont)
{
  int error= 0;
  pipeline->handle_event(pevent, cont);

  if ((error= cont->wait()))
    log_message(MY_ERROR_LEVEL, "Error at event handling! Got error: %d", error);

  return error;
}



bool Applier_module::apply_action_packet(Action_packet *action_packet)
{
  enum_packet_action action= action_packet->packet_action;

  //packet used to break the queue blocking wait
  if (action == TERMINATION_PACKET)
  {
     return true;
  }
  //packet to signal the applier to suspend
  if (action == SUSPENSION_PACKET)
  {
    suspend_applier_module();
    return false;
  }
  return false;
}

int
Applier_module::apply_view_change_packet(View_change_packet *view_change_packet,
                                         Format_description_log_event *fde_evt,
                                         IO_CACHE *cache,
                                         Continuation *cont)
{
  int error= 0;

  Gtid_set *group_executed_set= NULL;
  Sid_map *sid_map= NULL;
  if (!view_change_packet->group_executed_set.empty())
  {
    sid_map= new Sid_map(NULL);
    group_executed_set= new Gtid_set(sid_map, NULL);
    if (intersect_group_executed_sets(view_change_packet->group_executed_set,
                                      group_executed_set))
    {
       log_message(MY_WARNING_LEVEL,
                   "Error when extracting group GTID execution information, "
                   "some recovery operations may face future issues");
       delete sid_map;
       delete group_executed_set;
       group_executed_set= NULL;
    }
  }

  if (group_executed_set != NULL)
  {
    if (get_certification_handler()->get_certifier()->
        set_group_stable_transactions_set(group_executed_set))
    {
      log_message(MY_WARNING_LEVEL,
                  "An error happened when trying to reduce the Certification "
                  " information size for transmission");
    }
    delete sid_map;
    delete group_executed_set;
  }

  View_change_log_event* view_change_event
      = new View_change_log_event((char*)view_change_packet->view_id.c_str());

  Pipeline_event* pevent= new Pipeline_event(view_change_event, fde_evt, cache);
  error= inject_event_into_pipeline(pevent, cont);
  delete pevent;

  return error;
}

int Applier_module::apply_data_packet(Data_packet *data_packet,
                                      Format_description_log_event *fde_evt,
                                      IO_CACHE *cache,
                                      Continuation *cont)
{
  int error= 0;
  uchar* payload= data_packet->payload;
  uchar* payload_end= data_packet->payload + data_packet->len;

  while ((payload != payload_end) && !error)
  {
    uint event_len= uint4korr(((uchar*)payload) + EVENT_LEN_OFFSET);

    Data_packet* new_packet= new Data_packet(payload, event_len);
    payload= payload + event_len;

    Pipeline_event* pevent= new Pipeline_event(new_packet, fde_evt, cache);
    error= inject_event_into_pipeline(pevent, cont);

    delete pevent;
  }

  return error;
}


int
Applier_module::applier_thread_handle()
{
  DBUG_ENTER("ApplierModule::applier_thread_handle()");

  //set the thread context
  set_applier_thread_context();

  Handler_THD_setup_action *thd_conf_action= NULL;
  Format_description_log_event* fde_evt= NULL;
  Continuation* cont= NULL;
  Packet *packet= NULL;
  bool loop_termination = false;

  IO_CACHE *cache= (IO_CACHE*) my_malloc(PSI_NOT_INSTRUMENTED,
                                         sizeof(IO_CACHE),
                                         MYF(MY_ZEROFILL));
  if (!cache || (!my_b_inited(cache) &&
                 open_cached_file(cache, mysql_tmpdir,
                                  "group_replication_pipeline_applier_cache",
                                  SHARED_EVENT_IO_CACHE_SIZE,
                                  MYF(MY_WME))))
  {
    my_free(cache);
    cache= NULL;
    log_message(MY_ERROR_LEVEL,
                "Failed to create group replication pipeline applier cache!");
    applier_error= 1;
    goto end;
  }

  applier_error= setup_pipeline_handlers();

  applier_channel_observer= new Applier_channel_state_observer();
  channel_observation_manager
      ->register_channel_observer(applier_channel_observer);

  if (!applier_error)
  {
    Pipeline_action *start_action = new Handler_start_action();
    applier_error= pipeline->handle_action(start_action);
    delete start_action;
  }

  if (applier_error)
  {
    goto end;
  }

  mysql_mutex_lock(&run_lock);
  applier_thread_is_exiting= false;
  applier_running= true;
  mysql_cond_broadcast(&run_cond);
  mysql_mutex_unlock(&run_lock);

  fde_evt= new Format_description_log_event(BINLOG_VERSION);
  cont= new Continuation();

  //Give the handlers access to the applier THD
  thd_conf_action= new Handler_THD_setup_action(applier_thd);
  applier_error= pipeline->handle_action(thd_conf_action);
  delete thd_conf_action;

  //applier main loop
  while (!applier_error && !loop_termination)
  {
    if (is_applier_thread_aborted())
      break;

    this->incoming->front(&packet); // blocking

    switch (packet->get_packet_type())
    {
      case ACTION_PACKET_TYPE:
          this->incoming->pop();
          loop_termination= apply_action_packet((Action_packet*)packet);
          break;
      case VIEW_CHANGE_PACKET_TYPE:
          applier_error= apply_view_change_packet((View_change_packet*)packet,
                                                  fde_evt, cache, cont);
          this->incoming->pop();
          break;
      case DATA_PACKET_TYPE:
          applier_error= apply_data_packet((Data_packet*)packet,
                                           fde_evt, cache, cont);
          //Remove from queue here, so the size only decreases after packet handling
         this->incoming->pop();
          break;
      default:
        DBUG_ASSERT(0);
    }

    delete packet;
  }
  delete fde_evt;
  delete cont;

end:

  //always remove the observer even the thread is no longer running
  channel_observation_manager
      ->unregister_channel_observer(applier_channel_observer);

  //only try to leave if the applier managed to start
  if (applier_error && applier_running)
    leave_group_on_failure();

  //Even on error cases, send a stop signal to all handlers that could be active
  Pipeline_action *stop_action= new Handler_stop_action();
  int local_applier_error= pipeline->handle_action(stop_action);
  delete stop_action;

  log_message(MY_INFORMATION_LEVEL, "The group replication applier thread"
                                    " was killed");

  DBUG_EXECUTE_IF("applier_thd_timeout",
                  {
                    const char act[]= "now wait_for signal.applier_continue";
                    DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
                  });

  if (cache != NULL)
  {
    close_cached_file(cache);
    my_free(cache);
  }

  clean_applier_thread_context();

  delete applier_thd;

  /*
    Don't overwrite applier_error when stop_applier_thread() doesn't return
    error. So applier_error which is also referred by main thread
    doesn't return true from initialize_applier_thread() when
    start_applier_thread() fails and stop_applier_thread() succeeds.
    Also use local var - local_applier_error, as the applier can be deleted
    before the thread returns.
  */
  if (local_applier_error)
    applier_error= local_applier_error;
  else
    local_applier_error= applier_error;

  mysql_mutex_lock(&run_lock);
  applier_running= false;
  mysql_cond_broadcast(&run_cond);
  mysql_mutex_unlock(&run_lock);

  my_thread_end();
  applier_thread_is_exiting= true;
  my_thread_exit(0);

  DBUG_RETURN(local_applier_error);
}

int
Applier_module::initialize_applier_thread()
{
  DBUG_ENTER("Applier_module::initialize_applier_thd");

  //avoid concurrency calls against stop invocations
  mysql_mutex_lock(&run_lock);

  applier_error= 0;

  if ((mysql_thread_create(key_GR_THD_applier_module_receiver,
                           &applier_pthd,
                           get_connection_attrib(),
                           launch_handler_thread,
                           (void*)this)))
  {
    mysql_mutex_unlock(&run_lock);
    DBUG_RETURN(1);
  }

  while (!applier_running && !applier_error)
  {
    DBUG_PRINT("sleep",("Waiting for applier thread to start"));
    mysql_cond_wait(&run_cond, &run_lock);
  }

  mysql_mutex_unlock(&run_lock);
  DBUG_RETURN(applier_error);
}

int
Applier_module::terminate_applier_pipeline()
{
  int error= 0;
  if (pipeline != NULL)
  {
    if ((error= pipeline->terminate_pipeline()))
    {
      log_message(MY_WARNING_LEVEL,
                  "The group replication applier pipeline was not properly"
                  " disposed. Check the error log for further info.");
    }
    //delete anyway, as we can't do much on error cases
    delete pipeline;
    pipeline= NULL;
  }
  return error;
}

int
Applier_module::terminate_applier_thread()
{
  DBUG_ENTER("Applier_module::terminate_applier_thread");

  /* This lock code needs to be re-written from scratch*/
  mysql_mutex_lock(&run_lock);

  applier_aborted= true;

  if (!applier_running)
  {
    goto delete_pipeline;
  }

  while (applier_running)
  {
    DBUG_PRINT("loop", ("killing group replication applier thread"));

    mysql_mutex_lock(&applier_thd->LOCK_thd_data);

    applier_thd->awake(THD::NOT_KILLED);
    mysql_mutex_unlock(&applier_thd->LOCK_thd_data);

    //before waiting for termination, signal the queue to unlock.
    add_termination_packet();

    //also awake the applier in case it is suspended
    awake_applier_module();

    /*
      There is a small chance that thread might miss the first
      alarm. To protect against it, resend the signal until it reacts
    */
    struct timespec abstime;
    set_timespec(&abstime, 2);
#ifndef DBUG_OFF
    int error=
#endif
      mysql_cond_timedwait(&run_cond, &run_lock, &abstime);
    if (stop_wait_timeout >= 2)
    {
      stop_wait_timeout= stop_wait_timeout - 2;
    }
    else if (applier_running) // quit waiting
    {
      mysql_mutex_unlock(&run_lock);
      DBUG_RETURN(1);
    }
    DBUG_ASSERT(error == ETIMEDOUT || error == 0);
  }

  DBUG_ASSERT(!applier_running);

delete_pipeline:

  //The thread ended properly so we can terminate the pipeline
  terminate_applier_pipeline();

  while (!applier_thread_is_exiting)
  {
    /* Check if applier thread is exiting per microsecond. */
    my_sleep(1);
  }

  /*
    Give applier thread one microsecond to exit completely after
    it set applier_thread_is_exiting to true.
  */
  my_sleep(1);

  mysql_mutex_unlock(&run_lock);

  DBUG_RETURN(0);
}

void Applier_module::inform_of_applier_stop(my_thread_id thread_id,
                                            bool aborted)
{
  DBUG_ENTER("Applier_module::inform_of_applier_stop");

  if (is_own_event_channel(thread_id) && aborted && applier_running )
  {
    log_message(MY_ERROR_LEVEL,
                "The applier thread execution was aborted."
                " Unable to process more transactions,"
                " this member will now leave the group.");

    applier_error= 1;

    //before waiting for termination, signal the queue to unlock.
    add_termination_packet();

    //also awake the applier in case it is suspended
    awake_applier_module();
  }

  DBUG_VOID_RETURN;
}

void Applier_module::leave_group_on_failure()
{
  DBUG_ENTER("Applier_module::leave_group_on_failure");

  log_message(MY_ERROR_LEVEL,
              "Fatal error during execution on the Applier process of "
              "Group Replication. The server will now leave the group.");

  Gcs_leave_coordinator::enum_leave_state state=
      gcs_leave_coordinator->group_replication_leave_group(false);

  std::stringstream ss;
  plugin_log_level log_severity= MY_WARNING_LEVEL;
  switch (state)
  {
    case Gcs_leave_coordinator::ERROR_WHEN_LEAVING:
      ss << "Unable to confirm whether the server has left the group or not. "
            "Check performance_schema.replication_group_members to check group membership information.";
      log_severity= MY_ERROR_LEVEL;

      //If you can't leave at least force the Error state.
      group_member_mgr->update_member_status(local_member_info->get_uuid(),
                                             Group_member_info::MEMBER_ERROR);
      break;
    case Gcs_leave_coordinator::ALREADY_LEAVING:
      ss << "Skipping leave operation: concurrent attempt to leave the group is on-going.";
      break;
    case Gcs_leave_coordinator::ALREADY_LEFT:
      ss << "Skipping leave operation: member already left the group.";
      break;
    case Gcs_leave_coordinator::NOW_LEAVING:
      goto end;
  }
  log_message(log_severity, ss.str().c_str());

end:
  //Unblock any blocked transactions. Make them rollback
  plugin_stop_lock->rdlock();
  //Stop any more transactions from waiting
  unblock_waiting_transactions();
  plugin_stop_lock->unlock();

  DBUG_VOID_RETURN;
}

int
Applier_module::wait_for_applier_complete_suspension(bool *abort_flag)
{
  int error= 0;

  mysql_mutex_lock(&suspend_lock);

  /*
   We use an external flag to avoid race conditions.
   A local flag could always lead to the scenario of
     wait_for_applier_complete_suspension()

   >> thread switch

     break_applier_suspension_wait()
       we_are_waiting = false;
       awake

   thread switch <<

      we_are_waiting = true;
      wait();
  */
  while (!suspended && !(*abort_flag))
  {
    mysql_cond_wait(&suspension_waiting_condition, &suspend_lock);
  }

  mysql_mutex_unlock(&suspend_lock);

  /**
    Wait for the applier execution of pre suspension events (blocking method)
    while(the wait method times out)
      wait()
  */
  error= APPLIER_GTID_CHECK_TIMEOUT_ERROR; //timeout error
  while (error == APPLIER_GTID_CHECK_TIMEOUT_ERROR && !(*abort_flag))
    error= wait_for_applier_event_execution(1); //blocking

  return (error == APPLIER_RELAY_LOG_NOT_INITED);
}

void
Applier_module::interrupt_applier_suspension_wait()
{
  mysql_mutex_lock(&suspend_lock);
  mysql_cond_broadcast(&suspension_waiting_condition);
  mysql_mutex_unlock(&suspend_lock);
}

int
Applier_module::wait_for_applier_event_execution(ulonglong timeout)
{
  Event_handler* event_applier= NULL;
  Event_handler::get_handler_by_role(pipeline, APPLIER, &event_applier);

  //Nothing to wait?
  if (event_applier == NULL)
    return 0;

  //The only event applying handler by now
  return ((Applier_handler*)event_applier)->wait_for_gtid_execution(timeout);
}

bool
Applier_module::is_own_event_channel(my_thread_id id){

  Event_handler* event_applier= NULL;
  Event_handler::get_handler_by_role(pipeline, APPLIER, &event_applier);

  //No applier exists so return false
  if (event_applier == NULL)
    return false;

  //The only event applying handler by now
  return ((Applier_handler*)event_applier)->is_own_event_applier(id);
}

Certification_handler* Applier_module::get_certification_handler(){

  Event_handler* event_applier= NULL;
  Event_handler::get_handler_by_role(pipeline, CERTIFIER, &event_applier);

  //The only certification handler for now
  return (Certification_handler*) event_applier;
}

int
Applier_module::intersect_group_executed_sets(std::vector<std::string>& gtid_sets,
                                              Gtid_set* output_set)
{
  Sid_map* sid_map= output_set->get_sid_map();

  std::vector<std::string>::iterator set_iterator;
  for (set_iterator= gtid_sets.begin();
       set_iterator!= gtid_sets.end();
       set_iterator++)
  {

    Gtid_set member_set(sid_map, NULL);
    Gtid_set intersection_result(sid_map, NULL);

    std::string exec_set_str= (*set_iterator);

    if (member_set.add_gtid_text(exec_set_str.c_str()) != RETURN_STATUS_OK)
    {
      return 1;
    }

    if (output_set->is_empty())
    {
      if (output_set->add_gtid_set(&member_set))
      {
      return 1;
      }
    }
    else
    {
      /*
        We have three sets:
          member_set:          the one sent from a given member;
          output_set:        the one that contains the intersection of
                               the computed sets until now;
          intersection_result: the intersection between set and
                               intersection_result.
        So we compute the intersection between member_set and output_set, and
        set that value to output_set to be used on the next intersection.
      */
      if (member_set.intersection(output_set, &intersection_result) != RETURN_STATUS_OK)
      {
        return 1;
      }

      output_set->clear();
      if (output_set->add_gtid_set(&intersection_result) != RETURN_STATUS_OK)
      {
        return 1;
      }
    }
  }

#if !defined(DBUG_OFF)
  char *executed_set_string;
  output_set->to_string(&executed_set_string);
  DBUG_PRINT("info", ("View change GTID information: output_set: %s",
             executed_set_string));
  my_free(executed_set_string);
#endif

  return 0;
}
