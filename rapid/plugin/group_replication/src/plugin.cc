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

#include <sstream>

#include "observer_server_actions.h"
#include "observer_server_state.h"
#include "observer_trans.h"
#include "gcs_logger.h"
#include "gcs_operations.h"
#include "plugin.h"
#include "plugin_log.h"
#include "sql_service_gr_user.h"

using std::string;

/* Plugin generic fields */

static MYSQL_PLUGIN plugin_info_ptr;

//The plugin running flag and lock
static mysql_mutex_t plugin_running_mutex;
static bool group_replication_running;
bool wait_on_engine_initialization= false;
bool delay_gr_user_creation= false;
bool server_shutdown_status= false;
bool plugin_is_auto_starting= false;
bool plugin_is_being_unistalled= false;
bool plugin_is_being_stopped= false;

/* Plugin modules */
//The plugin applier
Applier_module *applier_module= NULL;
//The plugin recovery module
Recovery_module *recovery_module= NULL;
//The plugin group communication module
Gcs_interface *gcs_module= NULL;
Gcs_gr_logger_impl gcs_logger;
//The channel observation module
Channel_observation_manager *channel_observation_manager= NULL;
//Lock to check if the plugin is running or not.
Checkable_rwlock *plugin_stop_lock;
//Read mode handler
Read_mode_handler *read_mode_handler= NULL;
//Initialization thread for server starts
Delayed_initialization_thread *delayed_initialization_thread= NULL;
//The leave coordinator to control simultaneous leave requests
Gcs_leave_coordinator *gcs_leave_coordinator= NULL;

/* Group communication options */
const std::string gcs_engine("xcom");
char *local_address_var= NULL;
char *group_seeds_var= NULL;
char *force_members_var= NULL;
bool force_members_running= false;
static mysql_mutex_t force_members_running_mutex;
my_bool bootstrap_group_var= false;
ulong poll_spin_loops_var= 0;
ulong ssl_mode_var= 0;

const char* ssl_mode_values[]= {
  "DISABLED",
  "REQUIRED",
  "VERIFY_CA",
  "VERIFY_IDENTITY",
  (char*)0
};

#define IP_WHITELIST_STR_BUFFER_LENGTH 1024
char *ip_whitelist_var= NULL;
const char *IP_WHITELIST_DEFAULT= "AUTOMATIC";

//The plugin auto increment handler
Plugin_group_replication_auto_increment *auto_increment_handler= NULL;
Plugin_gcs_events_handler* events_handler= NULL;
Plugin_gcs_view_modification_notifier* view_change_notifier= NULL;

int gcs_communication_event_handle= 0;
int gcs_control_event_handler= 0;

/* Group management information */
Group_member_info_manager_interface *group_member_mgr= NULL;
Group_member_info* local_member_info= NULL;

/*Compatibility management*/
Compatibility_module* compatibility_mgr= NULL;

/* Plugin group related options */
const char *group_replication_plugin_name= "group_replication";
char *group_name_var= NULL;
my_bool start_group_replication_at_boot_var= true;
rpl_sidno group_sidno;

/* Applier module related */
bool known_server_reset;

//Recovery ssl options

// Option map entries that map the different SSL options to integer
static const int RECOVERY_SSL_CA_OPT= 1;
static const int RECOVERY_SSL_CAPATH_OPT= 2;
static const int RECOVERY_SSL_CERT_OPT= 3;
static const int RECOVERY_SSL_CIPHER_OPT= 4;
static const int RECOVERY_SSL_KEY_OPT= 5;
static const int RECOVERY_SSL_CRL_OPT= 6;
static const int RECOVERY_SSL_CRLPATH_OPT= 7;
//The option map <SSL var_name, SSL var code>
std::map<const char*, int> recovery_ssl_opt_map;

// SSL options
my_bool recovery_use_ssl_var= false;
char* recovery_ssl_ca_var= NULL;
char* recovery_ssl_capath_var= NULL;
char* recovery_ssl_cert_var= NULL;
char* recovery_ssl_cipher_var= NULL;
char* recovery_ssl_key_var= NULL;
char* recovery_ssl_crl_var= NULL;
char* recovery_ssl_crlpath_var= NULL;
my_bool recovery_ssl_verify_server_cert_var= false;
ulong  recovery_completion_policy_var;

ulong recovery_retry_count_var= 0;
ulong recovery_reconnect_interval_var= 0;

/* Write set extraction algorithm*/
int write_set_extraction_algorithm= HASH_ALGORITHM_OFF;

/* Generic components variables */
ulong components_stop_timeout_var= LONG_TIMEOUT;

/**
  The default value for auto_increment_increment is choosen taking into
  account the maximum usable values for each possible auto_increment_increment
  and what is a normal group expected size.
*/
#define DEFAULT_AUTO_INCREMENT_INCREMENT 7
#define MIN_AUTO_INCREMENT_INCREMENT 1
#define MAX_AUTO_INCREMENT_INCREMENT 65535
ulong auto_increment_increment_var= DEFAULT_AUTO_INCREMENT_INCREMENT;

/* compression options */
#define DEFAULT_COMPRESSION_THRESHOLD 0
#define MAX_COMPRESSION_THRESHOLD UINT_MAX32
#define MIN_COMPRESSION_THRESHOLD 0
ulong compression_threshold_var= DEFAULT_COMPRESSION_THRESHOLD;

/* GTID assignment block size options */
#define DEFAULT_GTID_ASSIGNMENT_BLOCK_SIZE 1
#define MIN_GTID_ASSIGNMENT_BLOCK_SIZE 1
#define MAX_GTID_ASSIGNMENT_BLOCK_SIZE MAX_GNO
ulonglong gtid_assignment_block_size_var= DEFAULT_GTID_ASSIGNMENT_BLOCK_SIZE;

/* Downgrade options */
char allow_local_lower_version_join_var= 0;

/* Allow errand transactions */
char allow_local_disjoint_gtids_join_var= 0;

/* Certification latch */
Wait_ticket<my_thread_id> *certification_latch;

/*
  Internal auxiliary functions signatures.
*/
static int check_group_name_string(const char *str,
                                   bool is_var_update= false);

static int check_recovery_ssl_string(const char *str, const char *var_name,
                                     bool is_var_update= false);

static int check_if_server_properly_configured();

static bool init_group_sidno();

static void initialize_ssl_option_map();

/*
  Auxiliary public functions.
*/
void *get_plugin_pointer()
{
  return plugin_info_ptr;
}

bool plugin_is_group_replication_running()
{
  return group_replication_running;
}

bool plugin_is_group_replication_stopping()
{
  return plugin_is_being_stopped;
}

void plugin_declare_group_replication_stopping()
{
  plugin_is_being_stopped= true;
}

int plugin_group_replication_set_retrieved_certification_info(void* info)
{
  return recovery_module->set_retrieved_cert_info(info);
}

int log_message(enum plugin_log_level level, const char *format, ...)
{
  va_list args;
  char buff[1024];

  va_start(args, format);
  my_vsnprintf(buff, sizeof(buff), format, args);
  va_end(args);
  return my_plugin_log_message(&plugin_info_ptr, level, buff);
}

/*
  Plugin interface.
*/
struct st_mysql_group_replication group_replication_descriptor=
{
  MYSQL_GROUP_REPLICATION_INTERFACE_VERSION,
  plugin_group_replication_start,
  plugin_group_replication_stop,
  plugin_is_group_replication_running,
  plugin_group_replication_set_retrieved_certification_info,
  plugin_get_connection_status,
  plugin_get_group_members,
  plugin_get_group_member_stats,
  plugin_get_group_members_number,
};

bool
plugin_get_connection_status(
    const GROUP_REPLICATION_CONNECTION_STATUS_CALLBACKS& callbacks)
{
  char* channel_name= applier_module_channel_name;

  return get_connection_status(callbacks, gcs_module, group_name_var,
                               channel_name,
                               plugin_is_group_replication_running());
}

bool
plugin_get_group_members(
    uint index, const GROUP_REPLICATION_GROUP_MEMBERS_CALLBACKS& callbacks)
{
  char* channel_name= applier_module_channel_name;

  return get_group_members_info(index, callbacks, group_member_mgr, gcs_module,
                                group_name_var, channel_name);
}

uint plugin_get_group_members_number()
{
  return group_member_mgr == NULL? 1 :
                                    (uint)group_member_mgr
                                                      ->get_number_of_members();
}

bool
plugin_get_group_member_stats(
    const GROUP_REPLICATION_GROUP_MEMBER_STATS_CALLBACKS& callbacks)
{
  char* channel_name= applier_module_channel_name;

  return get_group_member_stats(callbacks, group_member_mgr, applier_module,
                                gcs_module, group_name_var, channel_name);
}

