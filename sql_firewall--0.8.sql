/* contrib/sql_firewall/sql_firewall--0.8.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sql_firewall" to load this file. \quit

CREATE SCHEMA sql_firewall;

-- Register functions.
CREATE FUNCTION sql_firewall_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION sql_firewall_statements(IN showtext boolean,
    OUT userid oid,
    OUT queryid bigint,
    OUT query text,
    OUT calls int8,
    OUT banned int8,
    OUT type  text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'sql_firewall_statements'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sql_firewall_stat_warning_count()
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION sql_firewall_stat_error_count()
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION sql_firewall_stat_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Register a view on the function for ease of use.
CREATE VIEW sql_firewall.sql_firewall_statements AS
  SELECT * FROM sql_firewall_statements(true);

GRANT SELECT ON sql_firewall.sql_firewall_statements TO PUBLIC;

CREATE VIEW sql_firewall.sql_firewall_stat AS
  SELECT sql_firewall_stat_warning_count() AS sql_warning,
         sql_firewall_stat_error_count() AS sql_error;

GRANT SELECT ON sql_firewall.sql_firewall_stat TO PUBLIC;

-- Export/import firewall rules to/from the file.
CREATE FUNCTION sql_firewall_export_rule(text)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION sql_firewall_import_rule(text)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- add/delete firewall rule to/from the rules table.
CREATE FUNCTION sql_firewall.add_rule(text, text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'sql_firewall_add_rule'
LANGUAGE C;

CREATE FUNCTION sql_firewall.del_rule(text, text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'sql_firewall_del_rule'
LANGUAGE C;

-- Export/import firewall rules to/from the file.
CREATE FUNCTION sql_firewall.export_rule(text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'sql_firewall_export_rule'
LANGUAGE C;

CREATE FUNCTION sql_firewall.import_rule(text)
RETURNS boolean
AS 'MODULE_PATHNAME','sql_firewall_import_rule'
LANGUAGE C;

-- Don't want this to be available to non-superusers.
REVOKE ALL ON FUNCTION sql_firewall_reset() FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall_stat_reset() FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall_export_rule(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall_import_rule(text) FROM PUBLIC;

REVOKE ALL ON FUNCTION sql_firewall.add_rule(text, text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall.del_rule(text, text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall.export_rule(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION sql_firewall.import_rule(text) FROM PUBLIC;

-- display only blacklist sql firewall rules
CREATE VIEW sql_firewall.blacklist AS
  SELECT userid, queryid, query, banned
    FROM sql_firewall.sql_firewall_statements
    WHERE type = 'blacklist';

-- display only whitelist sql firewall rules
CREATE VIEW sql_firewall.whitelist AS
  SELECT userid, queryid, query, calls
    FROM sql_firewall.sql_firewall_statements
    WHERE type = 'whitelist';

-- display only all sql firewall rules
CREATE VIEW sql_firewall.all_rules AS
  SELECT * FROM sql_firewall.sql_firewall_statements;

