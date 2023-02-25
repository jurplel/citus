SHOW server_version \gset
SELECT substring(:'server_version', '\d+')::int >= 15 AS server_version_ge_15
\gset
\if :server_version_ge_15
\else
\q
\endif

SET search_path TO merge_arbitrary_schema;
INSERT INTO target_cj VALUES (1, 'target', 0);
INSERT INTO target_cj VALUES (2, 'target', 0);
INSERT INTO target_cj VALUES (2, 'target', 0);
INSERT INTO target_cj VALUES (3, 'target', 0);

INSERT INTO source_cj1 VALUES (2, 'source-1', 10);
INSERT INTO source_cj2 VALUES (2, 'source-2', 20);

BEGIN;
MERGE INTO target_cj t
USING source_cj1 s1 INNER JOIN source_cj2 s2 ON sid1 = sid2
ON t.tid = sid1 AND t.tid = 2
WHEN MATCHED THEN
        UPDATE SET src = src2
WHEN NOT MATCHED THEN
        DO NOTHING;
SELECT * FROM target_cj ORDER BY 1;
ROLLBACK;

BEGIN;
-- try accessing columns from either side of the source join
MERGE INTO target_cj t
USING source_cj1 s2
        INNER JOIN source_cj2 s1 ON sid1 = sid2 AND val1 = 10
ON t.tid = sid1 AND t.tid = 2
WHEN MATCHED THEN
        UPDATE SET src = src1, val = val2
WHEN NOT MATCHED THEN
        DO NOTHING;
SELECT * FROM target_cj ORDER BY 1;
ROLLBACK;

-- Test PREPARE
CREATE TABLE prept(t1 int, t2 int);
CREATE TABLE preps(s1 int, s2 int);

INSERT INTO prept VALUES(100, 0);

INSERT INTO preps VALUES(100, 0);
INSERT INTO preps VALUES(200, 0);

PREPARE insert(int) AS
MERGE INTO prept
USING preps
ON prept.t1 = preps.s1
WHEN MATCHED THEN
	UPDATE SET t2 = t2 + $1
WHEN NOT MATCHED THEN
	INSERT VALUES(s1, s2);

PREPARE delete(int) AS
MERGE INTO prept
USING preps
ON prept.t1 = preps.s1
WHEN MATCHED AND prept.t2 = $1 THEN
	DELETE
WHEN MATCHED THEN
	UPDATE SET t2 = t2 + 1;

EXECUTE insert(1); EXECUTE delete(0);
EXECUTE insert(1); EXECUTE delete(0);
EXECUTE insert(1); EXECUTE delete(0);
EXECUTE insert(1); EXECUTE delete(0);
EXECUTE insert(1); EXECUTE delete(0);

-- sixth time
EXECUTE insert(1); EXECUTE delete(0);

-- Should have the counter as 12 (6 * 2)
SELECT * FROM prept;