int plugin_group_replication_start()
{
  Mutex_autolock auto_lock_mutex(&plugin_running_mutex);

  plugin_is_being_stopped= false;

  DBUG_ENTER("plugin_group_replication_start");
  int error= 0;

  if (plugin_is_group_replication_running())
    DBUG_RETURN(GROUP_REPLICATION_ALREADY_RUNNING);
  if (check_if_server_properly_configured())
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);
  if (check_group_name_string(group_name_var))
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);
  if (check_recovery_ssl_string(recovery_ssl_ca_var, "ssl_ca") ||
      check_recovery_ssl_string(recovery_ssl_capath_var, "ssl_capath") ||
      check_recovery_ssl_string(recovery_ssl_cert_var, "ssl_cert_pointer") ||
      check_recovery_ssl_string(recovery_ssl_cipher_var,
                                "ssl_cipher_pointer") ||
      check_recovery_ssl_string(recovery_ssl_key_var, "ssl_key_pointer") ||
      check_recovery_ssl_string(recovery_ssl_crl_var, "ssl_crl_pointer") ||
      check_recovery_ssl_string(recovery_ssl_crlpath_var,
                                "ssl_crlpath_pointer"))
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);
  if (!start_group_replication_at_boot_var &&
      !server_engine_initialized())
  {
    log_message(MY_ERROR_LEVEL,
                "Unable to start Group Replication. Replication applier "
                "infrastructure is not initialized since the server was "
                "started with server_id=0. Please, restart the server "
                "with server_id larger than 0.");
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);
  }
  if (force_members_var != NULL &&
      strlen(force_members_var) > 0)
  {
    log_message(MY_ERROR_LEVEL,
                "group_replication_force_members must be empty "
                "on group start. Current value: '%s'",
                force_members_var);
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);
  }
  if (init_group_sidno())
    DBUG_RETURN(GROUP_REPLICATION_CONFIGURATION_ERROR);

  /*
    Instantiate certification latch.
  */
  certification_latch= new Wait_ticket<my_thread_id>();
  read_mode_handler= new Read_mode_handler();
  Sql_service_command *sql_command_interface= new Sql_service_command();
  Sql_service_interface *sql_interface= NULL;
  int check_gr_user= 1;

  // GCS interface.
  DBUG_ASSERT(gcs_module == NULL);
  if ((gcs_module=
          Gcs_interface_factory::get_interface_implementation(
              gcs_engine)) == NULL)
  {
    log_message(MY_ERROR_LEVEL,
                "Failure in group communication engine '%s' initialization",
                gcs_engine.c_str());
    error= GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR;
    goto err;
  }

  // Set GCS logger.
  if (gcs_module->set_logger(&gcs_logger))
  {
    log_message(MY_ERROR_LEVEL,
                "Unable to set the group communication engine logger");
    error= GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR;
    goto err;
  }

  // GR delayed initialization.
  if(!server_engine_initialized())
  {
    wait_on_engine_initialization= true;
    plugin_is_auto_starting= false;
    delete sql_command_interface;

    DBUG_RETURN(0); //leave the decision for later
  }

  // Setup SQL service interface.
  if (sql_command_interface->establish_session_connection(false))
  {
    error =1;
    goto err;
  }

  sql_interface= sql_command_interface->get_sql_service_interface();
  check_gr_user= check_group_replication_user(false,sql_interface);

  if (check_gr_user < 0)
  {
    log_message(MY_ERROR_LEVEL,
                "Could not evaluate if the group replication user is present in"
                " the server");
    error =1;
    goto err;
  }
  if (!check_gr_user)
  {
    log_message(MY_WARNING_LEVEL,
                "The group replication user is not present in the server."
                " The user will be recreated, please do not remove it");

    if (create_group_replication_user(false, sql_interface))
    {
      log_message(MY_ERROR_LEVEL,
                  "It was not possible to create the group replication user used"
                  "by the plugin for internal operations.");
      error =1;
      goto err;
    }
  }

  if (sql_command_interface->set_interface_user(GROUPREPL_USER))
  {
    error =1;
    goto err;
  }

  // Setup GCS.
  if ((error= configure_group_communication(sql_interface)))
  {
    log_message(MY_ERROR_LEVEL,
                "Error on group communication engine initialization");
    goto err;
  }

  // Setup Group Member Manager.
  if ((error= configure_group_member_manager()))
    goto err;

  DBUG_EXECUTE_IF("group_replication_compatability_rule_error",
                  {
                    //Mark this member as being another version
                    Member_version other_version=
                        GROUP_REPLICATION_PLUGIN_VERSION + (0x000001);
                    compatibility_mgr->set_local_version(other_version);
                    Member_version
                        local_member_version(GROUP_REPLICATION_PLUGIN_VERSION);
                    //Add an incomparability with the real plugin version
                    compatibility_mgr->add_incompatibility(other_version,
                                                           local_member_version);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatability_higher_minor_version",
                  {
                    Member_version higher_version=
                        GROUP_REPLICATION_PLUGIN_VERSION + (0x000100);
                    compatibility_mgr->set_local_version(higher_version);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatability_higher_major_version",
                  {
                    Member_version higher_version=
                        GROUP_REPLICATION_PLUGIN_VERSION + (0x010000);
                    compatibility_mgr->set_local_version(higher_version);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatability_restore_version",
                  {
                    Member_version current_version=
                        GROUP_REPLICATION_PLUGIN_VERSION;
                    compatibility_mgr->set_local_version(current_version);
                  };);

  /*
   At this point in the code, set the super_read_only mode here on the
   server to protect recovery and version module of the Group Replication.
   This can only be done on START command though, on installs there are
   deadlock issues.
  */
  if (!plugin_is_auto_starting &&
      read_mode_handler->set_super_read_only_mode(sql_command_interface))
  {
    error =1;
    log_message(MY_ERROR_LEVEL,
                "Could not enable the server read only mode and guarantee a "
                "safe recovery execution");
    goto err;
  }

  if ((error= initialize_recovery_module()))
    goto err;

  //we can only start the applier if the log has been initialized
  if (configure_and_start_applier_module())
  {
    error= GROUP_REPLICATION_REPLICATION_APPLIER_INIT_ERROR;
    goto err;
  }

  DBUG_EXECUTE_IF("group_replication_read_mode_error",
                  { read_mode_handler->set_to_fail(); };);

  if ((error= start_group_communication()))
  {
    log_message(MY_ERROR_LEVEL,
                "Error on group communication engine start");
    goto err;
  }

  if (view_change_notifier->wait_for_view_modification())
  {
    if (!view_change_notifier->is_cancelled())
    {
      //Only log a error when a view modification was not cancelled.
      log_message(MY_ERROR_LEVEL,
                  "Timeout on wait for view after joining group");
    }
    error= view_change_notifier->get_error();
    goto err;
  }
  group_replication_running= true;

err:
  delete sql_command_interface;

  if (error)
  {
    leave_group();
    terminate_plugin_modules();
    if (certification_latch != NULL)
    {
      delete certification_latch;
      certification_latch= NULL;
    }
  }
  plugin_is_auto_starting= false;

  DBUG_RETURN(error);
}

int configure_group_member_manager()
{
  DBUG_ENTER("configure_group_member_manager");

  /*
    Ensure that group communication interfaces are initialized
    and ready to use, since plugin can leave the group on errors
    but continue to be active.
  */
  if (gcs_module == NULL || !gcs_module->is_initialized())
  {
    log_message(MY_ERROR_LEVEL, "Error calling group communication interfaces");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }

  //Retrieve local GCS information
  string group_id_str(group_name_var);
  Gcs_group_identifier group_id(group_id_str);
  Gcs_control_interface* gcs_control= gcs_module->get_control_session(group_id);

  if (gcs_control == NULL)
  {
    log_message(MY_ERROR_LEVEL, "Error calling group communication interfaces");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }

  //Configure Group Member Manager
  char *hostname, *uuid;
  uint port;
  get_server_host_port_uuid(&hostname, &port, &uuid);

  uint32 local_version= GROUP_REPLICATION_PLUGIN_VERSION;
  DBUG_EXECUTE_IF("group_replication_compatability_higher_patch_version",
                  {
                    local_version= GROUP_REPLICATION_PLUGIN_VERSION + (0x000001);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatability_higher_minor_version",
                  {
                    local_version= GROUP_REPLICATION_PLUGIN_VERSION + (0x000100);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatability_higher_major_version",
                  {
                    local_version= GROUP_REPLICATION_PLUGIN_VERSION + (0x010000);
                  };);
  DBUG_EXECUTE_IF("group_replication_compatibility_000400_version",
                  {
                    local_version= 0x000400;
                  };);
  DBUG_EXECUTE_IF("group_replication_compatibility_000500_version",
                  {
                    local_version= 0x000500;
                  };);
  Member_version local_member_plugin_version(local_version);
  delete local_member_info;
  local_member_info= new Group_member_info(hostname,
                                           port,
                                           uuid,
                                           write_set_extraction_algorithm,
                                           gcs_control->get_local_member_identifier(),
                                           Group_member_info::MEMBER_OFFLINE,
                                           local_member_plugin_version,
                                           gtid_assignment_block_size_var);

  //Create the membership info visible for the group
  delete group_member_mgr;
  group_member_mgr= new Group_member_info_manager(local_member_info);

  DBUG_RETURN(0);
}

int configure_compatibility_manager()
{
  if (compatibility_mgr != NULL)
  {
    delete compatibility_mgr;
  }

  Member_version local_member_version(GROUP_REPLICATION_PLUGIN_VERSION);

  compatibility_mgr= new Compatibility_module(local_member_version);

  /*
   If needed.. configure here static rules of incompatibility.

   Example:
     Member_version local_member_version(GROUP_REPLICATION_PLUGIN_VERSION);
     Member_version remote_member_version(0x005);
     compatibility_mgr->add_incompatibility(local_member_version,
                                            remote_member_version);
   */

   // This version is incompatible with version 0.4.0
   Member_version remote_member_version_000400(0x000400);
   compatibility_mgr->add_incompatibility(local_member_version,
                                          remote_member_version_000400);

   // This version is incompatible with version 0.5.0
   Member_version remote_member_version_000500(0x000500);
   compatibility_mgr->add_incompatibility(local_member_version,
                                          remote_member_version_000500);

  return 0;
}

int leave_group()
{
  string group_name(group_name_var);
  Gcs_group_identifier group_id(group_name);
  Gcs_control_interface *gcs_control= NULL;

  if (gcs_module != NULL && gcs_module->is_initialized())
  {
    gcs_control= gcs_module->get_control_session(group_id);
  }

  if (gcs_control != NULL)
  {
    if (gcs_control->belongs_to_group())
    {
      view_change_notifier->start_view_modification();

      Gcs_leave_coordinator::enum_leave_state state=
        gcs_leave_coordinator->group_replication_leave_group(true, gcs_control);

      std::stringstream ss;
      plugin_log_level log_severity= MY_WARNING_LEVEL;
      switch (state)
      {
        case Gcs_leave_coordinator::ERROR_WHEN_LEAVING:
          ss << "Unable to confirm whether the server has left the group or not. "
                "Check performance_schema.replication_group_members to check group membership information.";
          log_severity= MY_ERROR_LEVEL;
          break;
        case Gcs_leave_coordinator::ALREADY_LEAVING:
          ss << "Skipping leave operation: concurrent attempt to leave the group is on-going.";
          break;
        case Gcs_leave_coordinator::ALREADY_LEFT:
          ss << "Skipping leave operation: member already left the group.";
          break;
        case Gcs_leave_coordinator::NOW_LEAVING:
          goto bypass_message;
      }
      log_message(log_severity, ss.str().c_str());
bypass_message:
      //Wait anyway
      log_message(MY_INFORMATION_LEVEL, "Going to wait for view modification");
      if (view_change_notifier->wait_for_view_modification())
      {
        log_message(MY_WARNING_LEVEL,
                    "On shutdown there was a timeout receiving a view change. "
                    "This can lead to a possible inconsistent state. "
                    "Check the log for more details");
      }
    }
    else
    {
      /*
        Even when we do not belong to the group we invoke leave()
        to prevent the following situation:
         1) Server joins group;
         2) Server leaves group before receiving the view on which
            it joined the group.
        If we do not leave preemptively, the server will only leave
        the group when the communication layer failure detector
        detects that it left.
      */
      log_message(MY_INFORMATION_LEVEL,
                  "Requesting to leave the group despite of not "
                  "being a member");
      gcs_leave_coordinator->group_replication_leave_group(true, gcs_control);
    }
  }

  // Finalize GCS.
  if (gcs_module != NULL)
    gcs_module->finalize();
  Gcs_interface_factory::cleanup(gcs_engine);
  gcs_module= NULL;

  if (auto_increment_handler != NULL)
  {
    auto_increment_handler->reset_auto_increment_variables();
  }

  //Unregister callbacks and destroy notifiers
  gcs_control_event_handler= 0;
  gcs_communication_event_handle= 0;

  delete events_handler;
  events_handler= NULL;
  delete view_change_notifier;
  view_change_notifier= NULL;

  return 0;
}

int plugin_group_replication_stop()
{
  Mutex_autolock auto_lock_mutex(&plugin_running_mutex);
  DBUG_ENTER("plugin_group_replication_stop");

  plugin_stop_lock->wrlock();
  if (!plugin_is_group_replication_running())
  {
    plugin_stop_lock->unlock();
    DBUG_RETURN(0);
  }

  /* first leave all joined groups (currently one) */
  leave_group();

  group_member_mgr->update_member_status(local_member_info->get_uuid(),
                                         Group_member_info::MEMBER_OFFLINE);

  int error= terminate_plugin_modules();

  group_replication_running= false;
  plugin_stop_lock->unlock();

  DBUG_RETURN(error);
}

int terminate_plugin_modules()
{

  if (!server_shutdown_status && server_engine_initialized())
  {
    Sql_service_command *sql_command_interface= new Sql_service_command();
    if (sql_command_interface->establish_session_connection(false) ||
        sql_command_interface->set_interface_user(GROUPREPL_USER) ||
        read_mode_handler->reset_super_read_only_mode(sql_command_interface))
    {
      //Do not throw an error as the user can reset the read mode
      log_message(MY_WARNING_LEVEL,
                  "On plugin shutdown it was not possible to reset the server"
                  " read mode settings. Try to reset it manually.");
    }

    DBUG_EXECUTE_IF("group_replication_bypass_user_removal",
                    { plugin_is_being_unistalled= false; };);

    if (plugin_is_being_unistalled)
    {
      if (remove_group_replication_user(false,
                                        sql_command_interface->get_sql_service_interface()))
      {
        //Do not throw an error as the user can remove the user
        log_message(MY_WARNING_LEVEL,
                    "On plugin shutdown it was not possible to remove the user"
                    " associate to the plugin: " GROUPREPL_USER "."
                    " You can remove it manually if desired.");
      }
    }
    delete sql_command_interface;
  }

  delete read_mode_handler;

  if(terminate_recovery_module())
  {
    //Do not throw an error since recovery is not vital, but warn either way
    log_message(MY_WARNING_LEVEL,
                "On shutdown there was a timeout on the Group Replication "
                "recovery module termination. Check the log for more details");
  }

  DBUG_EXECUTE_IF("group_replication_after_recovery_module_terminated",
                 {
                   const char act[]= "now wait_for signal.termination_continue";
                   DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
                 });

  /*
    The applier is only shutdown after the communication layer to avoid
    messages being delivered in the current view, but not applied
  */
  int error= 0;
  if((error= terminate_applier_module()))
  {
    log_message(MY_ERROR_LEVEL,
                "On shutdown there was a timeout on the Group Replication"
                " applier termination.");
  }

  /*
    Destroy certification latch.
  */
  if (certification_latch != NULL)
  {
    delete certification_latch;
    certification_latch= NULL;
  }

  /*
    Clear server sessions open caches on transactions observer.
  */
  observer_trans_clear_io_cache_map();

  return error;
}

int plugin_group_replication_init(MYSQL_PLUGIN plugin_info)
{
  // Register all PSI keys at the time plugin init
#ifdef HAVE_PSI_INTERFACE
  register_all_group_replication_psi_keys();
#endif /* HAVE_PSI_INTERFACE */

  mysql_mutex_init(key_GR_LOCK_plugin_running, &plugin_running_mutex,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_GR_LOCK_force_members_running,
                   &force_members_running_mutex,
                   MY_MUTEX_INIT_FAST);

  plugin_stop_lock= new Checkable_rwlock(
#ifdef HAVE_PSI_INTERFACE
                                         key_GR_RWLOCK_plugin_stop
#endif /* HAVE_PSI_INTERFACE */
                                        );

  //Initialize transactions observer structures
  observer_trans_initialize();

  plugin_info_ptr= plugin_info;

  if (group_replication_init(group_replication_plugin_name))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure during Group Replication handler initialization");
    return 1;
  }

  if(register_server_state_observer(&server_state_observer,
                                    (void *)plugin_info_ptr))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when registering the server state observers");
    return 1;
  }

  if (register_trans_observer(&trans_observer, (void *)plugin_info_ptr))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when registering the transactions state observers");
    return 1;
  }

  if (register_binlog_transmit_observer(&binlog_transmit_observer,
                                        (void *)plugin_info_ptr))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when registering the binlog state observers");
    return 1;
  }

  //Initialize the recovery SSL option map
  initialize_ssl_option_map();

  //Initialize channel observation and auto increment handlers before start
  auto_increment_handler= new Plugin_group_replication_auto_increment();
  channel_observation_manager= new Channel_observation_manager(plugin_info);
  gcs_leave_coordinator= new Gcs_leave_coordinator();

  //Initialize the compatibility module before starting
  configure_compatibility_manager();

  //Create the group replication user and give it grants.
  if(server_engine_initialized())
  {
    if (create_group_replication_user(false))
    {
      log_message(MY_ERROR_LEVEL,
                  "It was not possible to create the group replication user used"
                  "by the plugin for internal operations.");
      return 1;
    }
  }
  else
  {
    delayed_initialization_thread= new Delayed_initialization_thread();
    if (delayed_initialization_thread->launch_initialization_thread())
    {
      log_message(MY_ERROR_LEVEL,
                  "It was not possible to guarantee the initialization of plugin"
                  " structures on server start");
      delete delayed_initialization_thread;
      delayed_initialization_thread= NULL;
      return 1;
    }
    delay_gr_user_creation= true;
  }

  plugin_is_auto_starting= start_group_replication_at_boot_var;
  if (start_group_replication_at_boot_var && group_replication_start())
  {
    log_message(MY_ERROR_LEVEL,
                "Unable to start Group Replication on boot");
  }

  return 0;
}

