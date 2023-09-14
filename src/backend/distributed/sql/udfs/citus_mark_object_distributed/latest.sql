CREATE OR REPLACE FUNCTION pg_catalog.citus_mark_object_distributed(classId Oid, objectName text, objectId Oid)
    RETURNS VOID
    LANGUAGE C
AS 'MODULE_PATHNAME', $$citus_mark_object_distributed$$;

COMMENT ON FUNCTION pg_catalog.citus_mark_object_distributed(classId Oid, objectName text, objectId Oid)
    IS 'adds to pg_dist_node on all nodes';
