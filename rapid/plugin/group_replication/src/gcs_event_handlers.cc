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

#include <algorithm>
#include <string>
#include <vector>

#include "gcs_event_handlers.h"
#include "plugin.h"
#include "sql_service_gr_user.h"

using std::vector;

Plugin_gcs_events_handler::
Plugin_gcs_events_handler(Applier_module_interface* applier_module,
                          Recovery_module* recovery_module,
                          Plugin_gcs_view_modification_notifier* vc_notifier,
                          Compatibility_module* compatibility_module,
                          Read_mode_handler* read_mode_handler)
: applier_module(applier_module), recovery_module(recovery_module),
  view_change_notifier(vc_notifier),
  compatibility_manager(compatibility_module),
  read_mode_handler(read_mode_handler)
{
  this->temporary_states= new std::set<Group_member_info*,
                                       Group_member_info_pointer_comparator>();
  this->joiner_compatibility_status= new st_compatibility_types(INCOMPATIBLE);

#ifndef DBUG_OFF
    set_number_of_members_on_view_changed_to_10= false;
    DBUG_EXECUTE_IF("group_replication_set_number_of_members_on_view_changed_to_10",
                    { set_number_of_members_on_view_changed_to_10= true; };);
#endif
}

Plugin_gcs_events_handler::~Plugin_gcs_events_handler()
{
  delete temporary_states;
  delete joiner_compatibility_status;
}

void
Plugin_gcs_events_handler::on_message_received(const Gcs_message& message) const
{
  Plugin_gcs_message::enum_cargo_type message_type=
      Plugin_gcs_message::get_cargo_type(
          message.get_message_data().get_payload());

  switch (message_type)
  {
  case Plugin_gcs_message::CT_TRANSACTION_MESSAGE:
    handle_transactional_message(message);
    break;

  case Plugin_gcs_message::CT_CERTIFICATION_MESSAGE:
    handle_certifier_message(message);
    break;

  case Plugin_gcs_message::CT_RECOVERY_MESSAGE:
    handle_recovery_message(message);
    break;

  default:
    DBUG_ASSERT(0);
  }
}

void
Plugin_gcs_events_handler::handle_transactional_message(const Gcs_message& message) const
{
  if ( (local_member_info->get_recovery_status() == Group_member_info::MEMBER_IN_RECOVERY ||
        local_member_info->get_recovery_status() == Group_member_info::MEMBER_ONLINE) &&
        this->applier_module)
  {
    const unsigned char* payload_data= NULL;
    size_t payload_size= 0;
    Plugin_gcs_message::get_first_payload_item_raw_data(
        message.get_message_data().get_payload(),
        &payload_data, &payload_size);

    this->applier_module->handle(payload_data, payload_size);
  }
  else
  {
    log_message(MY_ERROR_LEVEL,
                "Message received while the plugin is not ready,"
                " message discarded");
  }
}

void
Plugin_gcs_events_handler::handle_certifier_message(const Gcs_message& message) const
{
  if (this->applier_module == NULL)
  {
    log_message(MY_ERROR_LEVEL,
                "Message received without a proper group replication applier");
    return;
  }

  Certifier_interface *certifier=
      this->applier_module->get_certification_handler()->get_certifier();

  const unsigned char* payload_data= NULL;
  size_t payload_size= 0;
  Plugin_gcs_message::get_first_payload_item_raw_data(
      message.get_message_data().get_payload(),
      &payload_data, &payload_size);

  if (certifier->handle_certifier_data(payload_data,
                                       payload_size,
                                       message.get_origin().get_member_id()))
  {
    log_message(MY_ERROR_LEVEL, "Error processing message in Certifier");
  }
}

