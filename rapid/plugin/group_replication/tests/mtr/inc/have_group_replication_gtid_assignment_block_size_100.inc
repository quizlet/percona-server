# ==== Purpose ====
#
# Ensure that the Group Replication GTID assignment block size is 100.
#
# ==== Usage ====
#
# --source ../inc/have_group_replication_gtid_assignment_block_size_100.inc

# Check server configuration.
--source ../inc/have_group_replication_plugin_base.inc

if (`SELECT @@GLOBAL.group_replication_gtid_assignment_block_size != 100`)
{
  --skip Test requires Group Replication with GTID assignment block size = 100
}
