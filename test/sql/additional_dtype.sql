---
--- test cases for custom data type int1
---
SET pg_strom.regression_test_mode = on;
SET client_min_messages = error;
DROP SCHEMA IF EXISTS custom_dtype_temp CASCADE;
CREATE SCHEMA custom_dtype_temp;
RESET client_min_messages;

SET search_path = custom_dtype_temp,public;

---
--- int1
---

--- data type define

CREATE TABLE int1_table(
    id  smallint,
    num1    int1
);

-- range check
INSERT INTO int1_table values (0,0);
INSERT INTO int1_table values (1,-128);  -- lower limit
INSERT INTO int1_table values (2,127);   -- upper limit
INSERT INTO int1_table values (3,-129);  -- lower error
INSERT INTO int1_table values (4,128);   -- upper error

-- cast

CREATE TABLE various_dtypes(
    i1       int1,
    i2       int2,
    i4       int4,
    i8       int8,
    f2       float2,
    f4       float4,
    f8       float8,
    nm       numeric,
    ch       char(3),
    i1_1     int1,
    mny      money,
    m1       int1,
    mx1      int1,
    mn1      int1,
    mx2      int2,
    mn2      int2,
    mx4      int4,
    mn4      int4,
    mx8      int8,
    mn8      int8
);
INSERT INTO various_dtypes VALUES (11,12,14,18,21.1,22.2,24.4,33.33,'123',3,7.13,-1,127,-128,32767,-32768,2147483647,-2147483648,9223372036854775807,-9223372036854775808);

-- declare -A v=(["i2"]=12 ["i4"]=14 [i8]=18 ["f2"]=21.1 ["f4"]=22.2 ["f8"]=24.4 ["nm"]=33.33 ["ch"]="123");
-- declare -A d=(["i2"]="int2" ["i4"]="int4" [i8]="int8" ["f2"]="float2" ["f4"]="float4" ["f8"]="float8" ["nm"]="numeric" ["ch"]="char(3)");
-- echo "SELECT " ; for cn in ${!d[@]}; do echo "cast($cn AS int1) = cast(${v[$cn]} AS integer) \"${cn}_i1\", cast(i1 AS ${d[$cn]}) = cast(11 AS ${d[$cn]}) \"i1_$cn\"" ; done | awk 'NR==1{print $0}NR>1{print ","$0}' ; echo "FROM various_dtypes;"

---- comarison
-- eq,ne,lt,le,gt,ge
-- int1,int2,int4,int8

-- declare -A cs=(["eq"]="=" ["ne"]="<>" ["lt"]="<" ["le"]="<=" ["gt"]=">" ["ge"]=">=");
-- declare -A it=(["eq"]="TRUE" ["ne"]="FALSE" ["lt"]="FALSE" ["le"]="TRUE" ["gt"]="FALSE" ["ge"]="TRUE");
-- echo "SELECT " ; for cn in ${!d[@]}; do echo "cast($cn AS int1) = cast(${v[$cn]} AS integer) \"${cn}_i1\", cast(i1 AS ${d[$cn]}) = cast(11 AS ${d[$cn]}) \"i1_$cn\"" ; done | awk 'NR==1{print $0}NR>1{print ","$0}' ; echo "FROM various_dtypes;"
SELECT 
cast(f2 AS int1) = cast(21.1 AS integer) "f2_i1", cast(i1 AS float2) = cast(11 AS float2) "i1_f2"
,cast(f4 AS int1) = cast(22.2 AS integer) "f4_i1", cast(i1 AS float4) = cast(11 AS float4) "i1_f4"
,cast(f8 AS int1) = cast(24.4 AS integer) "f8_i1", cast(i1 AS float8) = cast(11 AS float8) "i1_f8"
,cast(ch AS int1) = cast(123 AS integer) "ch_i1", cast(i1 AS char(3)) = cast(11 AS char(3)) "i1_ch"
,cast(nm AS int1) = cast(33.33 AS integer) "nm_i1", cast(i1 AS numeric) = cast(11 AS numeric) "i1_nm"
,cast(i8 AS int1) = cast(18 AS integer) "i8_i1", cast(i1 AS int8) = cast(11 AS int8) "i1_i8"
,cast(i2 AS int1) = cast(12 AS integer) "i2_i1", cast(i1 AS int2) = cast(11 AS int2) "i1_i2"
,cast(i4 AS int1) = cast(14 AS integer) "i4_i1", cast(i1 AS int4) = cast(11 AS int4) "i1_i4"
FROM various_dtypes;