void
Plugin_gcs_events_handler::handle_recovery_message(const Gcs_message& message) const
{
  Recovery_message recovery_message(message.get_message_data().get_payload(),
                                    message.get_message_data().get_payload_length());

  std::string member_uuid= recovery_message.get_member_uuid();

  bool is_local= !member_uuid.compare(local_member_info->get_uuid());
  if(is_local)
  {
    // Only change member status if member is still on recovery.
    Group_member_info::Group_member_status member_status=
        local_member_info->get_recovery_status();
    if (member_status != Group_member_info::MEMBER_IN_RECOVERY)
    {
      log_message(MY_INFORMATION_LEVEL,
                  "This server was not declared online since it is on status %s",
                  Group_member_info::get_member_status_string(member_status));
      return;
    }

    log_message(MY_INFORMATION_LEVEL,
                "This server was declared online within the replication group");

    /**
      If the node is joining and not from a higher possible incompatible
      version then reset the read only mode in the server to 0
    */
    if (*joiner_compatibility_status != READ_COMPATIBLE)
    {
      Sql_service_command *sql_command_interface= new Sql_service_command();
      if (sql_command_interface->
	      establish_session_connection(true, get_plugin_pointer()) ||
        sql_command_interface->set_interface_user(GROUPREPL_USER) ||
	      read_mode_handler->reset_super_read_only_mode(sql_command_interface))
      {
        log_message(MY_WARNING_LEVEL,
                    "When declaring the plugin online it was not possible to "
                    "reset the server read mode settings. "
                    "Try to reset it manually.");
      }
      delete sql_command_interface;
    }

    // The member is declared as online upon receiving this message
    group_member_mgr->update_member_status(member_uuid,
                                           Group_member_info::MEMBER_ONLINE);
  }
  else
  {
    log_message(MY_INFORMATION_LEVEL,
                "Server %s was declared online within the replication group",
                member_uuid.c_str());

    // The member is declared as online upon receiving this message
    // We need to run this before running update_recovery_process
    group_member_mgr->update_member_status(member_uuid,
                                           Group_member_info::MEMBER_ONLINE);

    if (local_member_info->get_recovery_status() ==
          Group_member_info::MEMBER_IN_RECOVERY)
    {
      /*
        Inform recovery of a possible new donor
      */
      recovery_module->update_recovery_process(false, false);
    }
  }
}

void
Plugin_gcs_events_handler::on_suspicions(const std::vector<Gcs_member_identifier>& members,
                                         const std::vector<Gcs_member_identifier>& unreachable) const
{
  if (members.empty() && unreachable.empty()) // nothing to do
    return;

  DBUG_ASSERT(members.size() >= unreachable.size());

  std::vector<Gcs_member_identifier> tmp_unreachable(unreachable);
  std::vector<Gcs_member_identifier>::const_iterator mit;
  std::vector<Gcs_member_identifier>::iterator uit;

  if (!members.empty())
  {
    for (mit= members.begin(); mit != members.end(); mit ++)
    {
      Gcs_member_identifier member= *mit;
      Group_member_info* member_info=
        group_member_mgr->get_group_member_info_by_member_id(member);

      if (member_info == NULL) //Trying to update a non-existing member
        continue;

      uit= std::find(tmp_unreachable.begin(), tmp_unreachable.end(), member);
      if (uit != tmp_unreachable.end())
      {
        member_info->set_unreachable();

        // remove to not check again against this one
        tmp_unreachable.erase(uit);
      }
      else
      {
        member_info->set_reachable();
      }
    }
  }
}

void
Plugin_gcs_events_handler::on_view_changed(const Gcs_view& new_view,
                                           const Exchanged_data &exchanged_data)
                                           const
{
  bool is_leaving= is_member_on_vector(new_view.get_leaving_members(),
                                       local_member_info->get_gcs_member_id());

  bool is_joining= is_member_on_vector(new_view.get_joined_members(),
                                       local_member_info->get_gcs_member_id());

  if (is_joining)
    gcs_leave_coordinator->reset_state();
  if (is_leaving)
    gcs_leave_coordinator->member_left();

  //update the Group Manager with all the received states
  this->update_group_info_manager(new_view, exchanged_data, is_leaving);

  //Inform any interested handler that the view changed
  View_change_pipeline_action *vc_action=
    new View_change_pipeline_action(is_leaving);

  applier_module->handle_pipeline_action(vc_action);
  delete vc_action;

  //Update any recovery running process and handle state changes
  this->handle_leaving_members(new_view, is_joining, is_leaving);

  //Handle joining members
  this->handle_joining_members(new_view, is_joining, is_leaving);

  //Signal that the injected view was delivered
  if (view_change_notifier->is_injected_view_modification())
    view_change_notifier->end_view_modification();
}

