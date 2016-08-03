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

#include "gcs_leave_coordinator.h"
#include "plugin_psi.h"
#include "plugin.h"

Gcs_leave_coordinator::Gcs_leave_coordinator()
  : already_leaving(false), left(false)
{
  mysql_mutex_init(key_GR_LOCK_leave_coordinator, &leave_coordinator_mutex,
                   MY_MUTEX_INIT_FAST);
}

Gcs_leave_coordinator::~Gcs_leave_coordinator()
{
  mysql_mutex_destroy(&leave_coordinator_mutex);
}

void Gcs_leave_coordinator::reset_state()
{
  mysql_mutex_lock(&leave_coordinator_mutex);
  already_leaving= false;
  left= false;
  mysql_mutex_unlock(&leave_coordinator_mutex);
}

void Gcs_leave_coordinator::member_left()
{
  mysql_mutex_lock(&leave_coordinator_mutex);
  already_leaving= false;
  left= true;
  mysql_mutex_unlock(&leave_coordinator_mutex);

}

Gcs_leave_coordinator::enum_leave_state
Gcs_leave_coordinator::
group_replication_leave_group(bool group_replication_stopping,
                              Gcs_control_interface *gcs_control)
{
  DBUG_ENTER("Gcs_leave_coordinator::group_replication_leave_group");

  enum_leave_state state= ERROR_WHEN_LEAVING;

  mysql_mutex_lock(&leave_coordinator_mutex);
  if (left)
  {
    state= ALREADY_LEFT;
    goto err;
  }
  if (already_leaving)
  {
    state= ALREADY_LEAVING;
    goto err;
  }

  already_leaving= true;

  if (gcs_control == NULL)
  {
    /*
      Ensure that group communication interfaces are initialized
      and ready to use, since plugin can leave the group on errors
      but continue to be active.
    */
    if (gcs_module != NULL && gcs_module->is_initialized())
    {
      std::string group_name(group_name_var);
      Gcs_group_identifier group_id(group_name);
      gcs_control= gcs_module->get_control_session(group_id);
    }
    else
    {
      log_message(MY_ERROR_LEVEL,
                  "Error calling group communication interfaces while trying"
                   " to leave the group");
      goto err;
    }
  }

  if(group_replication_stopping)
    plugin_declare_group_replication_stopping();
  state= coordinator_leave_group(gcs_control);

err:
  if (state == ERROR_WHEN_LEAVING)
    already_leaving= false;

  mysql_mutex_unlock(&leave_coordinator_mutex);
  DBUG_RETURN(state);
}


Gcs_leave_coordinator::enum_leave_state
Gcs_leave_coordinator::coordinator_leave_group(Gcs_control_interface *gcs_control)
{
  DBUG_ENTER("Gcs_leave_coordinator::coordinator_leave_group");

  if (gcs_control != NULL)
  {
    if (gcs_control->leave())
    {
      DBUG_RETURN(ERROR_WHEN_LEAVING);
    }
  }
  else
  {
    log_message(MY_ERROR_LEVEL,
                "Error calling group communication interfaces while trying"
                " to leave the group");
    DBUG_RETURN(ERROR_WHEN_LEAVING);
  }

  DBUG_RETURN(NOW_LEAVING);
}
