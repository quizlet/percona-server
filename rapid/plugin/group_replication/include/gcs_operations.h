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

#ifndef GCS_OPERATIONS_INCLUDE
#define GCS_OPERATIONS_INCLUDE

/**
  Forces a new group membership, on which the excluded members
  will not receive a new view and will be blocked.

  @param members  The list of members, comma
                  separated. E.g., host1:port1,host2:port2

  @return Operation status
    @retval 0      OK
    @retval !=0    Error
*/
int force_members(const char* members);

#endif /* GCS_OPERATIONS_INCLUDE */