-- unset d; declare -A d=(["i2"]="12" ["i4"]="14" ["i8"]="18");
-- echo "SELECT "; for c in ${!cs[@]}; do for cn in ${!d[@]}; do echo "(i1 ${cs[$c]} $cn) is $(test 11 -${c} ${d[$cn]} && echo TRUE || echo FALSE) \"11_${c}_${d[$cn]}\" , ($cn ${cs[$c]} i1) is $(test ${d[$cn]} -${c} 11 && echo TRUE || echo FALSE) \"${d[$cn]}_${c}_11\"" ; done ; done | awk 'NR==1{print $0} NR>1{print ","$0}' ; echo "FROM various_dtypes;"
SELECT 
(i1 = i8) is FALSE "11_eq_18" , (i8 = i1) is FALSE "18_eq_11"
,(i1 = i2) is FALSE "11_eq_12" , (i2 = i1) is FALSE "12_eq_11"
,(i1 = i4) is FALSE "11_eq_14" , (i4 = i1) is FALSE "14_eq_11"
,(i1 >= i8) is FALSE "11_ge_18" , (i8 >= i1) is TRUE "18_ge_11"
,(i1 >= i2) is FALSE "11_ge_12" , (i2 >= i1) is TRUE "12_ge_11"
,(i1 >= i4) is FALSE "11_ge_14" , (i4 >= i1) is TRUE "14_ge_11"
,(i1 <> i8) is TRUE "11_ne_18" , (i8 <> i1) is TRUE "18_ne_11"
,(i1 <> i2) is TRUE "11_ne_12" , (i2 <> i1) is TRUE "12_ne_11"
,(i1 <> i4) is TRUE "11_ne_14" , (i4 <> i1) is TRUE "14_ne_11"
,(i1 > i8) is FALSE "11_gt_18" , (i8 > i1) is TRUE "18_gt_11"
,(i1 > i2) is FALSE "11_gt_12" , (i2 > i1) is TRUE "12_gt_11"
,(i1 > i4) is FALSE "11_gt_14" , (i4 > i1) is TRUE "14_gt_11"
,(i1 <= i8) is TRUE "11_le_18" , (i8 <= i1) is FALSE "18_le_11"
,(i1 <= i2) is TRUE "11_le_12" , (i2 <= i1) is FALSE "12_le_11"
,(i1 <= i4) is TRUE "11_le_14" , (i4 <= i1) is FALSE "14_le_11"
,(i1 < i8) is TRUE "11_lt_18" , (i8 < i1) is FALSE "18_lt_11"
,(i1 < i2) is TRUE "11_lt_12" , (i2 < i1) is FALSE "12_lt_11"
,(i1 < i4) is TRUE "11_lt_14" , (i4 < i1) is FALSE "14_lt_11"
FROM various_dtypes;

---- unary operators
--- +, - , @

SELECT +i1>0 as u1,-i1<0 as u2,@(-i1) >0 as u3 FROM various_dtypes;

---- arthmetic operators
--- +,-,*,/,%

-- int1 and int1
-- declare -A v=(["i1_1"]=3)
-- declare -A ops=(["plus"]="+" ["minus"]="-" ["mul"]="*" ["div"]="/" ["mod"]="%")
-- echo "SELECT "; for i in ${!v[@]};do for o in "${!ops[@]}"; do val=$(("11 ${ops[$o]} ${v[$i]}"))  ; echo "(i1 ${ops[$o]} ${i}) = ${val} as \"i1_${o}_${i}\"" ; done ; done | awk 'NR==1{print $0}NR>1{print ","$0}' && echo "FROM various_dtypes;"
SELECT 
(i1 % i1_1) = 2 as "i1_mod_i1_1"
,(i1 / i1_1) = 3 as "i1_div_i1_1"
,(i1 * i1_1) = 33 as "i1_mul_i1_1"
,(i1 + i1_1) = 14 as "i1_plus_i1_1"
,(i1 - i1_1) = 8 as "i1_minus_i1_1"
FROM various_dtypes;

