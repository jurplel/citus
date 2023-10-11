-- citus--12.0-1--12.1-1

-- bump version to 12.1-1

#include "udfs/citus_pause_node_within_txn/12.1-1.sql"
#include "udfs/citus_prepare_pg_upgrade/12.1-1.sql"
#include "udfs/citus_finish_pg_upgrade/12.1-1.sql"

#include "udfs/citus_internal_update_none_dist_table_metadata/12.1-1.sql"
#include "udfs/citus_internal_delete_placement_metadata/12.1-1.sql"

#include "udfs/citus_schema_move/12.1-1.sql"


#include "udfs/citus_internal_start_management_transaction/12.1-1.sql"
#include "udfs/execute_command_on_other_nodes/12.1-1.sql"
#include "udfs/citus_mark_object_distributed/12.1-1.sql"
#include "udfs/commit_management_command_2PC/12.1-1.sql"

ALTER TABLE pg_catalog.pg_dist_transaction ADD COLUMN outer_xid xid8;
