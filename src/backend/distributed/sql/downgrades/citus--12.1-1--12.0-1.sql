-- citus--12.1-1--12.0-1
DROP FUNCTION pg_catalog.citus_pause_node_within_txn(int,bool,int);
-- we have modified the relevant upgrade script to include any_value changes
-- we don't need to upgrade this downgrade path for any_value changes
-- since if we are doing a Citus downgrade, not PG downgrade, then it would be no-op.

DROP FUNCTION pg_catalog.citus_internal_update_none_dist_table_metadata(
    relation_id oid, replication_model "char", colocation_id bigint,
    auto_converted boolean
);

DROP FUNCTION pg_catalog.citus_internal_delete_placement_metadata(
    placement_id bigint
);

DROP FUNCTION pg_catalog.citus_schema_move(
    schema_id regnamespace, target_node_name text, target_node_port integer,
    shard_transfer_mode citus.shard_transfer_mode
);

DROP FUNCTION pg_catalog.citus_schema_move(
    schema_id regnamespace, target_node_id integer,
    shard_transfer_mode citus.shard_transfer_mode
);


DROP FUNCTION pg_catalog.citus_internal_start_management_transaction(
    outer_xid xid8
);

DROP FUNCTION pg_catalog.execute_command_on_other_nodes(
    query text
);

DROP FUNCTION pg_catalog.citus_mark_object_distributed(
    classId Oid, objectName text, objectId Oid
);

ALTER TABLE pg_catalog.pg_dist_transaction DROP COLUMN outer_xid;