-- int1 and (int2,int4,int8)
-- unset v;declare -A v=(["i2"]=12 ["i4"]=14 ["i8"]=18)
-- echo "SELECT "; for i in ${!v[@]};do for o in "${!ops[@]}"; do val=$(("${v[$i]} ${ops[$o]} 3"))  ; echo "(${i} ${ops[$o]} i1_1) = ${val} as \"i1_${o}_${i}\" -- ${v[$i]} ${ops[$o]} 3" ; done ; done | awk 'NR==1{print $0}NR>1{print ","$0}' && echo "FROM various_dtypes;"
SELECT 
(i8 % i1_1) = 0 as "i1_mod_i8" -- 18 % 3
,(i8 / i1_1) = 6 as "i1_div_i8" -- 18 / 3
,(i8 * i1_1) = 54 as "i1_mul_i8" -- 18 * 3
,(i8 + i1_1) = 21 as "i1_plus_i8" -- 18 + 3
,(i8 - i1_1) = 15 as "i1_minus_i8" -- 18 - 3
,(i2 % i1_1) = 0 as "i1_mod_i2" -- 12 % 3
,(i2 / i1_1) = 4 as "i1_div_i2" -- 12 / 3
,(i2 * i1_1) = 36 as "i1_mul_i2" -- 12 * 3
,(i2 + i1_1) = 15 as "i1_plus_i2" -- 12 + 3
,(i2 - i1_1) = 9 as "i1_minus_i2" -- 12 - 3
,(i4 % i1_1) = 2 as "i1_mod_i4" -- 14 % 3
,(i4 / i1_1) = 4 as "i1_div_i4" -- 14 / 3
,(i4 * i1_1) = 42 as "i1_mul_i4" -- 14 * 3
,(i4 + i1_1) = 17 as "i1_plus_i4" -- 14 + 3
,(i4 - i1_1) = 11 as "i1_minus_i4" -- 14 - 3
FROM various_dtypes;


-- error_case: range check
-- int1 , int1
SELECT mx1 + i1 as upper_over FROM various_dtypes;
SELECT mn1 + m1 as lower_over FROM various_dtypes;
SELECT mn1 - i1 as lower_over FROM various_dtypes;
SELECT mx1 - m1 as upper_over FROM various_dtypes;
SELECT mn1 * m1 as upper_over FROM various_dtypes;
SELECT mn1 * i1_1 as lower_over FROM various_dtypes;
SELECT mn1 / m1 as lower_over FROM various_dtypes;
-- int1, int2
SELECT mx2 + i1 as upper_over FROM various_dtypes;
SELECT mn2 + m1 as lower_over FROM various_dtypes;
SELECT mn2 - i1 as lower_over FROM various_dtypes;
SELECT mx2 - m1 as upper_over FROM various_dtypes;
SELECT mn2 * m1 as upper_over FROM various_dtypes;
SELECT mn2 * i1 as lower_over FROM various_dtypes;
SELECT mn2 / m1 as lower_over FROM various_dtypes;

SELECT i1 + mx2 as upper_over FROM various_dtypes;
SELECT m1 + mn2 as lower_over FROM various_dtypes;
SELECT mn1 - mx2 as lower_over FROM various_dtypes;
SELECT mx1 - mn2 as upper_over FROM various_dtypes;
SELECT mx1 * mx2 as upper_over FROM various_dtypes;
SELECT mx1 * mn2 as lower_over FROM various_dtypes;

-- int1, int4
SELECT mx4 + i1 as upper_over FROM various_dtypes;
SELECT mn4 + m1 as lower_over FROM various_dtypes;
SELECT mn4 - i1 as lower_over FROM various_dtypes;
SELECT mx4 - m1 as upper_over FROM various_dtypes;
SELECT mn4 * m1 as upper_over FROM various_dtypes;
SELECT mn4 * i1 as lower_over FROM various_dtypes;
SELECT mn4 / m1 as lower_over FROM various_dtypes;

SELECT i1 + mx4 as upper_over FROM various_dtypes;
SELECT m1 + mn4 as lower_over FROM various_dtypes;
SELECT mn1 - mx4 as lower_over FROM various_dtypes;
SELECT mx1 - mn4 as upper_over FROM various_dtypes;
SELECT mx1 * mx4 as upper_over FROM various_dtypes;
SELECT mx1 * mn4 as lower_over FROM various_dtypes;