void Plugin_gcs_events_handler::update_group_info_manager(const Gcs_view& new_view,
                                                          const Exchanged_data &exchanged_data,
                                                          bool is_leaving)
                                                          const
{
  //update the Group Manager with all the received states
  vector<Group_member_info*> to_update;

  if(!is_leaving)
  {
    //Process local state of exchanged data.
    process_local_exchanged_data(exchanged_data);

    to_update.insert(to_update.end(),
                     temporary_states->begin(),
                     temporary_states->end());

    //Clean-up members that are leaving
    vector<Gcs_member_identifier> leaving= new_view.get_leaving_members();
    vector<Gcs_member_identifier>::iterator left_it;
    vector<Group_member_info*>::iterator to_update_it;
    for(left_it= leaving.begin(); left_it != leaving.end(); left_it++)
    {
      for(to_update_it= to_update.begin();
          to_update_it != to_update.end();
          to_update_it++)
      {
        if( (*left_it) == (*to_update_it)->get_gcs_member_id() )
        {
          delete (*to_update_it);

          to_update.erase(to_update_it);
          break;
        }
      }
    }
  }
  group_member_mgr->update(&to_update);
  temporary_states->clear();
}

void Plugin_gcs_events_handler::handle_joining_members(const Gcs_view& new_view,
                                                       bool is_joining,
                                                       bool is_leaving)
                                                       const
{
  //nothing to do here
  int number_of_members= new_view.get_members().size();
  if (number_of_members == 0 || is_leaving)
  {
    return;
  }
  /**
   On the joining list there can be 2 types of members: online/recovering
   members coming from old views where this member was not present and new
   joining members that still have their status as offline.

   As so, for offline members, their state is changed to member_in_recovery.
  */
  update_member_status(new_view.get_joined_members(),
                       Group_member_info::MEMBER_IN_RECOVERY,
                       Group_member_info::MEMBER_OFFLINE,
                       Group_member_info::MEMBER_END);
  int number_of_joining_members= new_view.get_joined_members().size();

  /*
   If we are joining, 3 scenarios exist:
   1) We are incompatible with the group so we leave
   2) We are alone so we declare ourselves online
   3) We are in a group and recovery must happen
  */
  if (is_joining)
  {
    int error= 0;
    if ((error= check_group_compatibility(number_of_members)))
    {
      view_change_notifier->cancel_view_modification(error);
      return;
    }
    view_change_notifier->end_view_modification();

    log_message(MY_INFORMATION_LEVEL,
                "Starting group replication recovery with view_id %s",
                new_view.get_view_id().get_representation().c_str());

    /**
      Set the read mode if not set during start (auto-start)
    */
    if (!read_mode_handler->is_read_mode_active())
    {
      Sql_service_command *sql_command_interface= new Sql_service_command();
      if (sql_command_interface->
	      establish_session_connection(true, get_plugin_pointer()) ||
        sql_command_interface->set_interface_user(GROUPREPL_USER) ||
	      read_mode_handler->set_super_read_only_mode(sql_command_interface))
      {
        log_message(MY_ERROR_LEVEL,
                    "Error when securing the start process with super read mode. "
                    "The member will now exit the group.");
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
            delete sql_command_interface;
            return;
        }
        log_message(log_severity, ss.str().c_str());

        delete sql_command_interface;
        return;
      }
      delete sql_command_interface;
    }

    /**
      On the joining member log an error when group contains more members than
      auto_increment_increment variable.
    */
    ulong auto_increment_increment= get_auto_increment_increment();

    if (new_view.get_members().size() > auto_increment_increment)
    {
      log_message(MY_ERROR_LEVEL,
                  "Group contains %lu members which is greater then"
                  " group_replication_auto_increment_increment value of %lu."
                  " This can lead to an higher rate of transactional aborts.",
                  new_view.get_members().size(), auto_increment_increment);
    }

    /*
     During the view change, a suspension packet is sent to the applier module
     so all posterior transactions inbound are not applied, but queued, until
     the member finishes recovery.
    */
    applier_module->add_suspension_packet();

    /*
     Marking the view in the joiner since the incoming event from the donor
     is discarded in the Recovery process.
     */

    std::string view_id= new_view.get_view_id().get_representation();
    View_change_packet * view_change_packet= new View_change_packet(view_id);
    applier_module->add_view_change_packet(view_change_packet);

    /*
     Launch the recovery thread so we can receive missing data and the
     certification information needed to apply the transactions queued after
     this view change.

     Recovery receives a view id, as a means to identify logically on joiners
     and donors alike where this view change happened in the data. With that
     info we can then ask for the donor to give the member all the data until
     this point in the data, and the certification information for all the data
     that comes next.

     When alone, the server will go through Recovery to wait for the consumption
     of his applier relay log that may contain transactions from previous
     executions.
    */
    recovery_module->start_recovery(new_view.get_group_id().get_group_id(),
                                    new_view.get_view_id()
                                                      .get_representation());
  }
  else if (number_of_joining_members > 0)
  {
    log_message(MY_INFORMATION_LEVEL,
                "Marking group replication view change with view_id %s",
                new_view.get_view_id().get_representation().c_str());
    /**
     If not a joining member, all members should record on their own binlogs a
     marking event that identifies the frontier between the data the joining
     member was to receive and the data it should queue.
     The joining member can then wait for this event to know it was all the
     needed data.

     This packet will also pass in the certification process at this exact
     frontier giving us the opportunity to gather the necessary certification
     information to certify the transactions that will come after this view
     change. If selected as a donor, this info will also be sent to the joiner.

     Associated to this process, we collect and intersect the executed GTID sets
     of all ONLINE members so we can cut the certification info to gather and
     transmit to the minimum.
    */

    std::string view_id= new_view.get_view_id().get_representation();
    View_change_packet * view_change_packet= new View_change_packet(view_id);
    collect_members_executed_sets(new_view.get_joined_members(), view_change_packet);
    applier_module->add_view_change_packet(view_change_packet);
  }
}