int plugin_group_replication_deinit(void *p)
{
  int observer_unregister_error= 0;

  plugin_is_being_unistalled= true;

  if (group_replication_cleanup())
    log_message(MY_ERROR_LEVEL,
                "Failure when cleaning Group Replication server state");

  if(group_member_mgr != NULL)
  {
    delete group_member_mgr;
    group_member_mgr= NULL;
  }

  if(local_member_info != NULL)
  {
    delete local_member_info;
    local_member_info= NULL;
  }

  if (compatibility_mgr != NULL)
  {
    delete compatibility_mgr;
    compatibility_mgr= NULL;
  }

  if (channel_observation_manager != NULL)
  {
    delete channel_observation_manager;
    channel_observation_manager= NULL;
  }

  if (unregister_server_state_observer(&server_state_observer, p))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when unregistering the server state observers");
    observer_unregister_error++;
  }

  if (unregister_trans_observer(&trans_observer, p))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when unregistering the transactions state observers");
    observer_unregister_error++;
  }

  if (unregister_binlog_transmit_observer(&binlog_transmit_observer, p))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure when unregistering the binlog state observers");
    observer_unregister_error++;
  }

  if (observer_unregister_error == 0)
    log_message(MY_INFORMATION_LEVEL,
                "All Group Replication server observers"
                " have been successfully unregistered");

  if (delayed_initialization_thread != NULL)
  {
    delay_gr_user_creation= false;
    wait_on_engine_initialization= false;
    delayed_initialization_thread->signal_thread_ready();
    delayed_initialization_thread->wait_for_initialization();
    delete delayed_initialization_thread;
    delayed_initialization_thread= NULL;
  }

  delete gcs_leave_coordinator;
  gcs_leave_coordinator= NULL;

  if(auto_increment_handler != NULL)
  {
    delete auto_increment_handler;
    auto_increment_handler= NULL;
  }

  mysql_mutex_destroy(&plugin_running_mutex);
  mysql_mutex_destroy(&force_members_running_mutex);

  delete plugin_stop_lock;
  plugin_stop_lock= NULL;

  //Terminate transactions observer structures
  observer_trans_terminate();

  return observer_unregister_error;
}

