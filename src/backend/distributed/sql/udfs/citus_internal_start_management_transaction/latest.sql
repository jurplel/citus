CREATE OR REPLACE FUNCTION pg_catalog.citus_internal_start_management_transaction(outer_xid xid8)
    RETURNS VOID
    LANGUAGE C
AS 'MODULE_PATHNAME', $$citus_internal_start_management_transaction$$;

COMMENT ON FUNCTION pg_catalog.citus_internal_start_management_transaction(outer_xid xid8)
    IS 'starts management transaction';
