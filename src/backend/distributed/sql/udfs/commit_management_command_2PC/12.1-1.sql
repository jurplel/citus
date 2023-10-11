CREATE OR REPLACE FUNCTION pg_catalog.commit_management_command_2PC()
    RETURNS VOID
    LANGUAGE C
AS 'MODULE_PATHNAME', $$commit_management_command_2PC$$;

COMMENT ON FUNCTION pg_catalog.commit_management_command_2PC()
    IS 'commits management command';