-- int1, int8
SELECT mx8 + i1 as upper_over FROM various_dtypes;
SELECT mn8 + m1 as lower_over FROM various_dtypes;
SELECT mn8 - i1 as lower_over FROM various_dtypes;
SELECT mx8 - m1 as upper_over FROM various_dtypes;
SELECT mn8 * m1 as upper_over FROM various_dtypes;
SELECT mn8 * i1 as lower_over FROM various_dtypes;
SELECT mn8 / m1 as lower_over FROM various_dtypes;

SELECT i1 + mx8 as upper_over FROM various_dtypes;
SELECT m1 + mn8 as lower_over FROM various_dtypes;
SELECT mn1 - mx8 as lower_over FROM various_dtypes;
SELECT mx1 - mn8 as upper_over FROM various_dtypes;
SELECT mx1 * mx8 as upper_over FROM various_dtypes;
SELECT mx1 * mn8 as lower_over FROM various_dtypes;

-- error_case: div by 0
SELECT i1 / 0::int1 FROM various_dtypes;
SELECT i2 / 0::int1 FROM various_dtypes;
SELECT i4 / 0::int1 FROM various_dtypes;
SELECT i8 / 0::int1 FROM various_dtypes;
SELECT i1 % 0::int1 FROM various_dtypes;

-- ok_case: div by -1
-- div functions use if when -1, thus checked the flow
SELECT 
(i1 / m1) = -i1 as i1_div_m1,
(i2 / m1) = -i2 as i2_div_m1,
(i4 / m1) = -i4 as i4_div_m1,
(i8 / m1) = -i8 as i8_div_m1,
(i1 % m1) = 0 as i1_mod_m1,
(i2 % m1) = 0 as i2_mod_m1,
(i4 % m1) = 0 as i4_mod_m1,
(i8 % m1) = 0 as i8_mod_m1
FROM various_dtypes;


---- bit operations
---- &,|,#,~,<<,>>

