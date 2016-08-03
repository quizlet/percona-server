# ==== Purpose ====
#
# Shut down Group Replication initialized by
# include/group_replication.inc.
#
# ==== Usage ====
#
# [--let $rpl_debug= 1]
# [--let $rpl_group_replication= 1]
# --source include/group_replication_end.inc
#
# Parameters:
#   $rpl_debug
#     See include/rpl_init.inc
#
#   $rpl_group_replication
#     See include/rpl_end.inc
#

--let $include_filename= group_replication_end.inc
--source include/begin_include_file.inc

# Shut down Group Replication
--source include/rpl_end.inc

# Clean Group Replication XCom options on all servers to avoid false
# errors on MTR check testcase.
--source ../inc/group_replication_clear_configuration.inc

# Revert rpl debug to its previous value.
--let $show_rpl_debug_info= $_show_rpl_debug_info_previous

--connection default
# Do not restore connection, because we have disconnected it.
--let $skip_restore_connection= 1
--let $include_filename= group_replication_end.inc
--source include/end_include_file.inc
