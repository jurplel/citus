CREATE OR REPLACE FUNCTION pg_catalog.execute_command_on_other_nodes(query text)
    RETURNS VOID
    LANGUAGE C
AS 'MODULE_PATHNAME', $$execute_command_on_other_nodes$$;

COMMENT ON FUNCTION pg_catalog.execute_command_on_other_nodes(query text)
    IS 'executes command on other nodes';