static bool init_group_sidno()
{
  DBUG_ENTER("init_group_sidno");
  rpl_sid group_sid;

  if (group_sid.parse(group_name_var) != RETURN_STATUS_OK)
  {
    log_message(MY_ERROR_LEVEL, "Unable to parse the group name.");
    DBUG_RETURN(true);
  }

  group_sidno = get_sidno_from_global_sid_map(group_sid);
  if (group_sidno <= 0)
  {
    log_message(MY_ERROR_LEVEL, "Unable to generate the sidno for the group.");
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

void declare_plugin_running()
{
  group_replication_running= true;
}

int configure_and_start_applier_module()
{
  DBUG_ENTER("configure_and_start_applier_module");

  int error= 0;

  //The applier did not stop properly or suffered a configuration error
  if (applier_module != NULL)
  {
    if ((error= applier_module->is_running())) //it is still running?
    {
      log_message(MY_ERROR_LEVEL,
                  "Cannot start the Group Replication applier as a previous "
                  "shutdown is still running: "
                  "The thread will stop once its task is complete.");
      DBUG_RETURN(error);
    }
    else
    {
      //clean a possible existent pipeline
      applier_module->terminate_applier_pipeline();
      //delete it and create from scratch
      delete applier_module;
    }
  }

  applier_module= new Applier_module();

  recovery_module->set_applier_module(applier_module);

  //For now, only defined pipelines are accepted.
  error=
    applier_module->setup_applier_module(STANDARD_GROUP_REPLICATION_PIPELINE,
                                         known_server_reset,
                                         components_stop_timeout_var,
                                         group_sidno,
                                         gtid_assignment_block_size_var);
  if (error)
  {
    //Delete the possible existing pipeline
    applier_module->terminate_applier_pipeline();
    delete applier_module;
    applier_module= NULL;
    DBUG_RETURN(error);
  }

  known_server_reset= false;

  if ((error= applier_module->initialize_applier_thread()))
  {
    log_message(MY_ERROR_LEVEL,
                "Unable to initialize the Group Replication applier module.");
    //terminate the applier_thread if running
    if (!applier_module->terminate_applier_thread())
    {
      delete applier_module;
      applier_module= NULL;
    }
  }
  else
    log_message(MY_INFORMATION_LEVEL,
                "Group Replication applier module successfully initialized!");

  DBUG_RETURN(error);
}

int terminate_applier_module()
{

  int error= 0;
  if (applier_module != NULL)
  {
    if (!applier_module->terminate_applier_thread()) //all goes fine
    {
      delete applier_module;
      applier_module= NULL;
    }
    else
    {
      error= GROUP_REPLICATION_APPLIER_STOP_TIMEOUT;
    }
  }
  return error;
}

int configure_group_communication(Sql_service_interface *sql_interface)
{
  DBUG_ENTER("configure_group_communication");

  if (gcs_module == NULL)
  {
    log_message(MY_ERROR_LEVEL, "Error calling group communication interfaces");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }

  // GCS interface parameters.
  Gcs_interface_parameters gcs_module_parameters;
  gcs_module_parameters.add_parameter("group_name",
                                      std::string(group_name_var));
  gcs_module_parameters.add_parameter("local_node",
                                      std::string(local_address_var));
  gcs_module_parameters.add_parameter("peer_nodes",
                                      std::string(group_seeds_var));
  const std::string bootstrap_group_string=
      bootstrap_group_var ? "true" : "false";
  gcs_module_parameters.add_parameter("bootstrap_group", bootstrap_group_string);
  std::stringstream poll_spin_loops_stream_buffer;
  poll_spin_loops_stream_buffer << poll_spin_loops_var;
  gcs_module_parameters.add_parameter("poll_spin_loops",
                                      poll_spin_loops_stream_buffer.str());

  // Compression parameter
  if (compression_threshold_var > 0)
  {
    std::stringstream ss;
    ss << compression_threshold_var;
    gcs_module_parameters.add_parameter("compression", std::string("on"));
    gcs_module_parameters.add_parameter("compression_threshold", ss.str());
  }
  else
  {
    gcs_module_parameters.add_parameter("compression", std::string("off"));
  }

  // SSL parameters.
  std::string ssl_mode(ssl_mode_values[ssl_mode_var]);
  if (ssl_mode_var > 0)
  {
    std::string query= "SELECT @@have_ssl='YES', @@ssl_key, @@ssl_cert, "
                       "@@ssl_ca, @@ssl_capath, @@ssl_cipher, @@ssl_crl, "
                       "@@ssl_crlpath, @@tls_version;";
    Sql_resultset rset;
    long query_error= sql_interface->execute_query(query, &rset);
    if (query_error || rset.get_rows() != 1 || rset.get_cols() != 9)
    {
      log_message(MY_ERROR_LEVEL,
                  "Unable to fetch SSL configuration from server, START "
                  "GROUP_REPLICATION will abort");
      DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
    }

    bool have_ssl = rset.getLong(0);
    std::string ssl_key(rset.getString(1));
    std::string ssl_cert(rset.getString(2));
    std::string ssl_ca(rset.getString(3));
    std::string ssl_capath(rset.getString(4));
    std::string ssl_cipher(rset.getString(5));
    std::string ssl_crl(rset.getString(6));
    std::string ssl_crlpath(rset.getString(7));
    std::string tls_version(rset.getString(8));

    // SSL support on server.
    if (have_ssl)
    {
      gcs_module_parameters.add_parameter("ssl_mode", ssl_mode);
      gcs_module_parameters.add_parameter("server_key_file", ssl_key);
      gcs_module_parameters.add_parameter("server_cert_file", ssl_cert);
      gcs_module_parameters.add_parameter("client_key_file", ssl_key);
      gcs_module_parameters.add_parameter("client_cert_file", ssl_cert);
      gcs_module_parameters.add_parameter("ca_file", ssl_ca);
      if (!ssl_capath.empty())
        gcs_module_parameters.add_parameter("ca_path", ssl_capath);
      gcs_module_parameters.add_parameter("cipher", ssl_cipher);
      gcs_module_parameters.add_parameter("tls_version", tls_version);

#if !defined(HAVE_YASSL)
      // YaSSL does not support CRL.
      if (!ssl_crl.empty())
        gcs_module_parameters.add_parameter("crl_file", ssl_crl);
      if (!ssl_crlpath.empty())
        gcs_module_parameters.add_parameter("crl_path", ssl_crlpath);
#endif

      log_message(MY_INFORMATION_LEVEL,
                  "Group communication SSL configuration: "
                  "group_replication_ssl_mode: \"%s\"; "
                  "server_key_file: \"%s\"; "
                  "server_cert_file: \"%s\"; "
                  "client_key_file: \"%s\"; "
                  "client_cert_file: \"%s\"; "
                  "ca_file: \"%s\"; "
                  "ca_path: \"%s\"; "
                  "cipher: \"%s\"; "
                  "tls_version: \"%s\"; "
                  "crl_file: \"%s\"; "
                  "crl_path: \"%s\"",
                  ssl_mode.c_str(), ssl_key.c_str(), ssl_cert.c_str(),
                  ssl_key.c_str(), ssl_cert.c_str(), ssl_ca.c_str(),
                  ssl_capath.c_str(), ssl_cipher.c_str(), tls_version.c_str(),
                  ssl_crl.c_str(), ssl_crlpath.c_str());
    }
    // No SSL support on server.
    else
    {
      log_message(MY_ERROR_LEVEL,
                  "MySQL server does not have SSL support and "
                  "group_replication_ssl_mode is \"%s\", START "
                  "GROUP_REPLICATION will abort", ssl_mode.c_str());
      DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
    }
  }
  // GCS SSL disabled.
  else
  {
    gcs_module_parameters.add_parameter("ssl_mode", ssl_mode);
    log_message(MY_INFORMATION_LEVEL,
                "Group communication SSL configuration: "
                "group_replication_ssl_mode: \"%s\"", ssl_mode.c_str());
  }

  if (ip_whitelist_var != NULL)
  {
    std::string v(ip_whitelist_var);
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);

    // if the user specified a list other than automatic
    // then we need to pass it to the GCS, otherwise we
    // do nothing and let GCS scan for the proper IPs
    if (v.find("automatic") == std::string::npos)
    {
      gcs_module_parameters.add_parameter("ip_whitelist",
                                          std::string(ip_whitelist_var));
    }
  }

  // Initialize GCS.
  if(gcs_module->initialize(gcs_module_parameters))
  {
    log_message(MY_ERROR_LEVEL,
                "Unable to initialize the group communication engine");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }
  log_message(MY_INFORMATION_LEVEL,
              "Initialized group communication with configuration: "
              "group_replication_group_name: \"%s\"; "
              "group_replication_local_address: \"%s\"; "
              "group_replication_group_seeds: \"%s\"; "
              "group_replication_bootstrap_group: %s; "
              "group_replication_poll_spin_loops: %lu; "
              "group_replication_compression_threshold: %lu; "
              "group_replication_ip_whitelist: \"%s\"",
              group_name_var, local_address_var, group_seeds_var,
              bootstrap_group_var ? "true" : "false",
              poll_spin_loops_var, compression_threshold_var,
              ip_whitelist_var);

  DBUG_RETURN(0);
}

int start_group_communication()
{
  DBUG_ENTER("start_group_communication");

  if (gcs_module == NULL || !gcs_module->is_initialized())
  {
    log_message(MY_ERROR_LEVEL, "Error calling group communication interfaces");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }

  string group_id_str(group_name_var);
  Gcs_group_identifier group_id(group_id_str);
  Gcs_control_interface* gcs_control= gcs_module->get_control_session(group_id);
  Gcs_communication_interface *gcs_communication=
      gcs_module->get_communication_session(group_id);

  if (gcs_control == NULL || gcs_communication == NULL)
  {
    log_message(MY_ERROR_LEVEL, "Error calling group communication interfaces");
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR);
  }

  if (auto_increment_handler != NULL)
  {
    auto_increment_handler->
      set_auto_increment_variables(auto_increment_increment_var,
                                   get_server_id());
  }

  view_change_notifier= new Plugin_gcs_view_modification_notifier();
  events_handler= new Plugin_gcs_events_handler(applier_module,
                                                recovery_module,
                                                view_change_notifier,
                                                compatibility_mgr,
                                                read_mode_handler);

  view_change_notifier->start_view_modification();

  //Set events handlers.
  gcs_control_event_handler= gcs_control->add_event_listener(*events_handler);
  gcs_communication_event_handle= gcs_communication->add_event_listener(*events_handler);

  /*
    Fake a GCS join error by not invoking join(), the
    view_change_notifier will error out and return a error on
    START GROUP_REPLICATION command.
  */
  DBUG_EXECUTE_IF("group_replication_inject_gcs_join_error",
                  { DBUG_RETURN(0); };);

  if (gcs_control->join())
  {
    DBUG_RETURN(GROUP_REPLICATION_COMMUNICATION_LAYER_JOIN_ERROR);
  }

  DBUG_RETURN(0);
}

