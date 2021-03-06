# Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include/sql_service)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/libmysqlgcs/include)

SET(GROUP_REPLICATION_SOURCES
  src/applier.cc
  src/auto_increment.cc
  src/certifier.cc
  src/channel_observation_manager.cc
  src/delayed_plugin_initialization.cc
  src/gcs_event_handlers.cc
  src/gcs_leave_coordinator.cc
  src/gcs_logger.cc
  src/gcs_operations.cc
  src/gcs_plugin_messages.cc
  src/gcs_view_modification_notifier.cc
  src/member_info.cc
  src/observer_server_actions.cc
  src/observer_server_channels.cc
  src/observer_server_state.cc
  src/observer_trans.cc
  src/pipeline_factory.cc
  src/plugin.cc
  src/plugin_psi.cc
  src/plugin_utils.cc
  src/ps_information.cc
  src/read_mode_handler.cc
  src/recovery.cc
  src/recovery_channel_state_observer.cc
  src/applier_channel_state_observer.cc
  src/recovery_message.cc
  src/recovery_state_transfer.cc
  src/replication_threads_api.cc
  src/member_version.cc
  src/compatibility_module.cc
  src/handlers/applier_handler.cc
  src/handlers/certification_handler.cc
  src/handlers/event_cataloger.cc
  src/sql_service/sql_resultset.cc
  src/sql_service/sql_service_context.cc
  src/sql_service/sql_service_context_base.cc
  src/sql_service/sql_service_gr_user.cc
  src/sql_service/sql_service_interface.cc
  src/sql_service/sql_command_test.cc
  src/sql_service/sql_service_command.cc
)

ADD_CONVENIENCE_LIBRARY(gr_unit_test_resource
  src/gcs_plugin_messages.cc
  src/member_info.cc
  src/member_version.cc
  src/compatibility_module.cc
)
SET_TARGET_PROPERTIES(gr_unit_test_resource PROPERTIES COMPILE_DEFINITIONS "DISABLE_ALL_PSI")

MYSQL_ADD_PLUGIN(group_replication ${GROUP_REPLICATION_SOURCES}
                 MODULE_ONLY MODULE_OUTPUT_NAME "group_replication")

SET(WITH_UNIT_TESTS OFF)
IF(CMAKE_BUILD_TYPE MATCHES "Debug")
  SET(WITH_DEBUG_OUTPUT ON)
ENDIF()
SET(SKIP_GCS_SHARED_LIB ON CACHE INTERNAL "Disables shared libraries")
SET(SKIP_INSTALL_MYSQL_GCS ON CACHE INTERNAL "Disables installation targets")
ADD_SUBDIRECTORY(libmysqlgcs)
GET_PROPERTY(LIBRARIES_TO_LINK_MYSQL_GCS GLOBAL PROPERTY LIBRARIES_TO_LINK_MYSQL_GCS)

TARGET_LINK_LIBRARIES(group_replication mysql_gcs)
IF(LIBRARIES_TO_LINK_MYSQL_GCS)
  TARGET_LINK_LIBRARIES(group_replication ${LIBRARIES_TO_LINK_MYSQL_GCS})
ENDIF()

IF(INSTALL_MYSQLTESTDIR)
  INSTALL(
    FILES tests/mtr/my.cnf
          tests/mtr/rpl_1slave_base.cnf
    DESTINATION "${INSTALL_MYSQLTESTDIR}/suite/group_replication"
    COMPONENT Test
  )
  INSTALL(
    DIRECTORY tests/mtr/t tests/mtr/r tests/mtr/inc
    DESTINATION "${INSTALL_MYSQLTESTDIR}/suite/group_replication"
    USE_SOURCE_PERMISSIONS
    COMPONENT Test
    PATTERN "collections" EXCLUDE
  )
ENDIF()
