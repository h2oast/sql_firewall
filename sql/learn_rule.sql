--
-- test setup
--
DROP TABLE IF EXISTS tablea;
CREATE TABLE IF NOT EXISTS tablea(id int, name text, gender char);
DELETE FROM tablea;
INSERT INTO tablea(id, name, gender) VALUES(1, 'Bai', 'f'), (2, 'Cai', 'm');
SELECT * FROM tablea;

ALTER SYSTEM SET sql_firewall.firewall TO learning;
SELECT pg_reload_conf();
SHOW sql_firewall.firewall;

SELECT * FROM tablea WHERE id = 1;

ALTER SYSTEM SET sql_firewall.firewall TO permissive;
SELECT pg_reload_conf();
SHOW sql_firewall.firewall;
