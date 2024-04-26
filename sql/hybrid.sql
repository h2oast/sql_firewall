--------------------------------------------------------------------------------
--
-- hybrid sql firewall rule engine
--
--
--------------------------------------------------------------------------------

--
-- prepare the whitelist and blacklist rules
--   * learned whitelist rules
--   * manually added whitelist rules
--   * manually added blacklist rules
--
-- 
-- set to whitelist engine
--
ALTER SYSTEM SET sql_firewall.engine    TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

--
-- prepare test table and its tuples
--
CREATE TABLE employee(id int, gender char, name text, age int);
INSERT INTO employee(id, gender, name, age) VALUES (1, 'f', 'Bai Cai',     16), 
       	    		 	       	    	   (2, 'm', 'Cheng Cheng', 20),
						   (3, 'm', 'Cai Lei',     23),
						   (5, 'f', 'Chen Wei',    32),
						   (6, 'm', 'Li Ming',     27);
SELECT * FROM employee;

ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

SELECT sql_firewall_reset();

--
-- learn whitelist rules
--
ALTER SYSTEM SET sql_firewall.firewall TO    learning;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'learning');
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SHOW sql_firewall.firewall;
SELECT wait_be_set('sql_firewall.firewall', 'learning');
ALTER SYSTEM SET sql_firewall.engine TO hybrid;
ALTER SYSTEM SET sql_firewall.engine TO blacklist;
ALTER SYSTEM SET sql_firewall.engine TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SHOW sql_firewall.engine;


SELECT * FROM employee WHERE id = 1;
SELECT * FROM employee;

ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

--
-- the following whitelist rules should be properly configured here before
-- we test our whitelist rule engine, if this does not hold we would not be
-- able to disable the firewall of 'enforcing' mode at all, and then we
-- would not be able to do our rest tests.
--
ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

SHOW sql_firewall.firewall;
SHOW sql_firewall.max;
SHOW sql_firewall.engine;


--
-- allowed query rule for 'employee'
--
SELECT sql_firewall.add_rule('postgres', 'UPDATE employee SET age = age + 1;', 'whitelist');

--
-- prohibited query rule for 'employee'
--
SELECT sql_firewall.add_rule('postgres', 'UPDATE employee SET age = age + 10 WHERE id in (1, 2);', 'blacklist');
-- the following rule is defined for the same query, they should work on different rule engines.
SELECT sql_firewall.add_rule('postgres', 'SELECT * FROM employee WHERE id in (5, 10);',            'whitelist');
SELECT sql_firewall.add_rule('postgres', 'SELECT * FROM employee WHERE id in (5, 10);',            'blacklist');

-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.firewall TO enforcing;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'enforcing');
SHOW sql_firewall.firewall;
SHOW sql_firewall.engine;
						

--------------------------------------------------------------------------------
-- 
-- whitelist rule engine
-- 
--------------------------------------------------------------------------------

-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

-- 
-- there is a whitelist rule for following queries, they are permitted,
-- these rules are learned whitelist rules.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- there is also a whitelist rule for this query, it is permitted, the rule is
-- mannully added as above.
--
UPDATE employee SET age = age + 100;

--
-- the following queries are not permitted, as there are no whitelist rules for
-- them and the rule engine is in whitelist mode.
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;

--------------------------------------------------------------------------------
-- 
-- testcase 01: blacklist rule engine
--   switched from whitelist rule engine
-- 
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO blacklist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'blacklist');
SHOW sql_firewall.engine;

-- 
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
--
UPDATE employee SET age = age - 100;

--
-- the following queries are not allowed, as there are blacklist rules for
-- them and the rule engine is in blacklist mode.
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;


--------------------------------------------------------------------------------
-- 
-- testcase 02: hybrid rule engine
--   switched from blacklist rule engine
-- 
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO hybrid;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'hybrid');
SHOW sql_firewall.engine;

-- 
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 5;

--
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
--
UPDATE employee SET age = age + 10;

--
-- the following queries are not allowed, as there are blacklist rules or there
-- is no whitelist rule entry for them and the rule engine is in blacklist mode.
-- 
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);
SELECT * FROM employee where gender = 'f' AND name like 'Chen %';

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;


--------------------------------------------------------------------------------
-- 
-- testcase 03: whitelist rule engine
--   switched from hybrid rule engine
-- 
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

-- 
-- there is a whitelist rule for following queries, they are permitted,
-- these rules are learned whitelist rules.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- there is also a whitelist rule for this query, it is permitted, the rule is
-- mannully added as above.
--
UPDATE employee SET age = age + 100;

--
-- the following queries are not permitted, as there are no whitelist rules for
-- them and the rule engine is in whitelist mode.
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;


--------------------------------------------------------------------------------
-- 
-- testcase 04: hybrid rule engine
--   switched from whitelist rule engine
--
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO hybrid;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'hybrid');
SHOW sql_firewall.engine;

-- 
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 5;

--
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
--
UPDATE employee SET age = age + 10;

--
-- the following queries are not allowed, as there are blacklist rules or there
-- is no whitelist rule entry for them and the rule engine is in blacklist mode.
-- 
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);
SELECT * FROM employee where gender = 'f' AND name like 'Chen %';

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;

--------------------------------------------------------------------------------
-- 
-- testcase 05: blacklist rule engine
--   switched from hybrid rule engine
-- 
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO blacklist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'blacklist');
SHOW sql_firewall.engine;

-- 
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- should be allowed. for blacklist rule engine, there is no blacklist rule for
-- them.
--
UPDATE employee SET age = age - 100;

--
-- the following queries are not allowed, as there are blacklist rules for
-- them and the rule engine is in blacklist mode.
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;


--------------------------------------------------------------------------------
-- 
-- testcase 06: whitelist rule engine
--   switched from blacklist rule engine
-- 
--------------------------------------------------------------------------------
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

ALTER SYSTEM SET sql_firewall.engine TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

-- 
-- there is a whitelist rule for following queries, they are permitted,
-- these rules are learned whitelist rules.
-- 
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- there is also a whitelist rule for this query, it is permitted, the rule is
-- mannully added as above.
--
UPDATE employee SET age = age + 100;

--
-- the following queries are not permitted, as there are no whitelist rules for
-- them and the rule engine is in whitelist mode.
UPDATE employee SET age = age + 10 WHERE id in (2, 3);
SELECT * FROM employee WHERE id in (5, 10);

-- 
-- verify the execution of above queries
-- 
SELECT * FROM employee;


--
-- testcase level teardown
--
ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

DROP TABLE employee;