void
Plugin_gcs_events_handler::handle_leaving_members(const Gcs_view& new_view,
                                                  bool is_joining,
                                                  bool is_leaving)
                                                  const
{
  Group_member_info::Group_member_status member_status=
      local_member_info->get_recovery_status();

  bool members_left= (new_view.get_leaving_members().size() > 0);

  //if the member is joining or not in recovery, no need to update the process
  if (!is_joining && member_status == Group_member_info::MEMBER_IN_RECOVERY)
  {
    /*
     This method has 2 purposes:
     If a donor leaves, recovery needs to switch donor
     If this member leaves, recovery needs to shutdown.
    */
    recovery_module->update_recovery_process(members_left, is_leaving);
  }

  if (is_leaving)
  {
    //If the plugin was not instructed to stop then it must have faced an error
    if (!plugin_is_group_replication_stopping())
    {
      group_member_mgr->update_member_status(local_member_info->get_uuid(),
                                             Group_member_info::MEMBER_ERROR);

      if (!read_mode_handler->is_read_mode_active())
      {
        bool threaded= (current_thd == NULL);
        Sql_service_command *sql_command_interface= new Sql_service_command();
        if (sql_command_interface->
          establish_session_connection(threaded, get_plugin_pointer()) ||
          sql_command_interface->set_interface_user(GROUPREPL_USER) ||
          read_mode_handler->set_super_read_only_mode(sql_command_interface))
        {
           log_message(MY_ERROR_LEVEL,
                       "It was not possible to enable the server read only mode"
                       " following a error scenario.");
        }
        else
        {
           log_message(MY_ERROR_LEVEL,
                       "To prevent errors, the server read only mode was enabled"
                       " following a plugin error.");
        }
        delete sql_command_interface;
      }
    }
  }

  if (members_left)
  {
    update_member_status(new_view.get_leaving_members(),
                         Group_member_info::MEMBER_OFFLINE,
                         Group_member_info::MEMBER_END,
                         Group_member_info::MEMBER_ERROR);
  }

  if (is_leaving)
  {
    view_change_notifier->end_view_modification();
  }
}

