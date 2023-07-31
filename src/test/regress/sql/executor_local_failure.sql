CREATE SCHEMA failure_local_modification;
SET search_path TO failure_local_modification;
SET citus.next_shard_id TO 1989000;

SET citus.shard_replication_factor TO 1;
CREATE TABLE failover_to_local (key int PRIMARY KEY, value varchar(10));
SELECT create_reference_table('failover_to_local');

\c - - - :worker_2_port

SET search_path TO failure_local_modification;

-- prevent local connection establishment, imitate
-- a failure
ALTER SYSTEM SET citus.local_shared_pool_size TO -1;
SELECT pg_reload_conf();
SELECT pg_sleep(0.2);
BEGIN;
	-- we force the execution to use connections (e.g., remote execution)
	-- however, we do not allow connections as local_shared_pool_size=-1
	-- so, properly error out
	SET LOCAL citus.enable_local_execution TO false;
	INSERT INTO failover_to_local VALUES (1,'1'), (2,'2'),(3,'3'),(4,'4');
ROLLBACK;

ALTER SYSTEM RESET citus.local_shared_pool_size;
SELECT pg_reload_conf();

\c - - - :master_port
SET client_min_messages TO ERROR;
DROP SCHEMA failure_local_modification cascade;
