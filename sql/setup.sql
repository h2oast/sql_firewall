--------------------------------------------------------------------------------
--
-- prepare for sql_firewall tests
--
--------------------------------------------------------------------------------

--
-- utility to wait a setting has been applied
--
CREATE OR REPLACE FUNCTION wait_be_set(name text, value text) RETURNS INTEGER AS $$
    DECLARE setting TEXT;
BEGIN
    --
    -- make sure our configration has been applied.
    --
    LOOP
        SELECT current_setting(name) INTO setting;
        EXIT WHEN setting = value;

        -- DO NOT occupy the CPU cycles
        PERFORM pg_sleep(0.1);	-- sleep 100 ms
    END LOOP;
   
    RETURN 0;
END;
$$
LANGUAGE PLpgSQL;

--
-- drop the aready existing sql_firewall extension
--
DROP EXTENSION IF EXISTS sql_firewall;

--
-- create the extension - sql_firewall
--
CREATE EXTENSION sql_firewall;