bool
Plugin_gcs_events_handler::
is_member_on_vector(const vector<Gcs_member_identifier>& members,
                    const Gcs_member_identifier& member_id)
                    const
{
  vector<Gcs_member_identifier>::const_iterator it;

  it= std::find(members.begin(), members.end(), member_id);

  return it != members.end();
}

int
Plugin_gcs_events_handler::
process_local_exchanged_data(const Exchanged_data &exchanged_data)
                             const
{
  /*
  For now, we are only carrying Group Member Info on Exchangeable data
  Since we are receiving the state from all Group members, one shall
  store it in a set to ensure that we don't have repetitions.

  All collected data will be given to Group Member Manager at view install
  time.
  */
  for (Exchanged_data::const_iterator exchanged_data_it= exchanged_data.begin();
       exchanged_data_it != exchanged_data.end();
       exchanged_data_it++)
  {
    const uchar* data= exchanged_data_it->second->get_payload();
    size_t length= exchanged_data_it->second->get_payload_length();
    Gcs_member_identifier* member_id= exchanged_data_it->first;
    if (data == NULL)
    {
      log_message(MY_ERROR_LEVEL, "Member with id '%s' didn't provide any data"
                                  " during the last group change. Group"
                                  " information can be outdated and lead to"
                                  " errors on recovery",
                                  member_id->get_member_id().c_str());
      continue;
    }

    //Process data provided by member.
    vector<Group_member_info*>* member_infos=
        group_member_mgr->decode(data, length);

    //This construct is here in order to deallocate memory of duplicates
    vector<Group_member_info*>::iterator member_infos_it;
    for(member_infos_it= member_infos->begin();
        member_infos_it != member_infos->end();
        member_infos_it++)
    {
      /*
        Accept only the information the member has about himself
        Information received about other members is probably outdated
      */
      if ((*member_infos_it)->get_gcs_member_id() == *member_id)
      {
        this->temporary_states->insert((*member_infos_it));
      }
      else
      {
        delete (*member_infos_it);
      }
    }

    member_infos->clear();
    delete member_infos;
  }

  return 0;
}

Gcs_message_data*
Plugin_gcs_events_handler::get_exchangeable_data() const
{
  std::string server_executed_gtids;
  std::string applier_retrieved_gtids;
  Replication_thread_api applier_channel("group_replication_applier");

  Sql_service_command *sql_command_interface= new Sql_service_command();

  if(sql_command_interface->
      establish_session_connection(true, get_plugin_pointer()) ||
     sql_command_interface->set_interface_user(GROUPREPL_USER)
    )
  {
    log_message(MY_WARNING_LEVEL,
                "Error when extracting information for group change. "
                "Operations and checks made to group joiners may be incomplete");
    goto sending;
  }

  if (sql_command_interface->get_server_gtid_executed(server_executed_gtids))
  {
    log_message(MY_WARNING_LEVEL,
                "Error when extracting this member GTID executed set. "
                "Operations and checks made to group joiners may be incomplete");
    goto sending;
  }
  if (applier_channel.get_retrieved_gtid_set(applier_retrieved_gtids))
  {
    log_message(MY_WARNING_LEVEL,
                "Error when extracting this member retrieved set for its applier. "
                "Operations and checks made to group joiners may be incomplete");
  }

  group_member_mgr->update_gtid_sets(local_member_info->get_uuid(),
                                     server_executed_gtids,
                                     applier_retrieved_gtids);
sending:

  delete sql_command_interface;

  std::vector<uchar> data;

  Group_member_info* local_member_copy= new Group_member_info(*local_member_info);
  Group_member_info_manager_message *group_info_message=
    new Group_member_info_manager_message(local_member_copy);
  group_info_message->encode(&data);
  delete group_info_message;

  Gcs_message_data* msg_data= new Gcs_message_data(0, data.size());
  msg_data->append_to_payload(&data.front(), data.size());

  return msg_data;
}

