# contrib/sql_firewall/Makefile

MODULE_big = sql_firewall
OBJS = sql_firewall.o

EXTENSION = sql_firewall
DATA = sql_firewall--0.8.sql

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/sql_firewall/sql_firewall.conf

REGRESS = setup                                                               \
          sql_firewall blacklist whitelist hybrid import                      \
          teardown

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/sql_firewall
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
