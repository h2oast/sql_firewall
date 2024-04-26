--
-- extension management tests
--   * entension creation and drop operation is ok or not
--   * default parameter settings
--   * change paramenter approaches
--

ALTER SYSTEM SET sql_firewall.firewall TO disabled;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.firewall', 'disabled');
SHOW sql_firewall.firewall;

SELECT sql_firewall_reset();
SELECT * FROM sql_firewall.all_rules;

CREATE TABLE employee(id int, gender char, name text, age int);
INSERT INTO employee(id, gender, name, age) VALUES (1, 'f', 'Bai Cai',     16), 
       	    		 	       	    	   (2, 'm', 'Cheng Cheng', 20),
						   (3, 'm', 'Cai Lei',     23);
SELECT * FROM employee;


--
-- verify the settings of sql_firewall as we expected
--
SHOW sql_firewall.firewall;
SHOW sql_firewall.max;
SHOW sql_firewall.engine;

--------------------------------------------------------------------------------
--
-- testcase
--   rule engine switch
--
--
--------------------------------------------------------------------------------
--
-- set rule engine to whitelist engine
--
ALTER SYSTEM SET sql_firewall.engine TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

--
-- whitelist engine -> blacklist engine
--
ALTER SYSTEM SET sql_firewall.engine   TO blacklist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'blacklist');
SHOW sql_firewall.engine;

--
-- blacklist engine -> hybrid engine
--
ALTER SYSTEM SET sql_firewall.engine   TO hybrid;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'hybrid');
SHOW sql_firewall.engine;

--
-- hybrid engine -> whitelist engine
--
ALTER SYSTEM SET sql_firewall.engine   TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

--
-- whitelist engine -> hybrid engine
--
ALTER SYSTEM SET sql_firewall.engine    TO hybrid;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'hybrid');
SHOW sql_firewall.engine;

--
-- hybrid engine -> black engine
--
ALTER SYSTEM SET sql_firewall.engine    TO blacklist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'blacklist');
SHOW sql_firewall.engine;

--
-- blacklist engine -> whitelist engine
--
ALTER SYSTEM SET sql_firewall.engine    TO whitelist;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SELECT wait_be_set('sql_firewall.engine', 'whitelist');
SHOW sql_firewall.engine;

--
-- cleanup test
--
DROP TABLE employee;