void
Plugin_gcs_events_handler::
update_member_status(const vector<Gcs_member_identifier>& members,
                     Group_member_info::Group_member_status status,
                     Group_member_info::Group_member_status old_status_equal_to,
                     Group_member_info::Group_member_status old_status_different_from)
                     const
{
  for (vector<Gcs_member_identifier>::const_iterator it= members.begin();
       it != members.end();
       ++it)
  {
    Gcs_member_identifier member = *it;
    Group_member_info* member_info=
        group_member_mgr->get_group_member_info_by_member_id(member);

    if (member_info == NULL)
    {
      //Trying to update a non-existing member
      continue;
    }

    // if  (the old_status_equal_to is not defined or
    //      the previous status is equal to old_status_equal_to)
    //    and
    //     (the old_status_different_from is not defined or
    //      the previous status is different from old_status_different_from)
    if ((old_status_equal_to == Group_member_info::MEMBER_END ||
        member_info->get_recovery_status() == old_status_equal_to) &&
       (old_status_different_from == Group_member_info::MEMBER_END ||
        member_info->get_recovery_status() != old_status_different_from))
    {
      group_member_mgr->update_member_status(member_info->get_uuid(), status);
    }
  }
}

/**
  Here we check:
  1) If the number of members was exceeded
  2) If member version is compatible with the group
  3) If the gtid_assignment_block_size is equal to the group
  4) If the hash algorithm used is equal to the group
  5) If the member has more known transactions than the group
*/
int
Plugin_gcs_events_handler::check_group_compatibility(int number_of_members) const
{
  /*
    Check if group size did reach the maximum number of members.
  */
#ifndef DBUG_OFF
  if (set_number_of_members_on_view_changed_to_10)
    number_of_members= 10;
#endif
  if (number_of_members > 9)
  {
    log_message(MY_ERROR_LEVEL,
                "The START GROUP_REPLICATION command failed since the group "
                "already has 9 members");
    return GROUP_REPLICATION_MAX_GROUP_SIZE;
  }

  /*
    Check if the member is compatible with the group.
    It can be incompatible because its major version is lower or a rule says it.
    If incompatible notify whoever is waiting for the view with an error, so
    the plugin exits the group.
  */
  *joiner_compatibility_status= COMPATIBLE;
  int group_data_compatibility= 0;
  if (number_of_members > 1)
  {
    *joiner_compatibility_status= check_version_compatibility_with_group();
    group_data_compatibility= compare_member_transaction_sets();
  }

  if (*joiner_compatibility_status == INCOMPATIBLE)
  {
    log_message(MY_ERROR_LEVEL,
                "Member version is incompatible with the group");
    return GROUP_REPLICATION_CONFIGURATION_ERROR;
  }

  /*
    All group members must have the same gtid_assignment_block_size
    and transaction-write-set-extraction value, if joiner has a
    different value it is not allowed to join.
  */
  if (number_of_members > 1 &&
      compare_member_option_compatibility())
  {
    return GROUP_REPLICATION_CONFIGURATION_ERROR;
  }

  /*
    Check that the joiner doesn't has more GTIDs than the rest of the group.
    All the executed and received transactions in the group are collected and
    merged into a GTID set and all joiner transactions must be contained in it.
  */
  if (group_data_compatibility)
  {
    if (group_data_compatibility > 0)
    {
      if(get_allow_local_disjoint_gtids_join())
      {
        log_message(MY_WARNING_LEVEL,
                    "The member contains transactions not present in the group. "
                    "It is only allowed to join due to "
                    "group_replication_allow_local_disjoint_gtids_join option");
      }
      else {
        log_message(MY_ERROR_LEVEL,
                    "The member contains transactions not present in the group. "
                    "The member will now exit the group.");
        log_message(MY_INFORMATION_LEVEL,
                    "To force this member into the group you can use the "
                    "group_replication_allow_local_disjoint_gtids_join option");
        return GROUP_REPLICATION_CONFIGURATION_ERROR;
      }
    }
    else //error
    {
      if(get_allow_local_disjoint_gtids_join())
      {
        log_message(MY_WARNING_LEVEL,
                    "It was not possible to assess if the member has more "
                    "transactions than the group. "
                    "It is only allowed to join due to the "
                    "group_replication_allow_local_disjoint_gtids_join option");
      }
      else {
        log_message(MY_ERROR_LEVEL,
                    "It was not possible to assess if the member has more "
                    "transactions than the group. "
                    "The member will now exit the group.");
        log_message(MY_INFORMATION_LEVEL,
                    "To force this member into the group you can use the "
                    "group_replication_allow_local_disjoint_gtids_join option");
        return GROUP_REPLICATION_CONFIGURATION_ERROR;
      }
    }
  }

  return 0;
}

