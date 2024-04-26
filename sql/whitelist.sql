--
-- whitelist engine tests
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
						   (3, 'm', 'Cai Lei',     23);
SELECT * FROM employee;

ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

SELECT sql_firewall_reset();

--------------------------------------------------------------------------------
--
-- testcase:
--   learned whitelist rules allow us to query our 'employee' table with a
--   predicate and turn back to disabled mode from enforcing mode.
--
--------------------------------------------------------------------------------
ALTER SYSTEM SET sql_firewall.firewall TO    learning;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'learning');
-- learning pg_reload_conf() function as whitelist rule entry
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'learning');
SHOW sql_firewall.firewall;
SELECT * FROM employee WHERE id = 1;
ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT * FROM sql_firewall.whitelist WHERE query NOT LIKE '%pg_sleep%';

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
SELECT * FROM sql_firewall.whitelist WHERE query NOT LIKE '%pg_sleep%';
--
-- this query would introduce a whitelist rule for 'employee'.
--
SELECT * FROM employee WHERE id = 1;
--
-- verify the settings of sql_firewall have been properly set.
--
SHOW sql_firewall.firewall;
SHOW sql_firewall.max;
SHOW sql_firewall.engine;

--------------------------------------------------------------------------------
--
-- testcase
--   add a whitelist rule of table 'employee', and it would permit our query
--   as expected
--
--------------------------------------------------------------------------------

--
-- allowed query rule for 'employee'
--
SELECT sql_firewall.add_rule('postgres', 'UPDATE employee SET age = age + 1;', 'whitelist');

ALTER SYSTEM SET sql_firewall.firewall TO enforcing;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'enforcing');
SHOW sql_firewall.firewall;
						
--
-- confirm the rules of configured rules would prohibit queries as expected.
--
SELECT * FROM sql_firewall.whitelist WHERE query NOT LIKE '%pg_sleep%';

--
-- verify the whitelist rule permit our query
--
SELECT * FROM employee WHERE id = 1;
UPDATE employee SET age = age + 2;
DELETE   FROM employee WHERE id = 1;
SELECT * FROM employee WHERE id = 1;

--
-- this query is not allowed according to the whitelist rules, so it should failed
--
SELECT * FROM employee;

ALTER SYSTEM SET sql_firewall.firewall TO    disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

--
-- both query would be permitted as we have disabled the sql_firewall
--
SELECT * FROM employee;
SELECT * FROM employee WHERE id = 1;

--
-- teardown
--
DROP TABLE employee;