-- unset v;declare -A v=( ["i1_1"]=3 )
-- declare -A pbo=(["and"]="&" ["or"]="|" ["xor"]="#" ["lshift"]="<<" ["rshigt"]=">>")  # for postgres
-- declare -A bbo=(["and"]="&" ["or"]="|" ["xor"]="^" ["lshift"]="<<" ["rshigt"]=">>")  # for bash
-- echo "SELECT "; for i in ${!v[@]};do for o in "${!pbo[@]}"; do val=$(("11 ${bbo[$o]} ${v[$i]}"))  ; echo "(i1 ${pbo[$o]} ${i}) = ${val} as \"i1_${o}_${i}\" -- 11 ${pbo[$o]} ${v[$i]}" ; done ; done | awk 'NR==1{print $0}NR>1{print ","$0}' && echo "FROM various_dtypes;"
SELECT 
(i1 & i1_1) = 3 as "i1_and_i1_1" -- 11 & 3
,(i1 | i1_1) = 11 as "i1_or_i1_1" -- 11 | 3
,(i1 << i1_1) = 88 as "i1_lshift_i1_1" -- 11 << 3
,(i1 >> i1_1) = 1 as "i1_rshigt_i1_1" -- 11 >> 3
,(i1 # i1_1) = 8 as "i1_xor_i1_1" -- 11 # 3
FROM various_dtypes;

-- unset pbo; declare -A pbo=(["not"]="~")  # for postgres
-- unset bbo; declare -A bbo=(["not"]="~")  # for bash
-- echo "SELECT "; for i in ${!v[@]};do for o in "${!pbo[@]}"; do val=$(("${bbo[$o]}${v[${i}]}"))  ; echo "(${pbo[$o]} ${i}) = ${val} as \"${o}_${i}\" -- ${pbo[$o]}_${v[$i]}" ; done ; done | awk 'NR==1{print $0}NR>1{print ","$0}' && echo "FROM various_dtypes;"

SELECT 
(~ i1_1) = -4 as "not_i1_1" -- ~_3
FROM various_dtypes;

---- misc functions
-- money
SELECT 
(i1_1 * mny) = 21.39::money as "int1_mul_money",
(mny * i1_1) = 21.39::money as "money_mul_int1",
(mny / i1_1) = 2.37::money as "money_div_int1",
(mny / m1) = (-7.13)::money as "money_div_m1"
FROM various_dtypes;

-- error_case: money overflow
-- big_int_max=9223372036854775807
UPDATE various_dtypes SET mny=92233720368547758.07;
SELECT mny * i1 FROM various_dtypes;
SELECT i1 * mny FROM various_dtypes;
SELECT mny / m1 FROM various_dtypes;

-- error_case: money div 0
SELECT mny / 0::int1 FROM various_dtypes;

---- aggregate function
-- sum,max,min,avg(larger,smaller)
-- variance,var_samp,var_pop,stddev, stddevv_samp,stddev_pop


CREATE TABLE aggregate_data(
    i1  int1
);
INSERT INTO aggregate_data(i1) VALUES 
(1)
,(NULL)
,(3)
,(NULL)
,(-1)
,(0)
,(NULL)
,(NULL)
,(5)
;
-- sum
SELECT sum(i1) = sum(cast(i1 AS int2)) as "sum_check" FROM aggregate_data;

-- max
SELECT max(i1) = max(cast(i1 AS int2)) as "max_check" FROM aggregate_data;

-- min
SELECT min(i1) = min(cast(i1 AS int2)) as "min_check" FROM aggregate_data;

-- avg
-- TODO: fix this to get TRUE
SELECT avg(i1) = avg(cast(i1 AS int2)) as "avg_check" FROM aggregate_data;

-- variance.var_samp
-- TODO: fix this to get TRUE 
SELECT
variance(i1) = variance(cast(i1 AS int2)) as "variance_check"
,var_samp(i1) = var_samp(cast(i1 AS int2)) as "var_samp_check"
FROM aggregate_data;

-- var_pop
-- TODO: fix this to get TRUE 
SELECT
var_pop(i1) = var_pop(cast(i1 AS int2)) as "var_pop_check"
FROM aggregate_data;

-- stddev, stddev_samp
-- TODO: fix this to get TRUE 
SELECT
stddev(i1) = stddev(cast(i1 AS int2)) as "stddev_check"
,stddev_samp(i1) = stddev_samp(cast(i1 AS int2)) as "stddev_samp_check"
FROM aggregate_data;


-- stddev_pop
-- TODO: fix this to get TRUE 
SELECT
stddev_pop(i1) = stddev_pop(cast(i1 AS int2)) as "stddev_pop_check"
FROM aggregate_data;

---- index support
-- cmp,hash

CREATE TABLE indextest(
    i1 int1,
    i2 int2,
    i4 int4,
    i8 int8
);
INSERT INTO indextest (select 128-x from generate_series(1,256) x);
UPDATE indextest SET i2=i1, i4=i1,i8=i1;

CREATE INDEX int1bt ON indextest USING btree (i1);
CREATE INDEX int2bt ON indextest USING btree (i2);
CREATE INDEX int4bt ON indextest USING btree (i4);
CREATE INDEX int8bt ON indextest USING btree (i8);

SET enable_seqscan=off;
SET pg_strom.enabled=off;
SELECT COUNT(i8) = 1 AS btint1cmp FROM indextest where i1 = CAST(0 AS int1);
SELECT COUNT(i8) = 1 AS btint12cmp FROM indextest where i1 = CAST(0 AS int2);
SELECT COUNT(i8) = 1 AS btint14cmp FROM indextest where i1 = CAST(0 AS int4);
SELECT COUNT(i8) = 1 AS btint18cmp FROM indextest where i1 = CAST(0 AS int8);

SELECT COUNT(i1) = 1 AS btint21cmp FROM indextest where i2 = CAST(0 AS int2);
SELECT COUNT(i1) = 1 AS btint41cmp FROM indextest where i4 = CAST(0 AS int4);
SELECT COUNT(i1) = 1 AS btint81cmp FROM indextest where i8 = CAST(0 AS int8);

DROP INDEX int1bt,int2bt,int4bt,int8bt;

CREATE INDEX int1hash ON indextest USING hash (i1);

-- cleanup temporary resource
SET client_min_messages = error;
DROP SCHEMA custom_dtype_temp CASCADE;