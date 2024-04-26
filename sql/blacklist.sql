--
-- blacklist engine tests
--

ALTER SYSTEM SET sql_firewall.firewall TO disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

SELECT sql_firewall_reset();

CREATE TABLE employee(id int, gender char, name text, age int);
INSERT INTO employee(id, gender, name, age) VALUES (1, 'f', 'Bai Cai',     16), 
       	    		 	       	    	   (2, 'm', 'Cheng Cheng', 20),
						   (3, 'm', 'Cai Lei',     23);
SELECT * FROM employee;

--
-- verify the default settings of sql_firewall
--
SHOW sql_firewall.firewall;
SHOW sql_firewall.max;
SHOW sql_firewall.engine;

--------------------------------------------------------------------------------
--
-- testcase
--   add a blacklist rule, and it would ban our query as expected
--
--------------------------------------------------------------------------------

-- 
-- set to blacklist engine
--
ALTER SYSTEM SET sql_firewall.engine   TO blacklist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'blacklist');
SHOW sql_firewall.engine;

--
-- prohibited queries
--
SELECT sql_firewall.add_rule('postgres', 'SELECT * FROM employee WHERE id = 1;', 'blacklist');

ALTER SYSTEM SET sql_firewall.firewall TO enforcing;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'enforcing');
SHOW sql_firewall.firewall;
						
--
-- confirm the rules of configured rules would prohibit queries as expected.
--
SELECT * FROM sql_firewall.all_rules;

--------------------------------------------------------------------------------
--
-- testcase
--   delete the rule of 'SELECT' and add an 'UPDATE' rule, we make sure that
--   the previous 'SELECT' query could be executed, and the new add 'UPDATE'
--   rule should prohibit our update query.
--
--------------------------------------------------------------------------------
SELECT * FROM employee WHERE id = 2;
SELECT * FROM employee ORDER BY id;
UPDATE employee SET age = age + 1 WHERE id = 2;
SELECT * FROM employee ORDER BY id;

ALTER SYSTEM SET sql_firewall.firewall TO disabled;
SELECT pg_reload_conf();	-- switch to disabled mode
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

--
-- confirm the rules of configured rules would prohibit queries as expected.
--
SELECT * FROM sql_firewall.all_rules;
SHOW sql_firewall.firewall;

SELECT sql_firewall.del_rule('postgres', 'SELECT * FROM employee WHERE id = 1;',            'blacklist');
SELECT sql_firewall.add_rule('postgres', 'UPDATE employee SET age = age + 1 WHERE id = 1;', 'blacklist');
-- this rule would be applied to all users
SELECT sql_firewall.add_rule('',         'UPDATE employee SET age = 100',                   'blacklist');

ALTER SYSTEM SET sql_firewall.firewall TO enforcing;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'enforcing');
SHOW sql_firewall.firewall;
						
--
-- confirm the rules of configured rules would prohibit queries as expected.
--
SELECT * FROM sql_firewall.all_rules;


--
-- the update should be prohibited, as it is in the blacklist rules we added
-- as above.
--
SELECT * FROM employee WHERE id = 2;
SELECT * FROM employee ORDER BY id;
UPDATE employee SET age = age + 1 WHERE id = 2;
UPDATE employee SET age = 102;
SELECT * FROM employee ORDER BY id;

--
-- testcase:
--   rules applied to all user would ban the new created user 'sqlfirewall_user'
--   but rules defined for concrete user would not ban the our query
--
-- pg_sleep is radomly called so we ignore it here.
SELECT * FROM sql_firewall.all_rules WHERE query NOT LIKE '%pg_sleep%';

CREATE USER sqlfirewall_user WITH PASSWORD 'sqlfirewall';
GRANT ALL ON TABLE employee TO sqlfirewall_user;
SET SESSION AUTHORIZATION sqlfirewall_user;

SELECT * FROM employee WHERE id = 2;
SELECT * FROM employee ORDER BY id;
UPDATE employee SET age = age + 1 WHERE id = 2;
UPDATE employee SET age = 102;
SELECT * FROM employee ORDER BY id;

\c -

--------------------------------------------------------------------------------
--
-- testcase
--   we do have a firewall rule here , but we disable the firewall, so we
--   can successfully do any query we have been banned before when the
--   firewall was in 'enforcing' mode.
--
--------------------------------------------------------------------------------
ALTER SYSTEM SET sql_firewall.firewall TO disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;
						
--
-- confirm the rules of configured rules would prohibit queries as expected.
--
SELECT * FROM sql_firewall.all_rules;

--
-- all query should be performed, the sql firewall has been disabled
--
SELECT * FROM employee WHERE id = 2;
SELECT * FROM employee ORDER BY id;
UPDATE employee SET age = age + 1 WHERE id = 2;
SELECT * FROM employee ORDER BY id;

--
-- teardown
--
DROP USER prohibited_user;
DROP TABLE employee;

