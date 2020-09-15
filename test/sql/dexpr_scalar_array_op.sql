---
--- Test for ScalarArrayOp expression
---
SET pg_strom.regression_test_mode = on;
SET client_min_messages = error;
DROP SCHEMA IF EXISTS regtest_dexpr_scalar_array_op_temp CASCADE;
CREATE SCHEMA regtest_dexpr_scalar_array_op_temp;
RESET client_min_messages;

SET search_path = regtest_dexpr_scalar_array_op_temp,public;
CREATE TABLE regtest_data (
  id    int,
  x     int[],
  y     numeric[],
  z     text[]
);
SELECT pgstrom.random_setseed(20190630);
INSERT INTO regtest_data (
  SELECT x, array[pgstrom.random_int(2,0,1000),
                  pgstrom.random_int(2,0,1000),
                  pgstrom.random_int(2,0,1000)],
            array[pgstrom.random_float(2,0.0,100.0)::numeric(9,2),
                  pgstrom.random_float(2,0.0,100.0)::numeric(9,2),
                  pgstrom.random_float(2,0.0,100.0)::numeric(9,2)],
            array[pgstrom.random_text(2,'k***'),
                  pgstrom.random_text(2,'k***'),
                  pgstrom.random_text(2,'k***')]
    FROM generate_series(1,5000) x
);
UPDATE regtest_data
   SET x = array_append(x, pgstrom.random_int(2,0,1000)::int),
       y = array_append(y, pgstrom.random_float(2,0.0,100.0)::numeric(9,2)),
       z = array_append(z, pgstrom.random_text(2,'k***'))
 WHERE id % 7 = 3;
UPDATE regtest_data
   SET x = array_append(x, pgstrom.random_int(2,0,1000)::int),
       y = array_append(y, pgstrom.random_float(2,0.0,100.0)::numeric(9,2)),
       z = array_append(z, pgstrom.random_text(2,'k***'))
 WHERE id % 14 = 6;
UPDATE regtest_data
   SET x = array_append(x, pgstrom.random_int(2,0,1000)::int),
       y = array_append(y, pgstrom.random_float(2,0.0,100.0)::numeric(9,2)),
       z = array_append(z, pgstrom.random_text(2,'k***'))
 WHERE id % 31 = 6;

--- force to use GpuScan
SET enable_seqscan = off;
-- not to print kernel source code
SET pg_strom.debug_kernel_source = off;

-- ScalarArrayOp test
SET pg_strom.enabled = on;
EXPLAIN (costs off, verbose)
SELECT id,x INTO test01g FROM regtest_data
 WHERE 72 = ANY(x);
SELECT id,x INTO test01g FROM regtest_data
 WHERE 72 = ANY(x);
SET pg_strom.enabled = off;
SELECT id,x INTO test01p FROM regtest_data
 WHERE 72 = ANY(x);
(SELECT * FROM test01g EXCEPT SELECT * FROM test01p);
(SELECT * FROM test01p EXCEPT SELECT * FROM test01g);

SET pg_strom.enabled = on;
EXPLAIN (costs off, verbose)
SELECT id,y INTO test02g FROM regtest_data
 WHERE 75.96 = ANY(y);
SELECT id,y INTO test02g FROM regtest_data
 WHERE 75.96 = ANY(y);
SET pg_strom.enabled = off;
SELECT id,y INTO test02p FROM regtest_data
 WHERE 75.96 = ANY(y);
(SELECT * FROM test02g EXCEPT SELECT * FROM test02p);
(SELECT * FROM test02p EXCEPT SELECT * FROM test02g);

SET pg_strom.enabled = on;
EXPLAIN (costs off, verbose)
SELECT id,z INTO test03g FROM regtest_data
  WHERE 'kVOV' = ANY (z);
SELECT id,z INTO test03g FROM regtest_data
  WHERE 'kVOV' = ANY (z);
SET pg_strom.enabled = off;
SELECT id,z INTO test03p FROM regtest_data
  WHERE 'kVOV' = ANY (z);
(SELECT * FROM test03g EXCEPT SELECT * FROM test03p);
(SELECT * FROM test03p EXCEPT SELECT * FROM test03g);

-- TODO: array operation on fdw_arrow

-- should be empty result
SET pg_strom.enabled = off;
SELECT * FROM regtest_data WHERE null = ANY (x);

-- cleanup
SET client_min_messages = error;
DROP SCHEMA regtest_dexpr_scalar_array_op_temp CASCADE;