Compatibility_type
Plugin_gcs_events_handler::check_version_compatibility_with_group() const
{
  bool override_lower_incompatibility= false;
  Compatibility_type compatibility_type= INCOMPATIBLE;
  bool read_compatible= false;

  std::vector<Group_member_info*> *all_members= group_member_mgr->get_all_members();
  std::vector<Group_member_info*>::iterator all_members_it;
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {
    Member_version member_version= (*all_members_it)->get_member_version();
    //if the member is not leaving and is incompatible
    if ((*all_members_it)->get_recovery_status()
            != Group_member_info::MEMBER_OFFLINE)
    {
      compatibility_type=
        compatibility_manager->check_local_incompatibility(member_version);

      if (compatibility_type == READ_COMPATIBLE)
      {
        read_compatible= true;
      }

      if (compatibility_type == INCOMPATIBLE)
      {
        break;
      }

      if (compatibility_type == INCOMPATIBLE_LOWER_VERSION)
      {
        if (get_allow_local_lower_version_join())
        {
          /*
            Despite between these two members the compatibility type
            is INCOMPATIBLE_LOWER_VERSION, when compared with others
            group members this server may be INCOMPATIBLE, so we need
            to test with all group members.
          */
          override_lower_incompatibility= true;
          compatibility_type= COMPATIBLE;
        }
        else
        {
          compatibility_type= INCOMPATIBLE;
          break;
        }
      }
    }
  }

  if (compatibility_type != INCOMPATIBLE && override_lower_incompatibility)
  {
    log_message(MY_INFORMATION_LEVEL,
                "Member version is lower than some group member, but since "
                "option 'group_replication_allow_local_lower_version_join' "
                "is enabled, member will be allowed to join");
  }

  if (read_compatible && compatibility_type != INCOMPATIBLE)
  {
    compatibility_type= READ_COMPATIBLE;
  }

  //clean the members
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {
    delete (*all_members_it);
  }
  delete all_members;

  return compatibility_type;
}

