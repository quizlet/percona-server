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

#ifndef GCS_LEAVE_COORDINATOR_INCLUDE
#define GCS_LEAVE_COORDINATOR_INCLUDE

#include "plugin_server_include.h"
#include <mysql/gcs/gcs_interface.h>

/**
  Class the centralizes all requests to leave a group inside the plugin.
  If a leave is already in place or the member left, the request simply returns.

  Note: methods in here only concern the leave request, not if the member
  really leaves the group or not.
*/
class Gcs_leave_coordinator
{
public:

  Gcs_leave_coordinator();
  virtual ~Gcs_leave_coordinator();

  /**
    @enum enum_leave_state

    This enumeration describes the return values when a process tries to leave
    a group.
  */
  enum enum_leave_state
  {
      /* The request was accepted, the member should now be leaving. */
      NOW_LEAVING,
      /* The member is already leaving, no point in retrying */
      ALREADY_LEAVING,
      /* The member already left */
      ALREADY_LEFT,
      /* There was an error when trying to leave */
      ERROR_WHEN_LEAVING
  };

  /**
    Ask the group communication layer to leave the group.

    @param group_replication_stopping  is the plugin stopping
    @param gcs_control                 the group communication control interface

    Note: This method only asks to leave, it does not know if request was
          successful

    @return the operation status
      @retval NOW_LEAVING         Request accepted, the member is leaving
      @retval ALREADY_LEAVING     The member is already leaving
      @retval ALREADY_LEFT        The member already left
      @retval ERROR_WHEN_LEAVING  An error happened when trying to leave
  */
  enum_leave_state
  group_replication_leave_group(bool group_replication_stopping,
                                Gcs_control_interface *gcs_control= NULL);
  /**
    Reset the information about the member being leaving or out of the group
  */
  void reset_state();
  /**
    Declare the member as being already out of the group
  */
  void member_left();
private:

  /**
    Private method to ask the group communication layer to leave the group.

    @param gcs_control           the group communication control interface
  */
  enum_leave_state coordinator_leave_group(Gcs_control_interface *gcs_control);

  /** Is the member already leaving*/
  bool already_leaving;
  /** Did the member already left*/
  bool left;

  //mutex variables
  mysql_mutex_t leave_coordinator_mutex;
};

#endif /* GCS_LEAVE_COORDINATOR_INCLUDE */