int initialize_recovery_module()
{
  if (gcs_module == NULL || !gcs_module->is_initialized())
  {
    log_message(MY_ERROR_LEVEL,
                "Group communication interfaces were not properly initialized");
    return GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR;
  }

  recovery_module = new Recovery_module(applier_module,
                                        channel_observation_manager,
                                        components_stop_timeout_var);

  recovery_module->set_recovery_ssl_options(recovery_use_ssl_var,
                                            recovery_ssl_ca_var,
                                            recovery_ssl_capath_var,
                                            recovery_ssl_cert_var,
                                            recovery_ssl_cipher_var,
                                            recovery_ssl_key_var,
                                            recovery_ssl_crl_var,
                                            recovery_ssl_crlpath_var,
                                            recovery_ssl_verify_server_cert_var);
  recovery_module->
      set_recovery_completion_policy(
          (enum_recovery_completion_policies) recovery_completion_policy_var);
  recovery_module->set_recovery_donor_retry_count(recovery_retry_count_var);
  recovery_module->
      set_recovery_donor_reconnect_interval(recovery_reconnect_interval_var);

  return 0;
}

int terminate_recovery_module()
{
  int error= 0;
  if(recovery_module != NULL)
  {
    error = recovery_module->stop_recovery();
    delete recovery_module;
    recovery_module= NULL;
  }
  return error;
}

bool server_engine_initialized()
{
  //check if empty channel exists, i.e, the slave structures are initialized
  return channel_is_active("", CHANNEL_NO_THD);
}

void register_server_reset_master(){
  known_server_reset= true;
}

bool get_allow_local_lower_version_join()
{
  DBUG_ENTER("get_allow_local_lower_version_join");
  DBUG_RETURN(allow_local_lower_version_join_var);
}

bool get_allow_local_disjoint_gtids_join()
{
  DBUG_ENTER("get_allow_local_disjoint_gtids_join");
  DBUG_RETURN(allow_local_disjoint_gtids_join_var);
}

/*
  This method is used to accomplish the startup validations of the plugin
  regarding system configuration.

  It currently verifies:
  - Binlog enabled
  - Binlog checksum mode
  - Binlog format
  - Gtid mode
  - LOG_SLAVE_UPDATES

  @return If the operation succeed or failed
    @retval 0 in case of success
    @retval 1 in case of failure
 */