int Plugin_gcs_events_handler::compare_member_transaction_sets() const
{
  int result= 0;

  Sid_map local_sid_map(NULL);
  Sid_map group_sid_map(NULL);
  Gtid_set local_member_set(&local_sid_map, NULL);
  Gtid_set group_set(&group_sid_map, NULL);

  std::vector<Group_member_info*> *all_members= group_member_mgr->get_all_members();
  std::vector<Group_member_info*>::iterator all_members_it;
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++) {

    std::string member_exec_set_str= (*all_members_it)->get_gtid_executed();
    std::string applier_ret_set_str= (*all_members_it)->get_gtid_retrieved();
    if ((*all_members_it)->get_gcs_member_id() ==
            local_member_info->get_gcs_member_id())
    {
      if (local_member_set.
              add_gtid_text(member_exec_set_str.c_str()) != RETURN_STATUS_OK ||
          local_member_set.
              add_gtid_text(applier_ret_set_str.c_str()) != RETURN_STATUS_OK)
      {
        log_message(MY_ERROR_LEVEL,
                    "Error processing local GTID sets when comparing this member"
                    " transactions against the group");
        result= -1;
        goto cleaning;
      }
    }
    else
    {
      if (group_set.
              add_gtid_text(member_exec_set_str.c_str()) != RETURN_STATUS_OK ||
          group_set.
              add_gtid_text(applier_ret_set_str.c_str()) != RETURN_STATUS_OK)
      {
        log_message(MY_ERROR_LEVEL,
                    "Error processing group GTID sets when comparing this member"
                    " transactions with the group");
        result= -1;
        goto cleaning;
      }
    }

  }

  /*
    Here we only error out if the joiner set is bigger, i.e, if they are equal
    no error is returned.
    One could argue that if a joiner has the same transaction set as the group
    then something is wrong as the group also has transaction associated to
    previous view changes.
    To reject this cases cause however false negatives when members leave and
    quickly rejoin the group or when groups are started by add several nodes at
    once.
  */
  if (!local_member_set.is_subset(&group_set))
  {
    char *local_gtid_set_buf;
    local_member_set.to_string(&local_gtid_set_buf);
    char *group_gtid_set_buf;
    group_set.to_string(&group_gtid_set_buf);
    log_message(MY_ERROR_LEVEL,
                "This member has more executed transactions than those present"
                " in the group. Local transactions: %s > Group transactions: %s",
                local_gtid_set_buf, group_gtid_set_buf);
    my_free(local_gtid_set_buf);
    my_free(group_gtid_set_buf);
    result= 1;
  }

cleaning:

  //clean the members
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {
    delete (*all_members_it);
  }
  delete all_members;

  return result;
}

void Plugin_gcs_events_handler::
collect_members_executed_sets(const vector<Gcs_member_identifier> &joining_members,
                              View_change_packet *view_packet) const
{
  std::vector<Group_member_info*> *all_members= group_member_mgr->get_all_members();
  std::vector<Group_member_info*>::iterator all_members_it;
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {

    // Joining/Recovering members don't have valid GTID executed information
    if ((*all_members_it)->get_recovery_status() ==
            Group_member_info::MEMBER_IN_RECOVERY)
    {
      continue;
    }

    std::string exec_set_str= (*all_members_it)->get_gtid_executed();
    view_packet->group_executed_set.push_back(exec_set_str);
  }

  //clean the members
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {
    delete (*all_members_it);
  }
  delete all_members;
}

int
Plugin_gcs_events_handler::compare_member_option_compatibility() const
{
  int result= 0;

  std::vector<Group_member_info*> *all_members= group_member_mgr->get_all_members();
  std::vector<Group_member_info*>::iterator all_members_it;
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
  {
    if (local_member_info->get_gtid_assignment_block_size() !=
        (*all_members_it)->get_gtid_assignment_block_size())
    {
      result= 1;
      log_message(MY_ERROR_LEVEL,
                  "The member is configured with a "
                  "group_replication_gtid_assignment_block_size option "
                  "value '%llu' different from the group '%llu'. "
                  "The member will now exit the group.",
                  local_member_info->get_gtid_assignment_block_size(),
                  (*all_members_it)->get_gtid_assignment_block_size());
      goto cleaning;
    }

    if (local_member_info->get_write_set_extraction_algorithm() !=
       (*all_members_it)->get_write_set_extraction_algorithm())
    {
      result= 1;
      log_message(MY_ERROR_LEVEL,
                  "The member is configured with a "
                  "transaction-write-set-extraction option "
                  "value '%s' different from the group '%s'. "
                  "The member will now exit the group.",
                  get_write_set_algorithm_string(
                      local_member_info->get_write_set_extraction_algorithm()),
                  get_write_set_algorithm_string(
                      (*all_members_it)->get_write_set_extraction_algorithm()));
      goto cleaning;
    }
  }

cleaning:
  for (all_members_it= all_members->begin();
       all_members_it!= all_members->end();
       all_members_it++)
    delete (*all_members_it);
  delete all_members;

  return result;
}
