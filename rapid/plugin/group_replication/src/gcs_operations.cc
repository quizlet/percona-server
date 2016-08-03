/* Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

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

#include "plugin.h"
#include "plugin_log.h"


int force_members(const char* members)
{
  DBUG_ENTER("force_members");

  if (gcs_module == NULL || !gcs_module->is_initialized())
  {
    log_message(MY_ERROR_LEVEL,
                "Member is OFFLINE, it is not possible to force a "
                "new group membership");
    DBUG_RETURN(1);
  }

  if (local_member_info->get_recovery_status() == Group_member_info::MEMBER_ONLINE)
  {
    std::string group_id_str(group_name_var);
    Gcs_group_identifier group_id(group_id_str);
    Gcs_group_management_interface* gcs_management=
        gcs_module->get_management_session(group_id);

    if (gcs_management == NULL)
    {
      log_message(MY_ERROR_LEVEL,
                  "Error calling group communication interfaces");
      DBUG_RETURN(1);
    }

    view_change_notifier->start_injected_view_modification();

    Gcs_interface_parameters gcs_module_parameters;
    gcs_module_parameters.add_parameter("peer_nodes",
                                        std::string(members));
    enum_gcs_error result=
        gcs_management->modify_configuration(gcs_module_parameters);
    if (result != GCS_OK)
    {
      log_message(MY_ERROR_LEVEL,
                  "Error setting group_replication_force_members "
                  "value '%s' on group communication interfaces", members);
      DBUG_RETURN(1);
    }
    log_message(MY_INFORMATION_LEVEL,
                "The group_replication_force_members value '%s' "
                "was set in the group communication interfaces", members);
    if (view_change_notifier->wait_for_view_modification())
    {
      log_message(MY_ERROR_LEVEL,
                  "Timeout on wait for view after setting "
                  "group_replication_force_members value '%s' "
                  "into group communication interfaces", members);
      DBUG_RETURN(1);
    }
  }
  else
  {
    log_message(MY_ERROR_LEVEL,
                "Member is not ONLINE, it is not possible to force a "
                "new group membership");
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}
