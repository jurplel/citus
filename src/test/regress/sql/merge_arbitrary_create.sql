DROP SCHEMA IF EXISTS merge_arbitrary_schema CASCADE;
CREATE SCHEMA merge_arbitrary_schema;
SET search_path TO merge_arbitrary_schema;
SET citus.shard_count TO 4;
SET citus.next_shard_id TO 6000000;
CREATE TABLE target_cj(tid int, src text, val int);
CREATE TABLE source_cj1(sid1 int, src1 text, val1 int);
CREATE TABLE source_cj2(sid2 int, src2 text, val2 int);

SELECT create_distributed_table('target_cj', 'tid');
SELECT create_distributed_table('source_cj1', 'sid1');
SELECT create_distributed_table('source_cj2', 'sid2');