static int check_if_server_properly_configured()
{
  DBUG_ENTER("check_if_server_properly_configured");

  //Struct that holds startup and runtime requirements
  Trans_context_info startup_pre_reqs;

  get_server_startup_prerequirements(startup_pre_reqs, true);

  if(!startup_pre_reqs.binlog_enabled)
  {
    log_message(MY_ERROR_LEVEL, "Binlog must be enabled for Group Replication");
    DBUG_RETURN(1);
  }

  if(startup_pre_reqs.binlog_checksum_options != binary_log::BINLOG_CHECKSUM_ALG_OFF)
  {
    log_message(MY_ERROR_LEVEL, "binlog_checksum should be NONE for Group Replication");
    DBUG_RETURN(1);
  }

  if(startup_pre_reqs.binlog_format != BINLOG_FORMAT_ROW)
  {
    log_message(MY_ERROR_LEVEL, "Binlog format should be ROW for Group Replication");
    DBUG_RETURN(1);
  }

  if(startup_pre_reqs.gtid_mode != GTID_MODE_ON)
  {
    log_message(MY_ERROR_LEVEL, "Gtid mode should be ON for Group Replication");
    DBUG_RETURN(1);
  }

  if(startup_pre_reqs.log_slave_updates != true)
  {
    log_message(MY_ERROR_LEVEL,
                "LOG_SLAVE_UPDATES should be ON for Group Replication");
    DBUG_RETURN(1);
  }

  if(startup_pre_reqs.transaction_write_set_extraction ==
     HASH_ALGORITHM_OFF)
  {
    log_message(MY_ERROR_LEVEL,
                "Extraction of transaction write sets requires an hash algorithm "
                "configuration. Please, double check that the parameter "
                "transaction-write-set-extraction is set to a valid algorithm.");
    DBUG_RETURN(1);
  }
  else
  {
    write_set_extraction_algorithm=
       startup_pre_reqs.transaction_write_set_extraction;
  }

  if (startup_pre_reqs.mi_repository_type != 1) //INFO_REPOSITORY_TABLE
  {
    log_message(MY_ERROR_LEVEL, "Master info repository must be set to TABLE.");
    DBUG_RETURN(1);
  }

  if (startup_pre_reqs.rli_repository_type != 1) //INFO_REPOSITORY_TABLE
  {
    log_message(MY_ERROR_LEVEL, "Relay log info repository must be set to TABLE");
    DBUG_RETURN(1);
  }

  if (startup_pre_reqs.parallel_applier_workers > 0)
  {
    log_message(MY_ERROR_LEVEL,
                "Applier must be sequential on Group Replication, parameter "
                "slave-parallel-workers must be set to 0.");
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

static int check_group_name_string(const char *str, bool is_var_update)
{
  DBUG_ENTER("check_group_name_string");

  if (!str)
  {
    if(!is_var_update)
      log_message(MY_ERROR_LEVEL, "The group name option is mandatory");
    else
      my_message(ER_WRONG_VALUE_FOR_VAR,
                 "The group name option is mandatory",
                 MYF(0));
    DBUG_RETURN(1);
  }

  if (strlen(str) > UUID_LENGTH)
  {
    if(!is_var_update)
      log_message(MY_ERROR_LEVEL, "The group name '%s' is not a valid UUID, its"
                  " length is too big", str);
    else
      my_message(ER_WRONG_VALUE_FOR_VAR,
                 "The group name is not a valid UUID, its length is too big",
                 MYF(0));
    DBUG_RETURN(1);
  }

  if (!Uuid::is_valid(str))
  {
    if(!is_var_update)
      log_message(MY_ERROR_LEVEL, "The group name '%s' is not a valid UUID", str);
    else
      my_message(ER_WRONG_VALUE_FOR_VAR, "The group name is not a valid UUID",
                 MYF(0));
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

static int check_group_name(MYSQL_THD thd, SYS_VAR *var, void* save,
                            struct st_mysql_value *value)
{
  DBUG_ENTER("check_group_name");

  char buff[NAME_CHAR_LEN];
  const char *str;

  if (plugin_is_group_replication_running())
  {
    my_message(ER_GROUP_REPLICATION_RUNNING,
               "The group name cannot be changed when Group Replication is running",
               MYF(0));
    DBUG_RETURN(1);
  }

  (*(const char **) save)= NULL;

  int length= sizeof(buff);
  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  else
    DBUG_RETURN(1);

  if (check_group_name_string(str, true))
    DBUG_RETURN(1);

  *(const char**)save= str;

  DBUG_RETURN(0);
}

//Recovery module's module variable update/validate methods

static void update_recovery_retry_count(MYSQL_THD thd, SYS_VAR *var,
                                        void *var_ptr, const void *save)
{
  DBUG_ENTER("update_recovery_retry_count");

  (*(ulong*) var_ptr)= (*(ulong*) save);
  ulong in_val= *static_cast<const ulong*>(save);

  if (recovery_module != NULL)
  {
    recovery_module->set_recovery_donor_retry_count(in_val);
  }

  DBUG_VOID_RETURN;
}

static void update_recovery_reconnect_interval(MYSQL_THD thd, SYS_VAR *var,
                                               void *var_ptr, const void *save)
{
  DBUG_ENTER("update_recovery_reconnect_interval");

  (*(ulong*) var_ptr)= (*(ulong*) save);
  ulong in_val= *static_cast<const ulong*>(save);

  if (recovery_module != NULL)
  {
    recovery_module->
        set_recovery_donor_reconnect_interval(in_val);
  }

  DBUG_VOID_RETURN;
}

//Recovery SSL options

static void
update_ssl_use(MYSQL_THD thd, SYS_VAR *var,
               void *var_ptr, const void *save)
{
  DBUG_ENTER("update_ssl_use");

  bool use_ssl_val= *((my_bool *) save);
  (*(my_bool *) var_ptr)= (*(my_bool *) save);

  if (recovery_module != NULL)
  {
      recovery_module->set_recovery_use_ssl(use_ssl_val);
  }

  DBUG_VOID_RETURN;
}

static int check_recovery_ssl_string(const char *str, const char *var_name,
                                     bool is_var_update)
{
  DBUG_ENTER("check_recovery_ssl_string");

  if (strlen(str) > FN_REFLEN)
  {
    if(!is_var_update)
      log_message(MY_ERROR_LEVEL,
                  "The given value for recovery ssl option '%s' is invalid"
                  " as its length is beyond the limit", var_name);
    else
      my_message(ER_WRONG_VALUE_FOR_VAR,
                  "The given value for recovery ssl option is invalid"
                  " as its length is beyond the limit",
                 MYF(0));
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

static int check_recovery_ssl_option(MYSQL_THD thd, SYS_VAR *var, void* save,
                                     struct st_mysql_value *value)
{
  DBUG_ENTER("check_recovery_ssl_option");

  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str= NULL;

  (*(const char **) save)= NULL;

  int length= sizeof(buff);
  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  else
    DBUG_RETURN(1);

  if (str != NULL && check_recovery_ssl_string(str, var->name, true))
  {
    DBUG_RETURN(1);
  }

  *(const char**)save= str;

  DBUG_RETURN(0);
}

static void update_recovery_ssl_option(MYSQL_THD thd, SYS_VAR *var,
                                       void *var_ptr, const void *save)
{
  DBUG_ENTER("update_recovery_ssl_option");


  const char *new_option_val= *(const char**)save;
  (*(const char **) var_ptr)= (*(const char **) save);

  //According to the var name, get the operation code and act accordingly
  switch(recovery_ssl_opt_map[var->name])
  {
    case RECOVERY_SSL_CA_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_ca(new_option_val);
      break;
    case RECOVERY_SSL_CAPATH_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_capath(new_option_val);
      break;
    case RECOVERY_SSL_CERT_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_cert(new_option_val);
      break;
    case RECOVERY_SSL_CIPHER_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_cipher(new_option_val);
      break;
    case RECOVERY_SSL_KEY_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_key(new_option_val);
      break;
    case RECOVERY_SSL_CRL_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_crl(new_option_val);
      break;
    case RECOVERY_SSL_CRLPATH_OPT:
      if (recovery_module != NULL)
        recovery_module->set_recovery_ssl_crlpath(new_option_val);
      break;
    default:
      DBUG_ASSERT(0);
  }

  DBUG_VOID_RETURN;
}

static void
update_ssl_server_cert_verification(MYSQL_THD thd, SYS_VAR *var,
                                    void *var_ptr, const void *save)
{
  DBUG_ENTER("update_ssl_server_cert_verification");

  bool ssl_verify_server_cert= *((my_bool *) save);
  (*(my_bool *) var_ptr)= (*(my_bool *) save);

  if (recovery_module != NULL)
  {
    recovery_module->
        set_recovery_ssl_verify_server_cert(ssl_verify_server_cert);
  }

  DBUG_VOID_RETURN;
}

// Recovery threshold update method

static void
update_recovery_completion_policy(MYSQL_THD thd, SYS_VAR *var,
                                  void *var_ptr, const void *save)
{
  DBUG_ENTER("update_recovery_completion_policy");

  ulong in_val= *static_cast<const ulong*>(save);
  (*(ulong*) var_ptr)= (*(ulong*) save);

  if (recovery_module != NULL)
  {
    recovery_module->
        set_recovery_completion_policy(
            (enum_recovery_completion_policies)in_val);
  }

  DBUG_VOID_RETURN;
}

//Component timeout update method

static void update_component_timeout(MYSQL_THD thd, SYS_VAR *var,
                                     void *var_ptr, const void *save)
{
  DBUG_ENTER("update_component_timeout");

  ulong in_val= *static_cast<const ulong*>(save);
  (*(ulong*) var_ptr)= (*(ulong*) save);

  if (applier_module != NULL)
  {
    applier_module->set_stop_wait_timeout(in_val);
  }
  if (recovery_module != NULL)
  {
    recovery_module->set_stop_wait_timeout(in_val);
  }

  DBUG_VOID_RETURN;
}

static int check_auto_increment_increment(MYSQL_THD thd, SYS_VAR *var,
                                          void* save,
                                          struct st_mysql_value *value)
{
  DBUG_ENTER("check_auto_increment_increment");

  longlong in_val;
  value->val_int(value, &in_val);

  if (plugin_is_group_replication_running())
  {
    my_message(ER_GROUP_REPLICATION_RUNNING,
               "The group auto_increment_increment cannot be changed"
               " when Group Replication is running",
               MYF(0));
    DBUG_RETURN(1);
  }

  if (in_val > MAX_AUTO_INCREMENT_INCREMENT ||
      in_val < MIN_AUTO_INCREMENT_INCREMENT)
  {
    std::stringstream ss;
    ss << "The value " << in_val << " is not within the range of "
          "accepted values for the option "
          "group_replication_auto_increment_increment. The value "
          "must be between " << MIN_AUTO_INCREMENT_INCREMENT <<
          " and " << MAX_AUTO_INCREMENT_INCREMENT << " inclusive.";
    my_message(ER_WRONG_VALUE_FOR_VAR, ss.str().c_str(), MYF(0));
    DBUG_RETURN(1);
  }

  *(longlong*)save= in_val;
  DBUG_RETURN(0);
}

//Communication layer options.

static int check_ip_whitelist_preconditions(MYSQL_THD thd, SYS_VAR *var,
                                            void *save,
                                            struct st_mysql_value *value)
{
  DBUG_ENTER("check_ip_whitelist_preconditions");

  char buff[IP_WHITELIST_STR_BUFFER_LENGTH];
  const char *str;
  int length= sizeof(buff);

  if (plugin_is_group_replication_running())
  {
    my_message(ER_GROUP_REPLICATION_RUNNING,
               "The IP whitelist cannot be set while Group Replication "
               "is running", MYF(0));
    DBUG_RETURN(1);
  }

  (*(const char **) save)= NULL;

  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  else // NULL value is not allowed
    DBUG_RETURN(1);

  // remove trailing whitespaces
  std::string v(str);
  v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
  std::transform(v.begin(), v.end(), v.begin(), ::tolower);
  if (v.find("automatic") != std::string::npos && v.size() != 9)
  {
    my_message(ER_GROUP_REPLICATION_CONFIGURATION,
               "The IP whitelist is invalid. Make sure that AUTOMATIC "
               "when specifying \"AUTOMATIC\" the list contains no "
               "other values.", MYF(0));
    DBUG_RETURN(1);
  }

  *(const char**)save= str;

  DBUG_RETURN(0);
}

static int check_compression_threshold(MYSQL_THD thd, SYS_VAR *var,
                                       void* save,
                                       struct st_mysql_value *value)
{
  DBUG_ENTER("check_compression_threshold");

  longlong in_val;
  value->val_int(value, &in_val);

  if (plugin_is_group_replication_running())
  {
    my_message(ER_GROUP_REPLICATION_RUNNING,
               "The compression threshold cannot be set while "
               "Group Replication is running",
               MYF(0));
    DBUG_RETURN(1);
  }

  if (in_val > MAX_COMPRESSION_THRESHOLD || in_val < 0)
  {
    std::stringstream ss;
    ss << "The value " << in_val << " is not within the range of "
      "accepted values for the option compression_threshold!";
    my_message(ER_WRONG_VALUE_FOR_VAR, ss.str().c_str(), MYF(0));
    DBUG_RETURN(1);
  }

  *(longlong*)save= in_val;

  DBUG_RETURN(0);
}

static int check_force_members(MYSQL_THD thd, SYS_VAR *var,
                               void* save,
                               struct st_mysql_value *value)
{
  DBUG_ENTER("check_force_members");
  int error= 0;
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str= NULL;
  (*(const char **) save)= NULL;
  int length= 0;

  // Only one set force_members can run at a time.
  mysql_mutex_lock(&force_members_running_mutex);
  if (force_members_running)
  {
    log_message(MY_ERROR_LEVEL,
                "There is one group_replication_force_members "
                "operation already ongoing");
    mysql_mutex_unlock(&force_members_running_mutex);
    DBUG_RETURN(1);
  }
  force_members_running= true;
  mysql_mutex_unlock(&force_members_running_mutex);

#ifndef DBUG_OFF
  DBUG_EXECUTE_IF("group_replication_wait_on_check_force_members",
                  {
                    const char act[]= "now wait_for waiting";
                    DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
                  });
#endif

  // String validations.
  length= sizeof(buff);
  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  else
  {
    error= 1;
    goto end;
  }

  // If option value is empty string, just update its value.
  if (length == 0)
    goto update_value;

  if ((error= force_members(str)))
    goto end;

update_value:
  *(const char**)save= str;

end:
  mysql_mutex_lock(&force_members_running_mutex);
  force_members_running= false;
  mysql_mutex_unlock(&force_members_running_mutex);

  DBUG_RETURN(error);
}

static int check_gtid_assignment_block_size(MYSQL_THD thd, SYS_VAR *var,
                                            void* save,
                                            struct st_mysql_value *value)
{
  DBUG_ENTER("check_gtid_assignment_block_size");

  longlong in_val;
  value->val_int(value, &in_val);

  if (plugin_is_group_replication_running())
  {
    my_message(ER_GROUP_REPLICATION_RUNNING,
               "The GTID assignment block size cannot be set while "
               "Group Replication is running", MYF(0));
    DBUG_RETURN(1);
  }

  if (in_val > MAX_GTID_ASSIGNMENT_BLOCK_SIZE ||
      in_val < MIN_GTID_ASSIGNMENT_BLOCK_SIZE)
  {
    std::stringstream ss;
    ss << "The value " << in_val << " is not within the range of "
          "accepted values for the option gtid_assignment_block_size. "
          "The value must be between " << MIN_GTID_ASSIGNMENT_BLOCK_SIZE <<
          " and " << MAX_GTID_ASSIGNMENT_BLOCK_SIZE << " inclusive.";
    my_message(ER_WRONG_VALUE_FOR_VAR, ss.str().c_str(), MYF(0));
    DBUG_RETURN(1);
  }

  *(longlong*)save= in_val;

  DBUG_RETURN(0);
}

//Base plugin variables

static MYSQL_SYSVAR_STR(
  group_name,                                 /* name */
  group_name_var,                             /* var */
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,  /* optional var */
  "The group name",
  check_group_name,                           /* check func*/
  NULL,                                       /* update func*/
  NULL);                                      /* default*/

static MYSQL_SYSVAR_BOOL(
  start_on_boot,                              /* name */
  start_group_replication_at_boot_var,        /* var */
  PLUGIN_VAR_OPCMDARG,                        /* optional var */
  "Whether the server should start Group Replication or not during bootstrap.",
  NULL,                                       /* check func*/
  NULL,                                       /* update func*/
  1);                                         /* default*/

//GCS module variables

static MYSQL_SYSVAR_STR(
  local_address,                              /* name */
  local_address_var,                          /* var */
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,  /* optional var | malloc string*/
  "The local address, i.e., host:port.",
  NULL,                                       /* check func*/
  NULL,                                       /* update func*/
  "");                                        /* default*/

static MYSQL_SYSVAR_STR(
  group_seeds,                                /* name */
  group_seeds_var,                            /* var */
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,  /* optional var | malloc string*/
  "The list of group seeds, comma separated. E.g., host1:port1,host2:port2.",
  NULL,                                       /* check func*/
  NULL,                                       /* update func*/
  "");                                        /* default*/

static MYSQL_SYSVAR_STR(
  force_members,                              /* name */
  force_members_var,                          /* var */
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,  /* optional var | malloc string*/
  "The list of members, comma separated. E.g., host1:port1,host2:port2. "
  "This option is used to force a new group membership, on which the excluded "
  "members will not receive a new view and will be blocked. The DBA will need "
  "to kill the excluded servers.",
  check_force_members,                        /* check func*/
  NULL,                                       /* update func*/
  "");                                        /* default*/

static MYSQL_SYSVAR_BOOL(
  bootstrap_group,                            /* name */
  bootstrap_group_var,                        /* var */
  PLUGIN_VAR_OPCMDARG,                        /* optional var */
  "Specify if this member will bootstrap the group.",
  NULL,                                       /* check func. */
  NULL,                                       /* update func*/
  0                                           /* default */
);

static MYSQL_SYSVAR_ULONG(
  poll_spin_loops,                            /* name */
  poll_spin_loops_var,                        /* var */
  PLUGIN_VAR_OPCMDARG,                        /* optional var */
  "The number of times a thread waits for an communication engine "
  "mutex to be freed before the thread is suspended.",
  NULL,                                       /* check func. */
  NULL,                                       /* update func. */
  0,                                          /* default */
  0,                                          /* min */
  ~0UL,                                       /* max */
  0                                           /* block */
);

//Recovery module variables

static MYSQL_SYSVAR_ULONG(
  recovery_retry_count,              /* name */
  recovery_retry_count_var,          /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "The number of times that the joiner tries to connect to the available donors before giving up.",
  NULL,                              /* check func. */
  update_recovery_retry_count,       /* update func. */
  10,                                /* default */
  0,                                 /* min */
  LONG_TIMEOUT,                      /* max */
  0                                  /* block */
);

static MYSQL_SYSVAR_ULONG(
  recovery_reconnect_interval,        /* name */
  recovery_reconnect_interval_var,    /* var */
  PLUGIN_VAR_OPCMDARG,                /* optional var */
  "The sleep time between reconnection attempts when no donor was found in the group",
  NULL,                               /* check func. */
  update_recovery_reconnect_interval, /* update func. */
  60,                                 /* default */
  0,                                  /* min */
  LONG_TIMEOUT,                       /* max */
  0                                   /* block */
);

//SSL options for recovery

static MYSQL_SYSVAR_BOOL(
    recovery_use_ssl,              /* name */
    recovery_use_ssl_var,          /* var */
    PLUGIN_VAR_OPCMDARG,           /* optional var */
    "Whether SSL use should be obligatory during Group Replication recovery process.",
    NULL,                          /* check func*/
    update_ssl_use,                /* update func*/
    0);                            /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_ca,                 /* name */
    recovery_ssl_ca_var,             /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The path to a file that contains a list of trusted SSL certificate authorities.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_capath,             /* name */
    recovery_ssl_capath_var,         /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The path to a directory that contains trusted SSL certificate authority certificates.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_cert,               /* name */
    recovery_ssl_cert_var,           /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The name of the SSL certificate file to use for establishing a secure connection.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_cipher,             /* name */
    recovery_ssl_cipher_var,         /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "A list of permissible ciphers to use for SSL encryption.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_key,                /* name */
    recovery_ssl_key_var,            /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The name of the SSL key file to use for establishing a secure connection.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_crl,                /* name */
    recovery_ssl_crl_var,            /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The path to a file containing certificate revocation lists.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_STR(
    recovery_ssl_crlpath,            /* name */
    recovery_ssl_crlpath_var,        /* var */
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC, /* optional var | malloc string*/
    "The path to a directory that contains files containing certificate revocation lists.",
    check_recovery_ssl_option,       /* check func*/
    update_recovery_ssl_option,      /* update func*/
    "");                             /* default*/

static MYSQL_SYSVAR_BOOL(
    recovery_ssl_verify_server_cert,        /* name */
    recovery_ssl_verify_server_cert_var,    /* var */
    PLUGIN_VAR_OPCMDARG,                    /* optional var */
    "Make recovery check the server's Common Name value in the donor sent certificate.",
    NULL,                                   /* check func*/
    update_ssl_server_cert_verification,    /* update func*/
    0);                                     /* default*/

/** Initialize the ssl option map with variable names*/
static void initialize_ssl_option_map()
{
  recovery_ssl_opt_map.clear();
  st_mysql_sys_var* ssl_ca_var= MYSQL_SYSVAR(recovery_ssl_ca);
  recovery_ssl_opt_map[ssl_ca_var->name]= RECOVERY_SSL_CA_OPT;
  st_mysql_sys_var* ssl_capath_var= MYSQL_SYSVAR(recovery_ssl_capath);
  recovery_ssl_opt_map[ssl_capath_var->name]= RECOVERY_SSL_CAPATH_OPT;
  st_mysql_sys_var* ssl_cert_var= MYSQL_SYSVAR(recovery_ssl_cert);
  recovery_ssl_opt_map[ssl_cert_var->name]= RECOVERY_SSL_CERT_OPT;
  st_mysql_sys_var* ssl_cipher_var= MYSQL_SYSVAR(recovery_ssl_cipher);
  recovery_ssl_opt_map[ssl_cipher_var->name]= RECOVERY_SSL_CIPHER_OPT;
  st_mysql_sys_var* ssl_key_var= MYSQL_SYSVAR(recovery_ssl_key);
  recovery_ssl_opt_map[ssl_key_var->name]= RECOVERY_SSL_KEY_OPT;
  st_mysql_sys_var* ssl_crl_var=MYSQL_SYSVAR(recovery_ssl_crl);
  recovery_ssl_opt_map[ssl_crl_var->name]= RECOVERY_SSL_CRL_OPT;
  st_mysql_sys_var* ssl_crlpath_var=MYSQL_SYSVAR(recovery_ssl_crlpath);
  recovery_ssl_opt_map[ssl_crlpath_var->name]= RECOVERY_SSL_CRLPATH_OPT;
}

// Recovery threshold options

const char* recovery_policies[]= { "TRANSACTIONS_CERTIFIED",
                                   "TRANSACTIONS_APPLIED",
                                   (char *)0};

TYPELIB recovery_policies_typelib_t= {
  array_elements(recovery_policies) - 1,
  "recovery_policies_typelib_t",
  recovery_policies,
  NULL
};

static MYSQL_SYSVAR_ENUM(
   recovery_complete_at,                                 /* name */
   recovery_completion_policy_var,                       /* var */
   PLUGIN_VAR_OPCMDARG,                                  /* optional var */
   "Recovery policies when handling cached transactions after state transfer."
   "possible values are TRANSACTIONS_CERTIFIED or TRANSACTION_APPLIED", /* values */
   NULL,                                                 /* check func. */
   update_recovery_completion_policy,                    /* update func. */
   RECOVERY_POLICY_WAIT_EXECUTED,                        /* default */
   &recovery_policies_typelib_t);                        /* type lib */

//Generic timeout setting

static MYSQL_SYSVAR_ULONG(
  components_stop_timeout,                         /* name */
  components_stop_timeout_var,                     /* var */
  PLUGIN_VAR_OPCMDARG,                             /* optional var */
  "Timeout in seconds that the plugin waits for each of the components when shutting down.",
        NULL,                                      /* check func. */
  update_component_timeout,                        /* update func. */
  LONG_TIMEOUT,                                    /* default */
  2,                                               /* min */
  LONG_TIMEOUT,                                    /* max */
  0                                                /* block */
);

//Allow member downgrade

static MYSQL_SYSVAR_BOOL(
  allow_local_lower_version_join,        /* name */
  allow_local_lower_version_join_var,    /* var */
  PLUGIN_VAR_OPCMDARG,                   /* optional var */
  "Allow this server to join the group even if it has a lower plugin version than the group",
  NULL,                                  /* check func. */
  NULL,                                  /* update func*/
  0                                      /* default */
);

static MYSQL_SYSVAR_BOOL(
  allow_local_disjoint_gtids_join,       /* name */
  allow_local_disjoint_gtids_join_var,   /* var */
  PLUGIN_VAR_OPCMDARG,                   /* optional var */
  "Allow this server to join the group even if it has transactions not present in the group",
  NULL,                                  /* check func. */
  NULL,                                  /* update func*/
  0                                      /* default */
);

static MYSQL_SYSVAR_ULONG(
  auto_increment_increment,          /* name */
  auto_increment_increment_var,      /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "The group replication auto_increment_increment determines interval between successive column values",
  check_auto_increment_increment,    /* check func. */
  NULL,                              /* update by update_func_long func. */
  DEFAULT_AUTO_INCREMENT_INCREMENT,  /* default */
  MIN_AUTO_INCREMENT_INCREMENT,      /* min */
  MAX_AUTO_INCREMENT_INCREMENT,      /* max */
  0                                  /* block */
);

static MYSQL_SYSVAR_ULONG(
  compression_threshold,             /* name */
  compression_threshold_var,         /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "The value in bytes above which (lz4) compression is "
  "enforced. When set to zero, deactivates compression. "
  "Default: 0.",
  check_compression_threshold,       /* check func. */
  NULL,                              /* update func. */
  DEFAULT_COMPRESSION_THRESHOLD,     /* default */
  MIN_COMPRESSION_THRESHOLD,         /* min */
  MAX_COMPRESSION_THRESHOLD,         /* max */
  0                                  /* block */
);

static MYSQL_SYSVAR_ULONGLONG(
  gtid_assignment_block_size,        /* name */
  gtid_assignment_block_size_var,    /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "The number of consecutive GTIDs that are reserved to each "
  "member. Each member will consume its blocks and reserve "
  "more when needed. Default: 1.",
  check_gtid_assignment_block_size,  /* check func. */
  NULL,                              /* update func. */
  DEFAULT_GTID_ASSIGNMENT_BLOCK_SIZE,/* default */
  MIN_GTID_ASSIGNMENT_BLOCK_SIZE,    /* min */
  MAX_GTID_ASSIGNMENT_BLOCK_SIZE,    /* max */
  0                                  /* block */
);

TYPELIB ssl_mode_values_typelib_t= {
  array_elements(ssl_mode_values) - 1,
  "ssl_mode_values_typelib_t",
  ssl_mode_values,
  NULL
};

static MYSQL_SYSVAR_ENUM(
  ssl_mode,                          /* name */
  ssl_mode_var,                      /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "Specifies the security state of the connection between Group "
  "Replication members. Default: DISABLED",
  NULL,                              /* check func. */
  NULL,                              /* update func. */
  0,                                 /* default */
  &ssl_mode_values_typelib_t         /* type lib */
);

static MYSQL_SYSVAR_STR(
  ip_whitelist,                             /* name */
  ip_whitelist_var,                         /* var */
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,  /* optional var | malloc string*/
  "This option can be used to specify which members "
  "are allowed to connect to this member. The input "
  "takes the form of a comma separated list of IPv4 "
  "addresses or subnet CIDR notation. For example: "
  "192.168.1.0/24,10.0.0.1. In addition, the user can "
  "also set as input the value 'AUTOMATIC', in which case "
  "active interfaces on the host will be scanned and "
  "those with addresses on private subnetworks will be "
  "automatically added to the IP whitelist. The address "
  "127.0.0.1 is always added if not specified explicitly "
  "in the whitelist. Default: 'AUTOMATIC'.",
  check_ip_whitelist_preconditions,           /* check func*/
  NULL,                                       /* update func*/
  IP_WHITELIST_DEFAULT);                      /* default*/

static SYS_VAR* group_replication_system_vars[]= {
  MYSQL_SYSVAR(group_name),
  MYSQL_SYSVAR(start_on_boot),
  MYSQL_SYSVAR(local_address),
  MYSQL_SYSVAR(group_seeds),
  MYSQL_SYSVAR(force_members),
  MYSQL_SYSVAR(bootstrap_group),
  MYSQL_SYSVAR(poll_spin_loops),
  MYSQL_SYSVAR(recovery_retry_count),
  MYSQL_SYSVAR(recovery_use_ssl),
  MYSQL_SYSVAR(recovery_ssl_ca),
  MYSQL_SYSVAR(recovery_ssl_capath),
  MYSQL_SYSVAR(recovery_ssl_cert),
  MYSQL_SYSVAR(recovery_ssl_cipher),
  MYSQL_SYSVAR(recovery_ssl_key),
  MYSQL_SYSVAR(recovery_ssl_crl),
  MYSQL_SYSVAR(recovery_ssl_crlpath),
  MYSQL_SYSVAR(recovery_ssl_verify_server_cert),
  MYSQL_SYSVAR(recovery_complete_at),
  MYSQL_SYSVAR(recovery_reconnect_interval),
  MYSQL_SYSVAR(components_stop_timeout),
  MYSQL_SYSVAR(allow_local_lower_version_join),
  MYSQL_SYSVAR(allow_local_disjoint_gtids_join),
  MYSQL_SYSVAR(auto_increment_increment),
  MYSQL_SYSVAR(compression_threshold),
  MYSQL_SYSVAR(gtid_assignment_block_size),
  MYSQL_SYSVAR(ssl_mode),
  MYSQL_SYSVAR(ip_whitelist),
  NULL,
};

mysql_declare_plugin(group_replication_plugin)
{
  MYSQL_GROUP_REPLICATION_PLUGIN,
  &group_replication_descriptor,
  group_replication_plugin_name,
  "ORACLE",
  "Group Replication (0.8.0)",      /* Plugin name with full version*/
  PLUGIN_LICENSE_GPL,
  plugin_group_replication_init,    /* Plugin Init */
  plugin_group_replication_deinit,  /* Plugin Deinit */
  0x0008,                           /* Plugin Version: major.minor */
  NULL,                             /* status variables */
  group_replication_system_vars,    /* system variables */
  NULL,                             /* config options */
  0,                                /* flags */
}
mysql_declare_plugin_end;
