/*-------------------------------------------------------------------------
 *
 * sql_firewall.c
 *		Prevent query execution which is not allowd by the rules.
 *
 * Copyright (c) 2015, Uptime Technologies, LLC
 *
 * sql_firewall is built on the top of pg_stat_statements.
 *
 */

 /*
 * pg_stat_statements.c
 *		Track statement execution times across a whole database cluster.
 *
 * Execution costs are totalled for each distinct source query, and kept in
 * a shared hashtable.  (We track only as many distinct queries as will fit
 * in the designated amount of shared memory.)
 *
 * As of Postgres 9.2, this module normalizes query entries.  Normalization
 * is a process whereby similar queries, typically differing only in their
 * constants (though the exact rules are somewhat more subtle than that) are
 * recognized as equivalent, and are tracked as a single entry.  This is
 * particularly useful for non-prepared queries.
 *
 * Normalization is implemented by fingerprinting queries, selectively
 * serializing those fields of each query tree's nodes that are judged to be
 * essential to the query.  This is referred to as a query jumble.  This is
 * distinct from a regular serialization in that various extraneous
 * information is ignored as irrelevant or not essential to the query, such
 * as the collations of Vars and, most notably, the values of constants.
 *
 * This jumble is acquired at the end of parse analysis of each query, and
 * a 32-bit hash of it is stored into the query's Query.queryId field.
 * The server then copies this value around, making it available in plan
 * tree(s) generated from the query.  The executor can then use this value
 * to blame query costs on the proper queryId.
 *
 * To facilitate presenting entries to users, we create "representative" query
 * strings in which constants are replaced with '?' characters, to make it
 * clearer what a normalized entry can represent.  To save on shared memory,
 * and to avoid having to truncate oversized query strings, we store these
 * strings in a temporary external query-texts file.  Offsets into this
 * file are kept in shared memory.
 *
 * Note about locking issues: to create or delete an entry in the shared
 * hashtable, one must hold pgss->lock exclusively.  Modifying any field
 * in an entry except the counters requires the same.  To look up an entry,
 * one must hold the lock shared.  To read or update the counters within
 * an entry, one must hold the lock shared or exclusive (so the entry doesn't
 * disappear!) and also take the entry's mutex spinlock.
 * The shared state variable pgss->extent (the next free spot in the external
 * query-text file) should be accessed only while holding either the
 * pgss->mutex spinlock, or exclusive lock on pgss->lock.  We use the mutex to
 * allow reserving file space while holding only shared lock on pgss->lock.
 * Rewriting the entire external query-text file, eg for garbage collection,
 * requires holding pgss->lock exclusively; this allows individual entries
 * in the file to be read or written while holding only shared lock.
 *
 *
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_statements/pg_stat_statements.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/hash.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/scanner.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "utils/acl.h"


PG_MODULE_MAGIC;

/* Location of permanent stats file (valid when database is shut down) */
#define PGSS_STATEMENTS_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/sql_firewall_statements.stat"
#define PGSS_COUNTER_FILE	    PGSTAT_STAT_PERMANENT_DIRECTORY "/sql_firewall.stat"

/*
 * Location of external query text file.  We don't keep it in the core
 * system's stats_temp_directory.  The core system can safely use that GUC
 * setting, because the statistics collector temp file paths are set only once
 * as part of changing the GUC, but pg_stat_statements has no way of avoiding
 * race conditions.  Besides, we only expect modest, infrequent I/O for query
 * strings, so placing the file on a faster filesystem is not compelling.
 */
#define PGSS_STATEMENTS_TEMP_FILE	PG_STAT_TMP_DIR "/sql_firewall_query_texts.stat"

/* Magic number identifying the stats file format */
static const uint32 PGSS_FILE_HEADER = 0x20140125;

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 PGSS_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define ASSUMED_LENGTH_INIT		1024	/* initial assumed mean query length */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5		/* free this % of entries at once */

#define JUMBLE_SIZE				1024	/* query serialization buffer size */

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 */
typedef struct pgssHashKey
{
	Oid			userid;			/* user OID */
	uint32		queryid;		/* query identifier */
	char        type;	    	/* rule type:
								 *   'w': whitelist
								 *   'b': blacklist
								 */
} pgssHashKey;

/*
 * The actual stats counters kept within pgssEntry.
 */
typedef struct Counters
{
	int64		calls;			/* # of times executed */
	int64		banned;			/* # of times prohibited by blacklist entry */
} Counters;

/*
 * Statistics per statement
 *
 * Note: in event of a failure in garbage collection of the query text file,
 * we reset query_offset to zero and query_len to -1.  This will be seen as
 * an invalid state by qtext_fetch().
 */
typedef struct pgssEntry
{
	pgssHashKey key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	Size		query_offset;	/* query text offset in external file */
	int			query_len;		/* # of valid bytes in query string */
	int			encoding;		/* query text encoding */
	slock_t		mutex;			/* protects the counters only */
	uint32      type;    		/* rule type, can be one of
								 *   'w'   : whitelist
								 *   'b'   : blacklist
								 *   'd'   : dummy entry
								 */
} pgssEntry;

/*
 * Global shared state
 */
typedef struct pgssSharedState
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	double		cur_median_usage;		/* current median usage in hashtable */
	Size		mean_query_len; /* current mean entry text length */
	slock_t		mutex;			/* protects following fields only: */
	Size		extent;			/* current extent of query file */
	int			n_writers;		/* number of active writers to query file */
	int			gc_count;		/* query file garbage collection cycle count */
	int64			error_count;
	int64			warning_count;
} pgssSharedState;

/*
 * Struct for tracking locations/lengths of constants during normalization
 */
typedef struct pgssLocationLen
{
	int			location;		/* start offset in query text */
	int			length;			/* length in bytes, or -1 to ignore */
} pgssLocationLen;

/*
 * Working state for computing a query jumble and producing a normalized
 * query string
 */
typedef struct pgssJumbleState
{
	/* Jumble of current query tree */
	unsigned char *jumble;

	/* Number of bytes used in jumble[] */
	Size		jumble_len;

	/* Array of locations of constants that should be removed */
	pgssLocationLen *clocations;

	/* Allocated length of clocations array */
	int			clocations_buf_size;

	/* Current number of valid entries in clocations array */
	int			clocations_count;
} pgssJumbleState;

extern void JumbleQuery(pgssJumbleState *jstate, Query *query);

/*---- Local variables ----*/

/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	nested_level = 0;

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
static pgssSharedState *pgss = NULL;
static HTAB *pgss_hash = NULL;

/*---- GUC variables ----*/

typedef enum
{
	PGSS_TRACK_NONE,			/* track no statements */
	PGSS_TRACK_TOP,				/* only top level statements */
	PGSS_TRACK_ALL				/* all statements, including nested ones */
}	PGSSTrackLevel;

#ifdef NOT_USED
static const struct config_enum_entry track_options[] =
{
	{"none", PGSS_TRACK_NONE, false},
	{"top", PGSS_TRACK_TOP, false},
	{"all", PGSS_TRACK_ALL, false},
	{NULL, 0, false}
};
#endif

typedef enum
{
	PGFW_MODE_DISABLED,
	PGFW_MODE_LEARNING,
	PGFW_MODE_PERMISSIVE,
	PGFW_MODE_ENFORCING
}	PGFWMode;


typedef enum
{
	PGFW_ENGINE_NONE      = 0x00,
	PGFW_ENGINE_WHITELIST = 0x01,
	PGFW_ENGINE_BLACKLIST = 0x02,
	PGFW_ENGINE_HYBRID    = 0x03,
}	PGFWEngineType;
  
static const struct config_enum_entry mode_options[] =
{
	{"disabled", PGFW_MODE_DISABLED, false},
	{"learning", PGFW_MODE_LEARNING, false},
	{"permissive", PGFW_MODE_PERMISSIVE, false},
	{"enforcing", PGFW_MODE_ENFORCING, false},
	{NULL, 0, false}
};

typedef enum
{
	PGFW_DUMMY_ENTRY     = 'd',	/* 'd', dummy entry, should not used at all yet */
	PGFW_WHITELIST_ENTRY = 'w',	/* 'w', whitelist entry */
	PGFW_BLACKLIST_ENTRY = 'b',	/* 'b', blacklist entry */
} PGFWEntryType;

static const struct config_enum_entry rule_engine_options[] =
{
	{"none",      PGFW_ENGINE_NONE,      false},
	{"whitelist", PGFW_ENGINE_WHITELIST, false},
	{"blacklist", PGFW_ENGINE_BLACKLIST, false},
	{"hybrid",    PGFW_ENGINE_HYBRID,    false},
	{NULL,        0,                     false}
};

static const struct config_enum_entry rule_type_options[] =
{
	{"dummy",     PGFW_DUMMY_ENTRY,      false},
	{"whitelist", PGFW_WHITELIST_ENTRY,  false},
	{"blacklist", PGFW_BLACKLIST_ENTRY,  false},
	{NULL,        0,                     false}
};


static int  pgfw_rule_engine;

static int	pgfw_mode;			/* firewall mode */

static int	pgss_max;			/* max # statements to track */
static int	pgss_track;			/* tracking level */
static bool pgss_save;			/* whether to save stats across shutdown */


#define pgss_enabled() \
	(pgss_track == PGSS_TRACK_ALL || \
	(pgss_track == PGSS_TRACK_TOP && nested_level == 0))

#define record_gc_qtexts() \
	do { \
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss; \
		SpinLockAcquire(&s->mutex); \
		s->gc_count++; \
		SpinLockRelease(&s->mutex); \
	} while(0)

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(sql_firewall_reset);
PG_FUNCTION_INFO_V1(sql_firewall_statements);
PG_FUNCTION_INFO_V1(sql_firewall_stat_error_count);
PG_FUNCTION_INFO_V1(sql_firewall_stat_warning_count);
PG_FUNCTION_INFO_V1(sql_firewall_stat_reset);
PG_FUNCTION_INFO_V1(sql_firewall_export_rule);
PG_FUNCTION_INFO_V1(sql_firewall_import_rule);
PG_FUNCTION_INFO_V1(sql_firewall_add_rule);
PG_FUNCTION_INFO_V1(sql_firewall_del_rule);

static void pgss_shmem_startup(void);
static void update_firewall_rule_file(void);
static void update_firewall_counter_file(void);
static void pgss_shmem_shutdown(int code, Datum arg);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction,
				 long count);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);
static void pgss_ProcessUtility(Node *parsetree, const char *queryString,
					ProcessUtilityContext context, ParamListInfo params,
					DestReceiver *dest, char *completionTag);
static uint32 pgss_hash_fn(const void *key, Size keysize);
static int	pgss_match_fn(const void *key1, const void *key2, Size keysize);
static uint32 pgss_hash_string(const char *str);
static void pgss_store(const char *query, uint32 queryId,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   pgssJumbleState *jstate);
static void pg_stat_statements_internal(FunctionCallInfo fcinfo,
							bool showtext);
static Size pgss_memsize(void);
static pgssEntry *entry_alloc(pgssHashKey *key, Size query_offset, int query_len,
			int encoding, bool sticky);
#ifdef NOT_USED
static void entry_dealloc(void);
#endif
static bool qtext_store(const char *query, int query_len,
			Size *query_offset, int *gc_count);
static bool pgss_restore(Oid userid, uint32 queryid, const char *query,
						 int64 calls, int64 banned, uint32 engine);
static char *qtext_load_file(Size *buffer_size);
static char *qtext_fetch(Size query_offset, int query_len,
			char *buffer, Size buffer_size);
static bool need_gc_qtexts(void);
static void gc_qtexts(void);
static void entry_reset(void);
static void AppendJumble(pgssJumbleState *jstate,
			 const unsigned char *item, Size size);
static void JumbleRangeTable(pgssJumbleState *jstate, List *rtable);
static void JumbleExpr(pgssJumbleState *jstate, Node *node);
static void RecordConstLocation(pgssJumbleState *jstate, int location);
static char *generate_normalized_query(pgssJumbleState *jstate, const char *query,
						  int *query_len_p, int encoding);
static void fill_in_constant_lengths(pgssJumbleState *jstate, const char *query);
static int	comp_location(const void *a, const void *b);

static bool       to_be_prohibited(Oid userid, uint32 queryid);
static pgssEntry *lookup_whitelist(Oid userid, uint32 queryid);
static uint32     sql_firewall_queryid(const char *query_string, char **normalized_query);
static int        add_rule(const char* user, const char *query_string, uint32 rule_type);
static int        del_rule(const char* user, const char *query_string, uint32 rule_type);
static char      *rule_typename(char rule_type);




/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("sql_firewall.max",
	  "Sets the maximum number of statements tracked by sql_firewall.",
							NULL,
							&pgss_max,
							5000,
							100,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	pgss_track = PGSS_TRACK_TOP;
	pgss_save = true;

	DefineCustomEnumVariable("sql_firewall.firewall",
			   "Enable SQL Firewall feature.",
							 NULL,
							 &pgfw_mode,
							 PGFW_MODE_DISABLED,
							 mode_options,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("sql_firewall");

	DefineCustomEnumVariable("sql_firewall.engine",
			   "SQL Firewall rule search engine. whitelist | backlist | hybrid."
			   "whitelist: take account of whitelist rules only"
			   "blacklist: take account of blacklist rules only"
			   "hybrid: take account of both whitelist and blacklist rules",
							 NULL,
							 &pgfw_rule_engine,
							 PGFW_ENGINE_HYBRID,
							 rule_engine_options,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("sql_firewall");

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgss_shmem_startup().
	 */
	RequestAddinShmemSpace(pgss_memsize());
	RequestAddinLWLocks(1);

	/*
	 * Install hooks.
	 */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgss_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgss_post_parse_analyze;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgss_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgss_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgss_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgss_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgss_ProcessUtility;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
	ProcessUtility_hook = prev_ProcessUtility;
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 */
static void
pgss_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE	   *file = NULL;
	FILE	   *qfile = NULL;
	uint32		header;
	int32		num;
	int32		pgver;
	int32		i;
	int			buffer_size;
	char	   *buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgss = NULL;
	pgss_hash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("sql_firewall",
						   sizeof(pgssSharedState),
						   &found);

	if (!found)
	{
		/* First time through ... */
		pgss->lock = LWLockAssign();
		pgss->cur_median_usage = ASSUMED_MEDIAN_INIT;
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;
		SpinLockInit(&pgss->mutex);
		pgss->extent = 0;
		pgss->n_writers = 0;
		pgss->gc_count = 0;
		pgss->warning_count = 0;
		pgss->error_count = 0;
	}

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(pgssHashKey);
	info.entrysize = sizeof(pgssEntry);
	info.hash = pgss_hash_fn;
	info.match = pgss_match_fn;
	pgss_hash = ShmemInitHash("sql_firewall hash",
							  pgss_max, pgss_max,
							  &info,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */

	/* Unlink query text file possibly left over from crash */
	unlink(PGSS_STATEMENTS_TEMP_FILE);

	/* Allocate new query text temp file */
	qfile = AllocateFile(PGSS_STATEMENTS_TEMP_FILE, PG_BINARY_W);
	if (qfile == NULL)
		goto write_error;

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 */
	if (!pgss_save)
	{
		FreeFile(qfile);
		return;
	}

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(PGSS_STATEMENTS_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done */
		FreeFile(qfile);
		return;
	}

	buffer_size = 2048;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != PGSS_FILE_HEADER ||
		pgver != PGSS_PG_MAJOR_VERSION)
		goto data_error;

	for (i = 0; i < num; i++)
	{
		pgssEntry	temp;
		pgssEntry  *entry;
		Size		query_offset;

		if (fread(&temp, sizeof(pgssEntry), 1, file) != 1)
			goto read_error;

		/* Encoding is the only field we can easily sanity-check */
		if (!PG_VALID_BE_ENCODING(temp.encoding))
			goto data_error;

		/* Resize buffer as needed */
		if (temp.query_len >= buffer_size)
		{
			buffer_size = Max(buffer_size * 2, temp.query_len + 1);
			buffer = repalloc(buffer, buffer_size);
		}

		if (fread(buffer, 1, temp.query_len + 1, file) != temp.query_len + 1)
			goto read_error;

		/* Should have a trailing null, but let's make sure */
		buffer[temp.query_len] = '\0';

		/* Store the query text */
		query_offset = pgss->extent;
		if (fwrite(buffer, 1, temp.query_len + 1, qfile) != temp.query_len + 1)
			goto write_error;
		pgss->extent += temp.query_len + 1;

		/* make the hashtable entry (discards old entries if too many) */
		entry = entry_alloc(&temp.key, query_offset, temp.query_len,
							temp.encoding,
							false);

		if (entry == NULL)
			break;

		/* copy in the actual stats */
		entry->counters = temp.counters;
	}

	pfree(buffer);
	FreeFile(file);
	FreeFile(qfile);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication slaves, etc.  A new file will be written on next
	 * shutdown.
	 *
	 * Note: it's okay if the PGSS_STATEMENTS_TEMP_FILE is included in a basebackup,
	 * because we remove that file on startup; it acts inversely to
	 * PGSS_STATEMENTS_FILE, in that it is only supposed to be around when the
	 * server is running, whereas PGSS_STATEMENTS_FILE is only supposed to be around
	 * when the server is not running.  Leaving the file creates no danger of
	 * a newly restored database having a spurious record of execution costs,
	 * which is what we're really concerned about here.
	 */
#ifdef NOT_USED
	unlink(PGSS_STATEMENTS_FILE);
#endif

	file = AllocateFile(PGSS_COUNTER_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done */
	}

	{
		int64 warnings = 0;
		int64 errors = 0;
		if (file && fscanf(file, "%ld %ld", &warnings, &errors) != 2)
		{
			goto data_error;
		}

		SpinLockAcquire(&pgss->mutex);
		pgss->warning_count = warnings;
		pgss->error_count = errors;
		SpinLockRelease(&pgss->mutex);
	}

	if (file)
		FreeFile(file);
	unlink(PGSS_COUNTER_FILE);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read sql_firewall file \"%s\": %m",
					PGSS_STATEMENTS_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in sql_firewall file \"%s\"",
					PGSS_STATEMENTS_FILE)));
	goto fail;
write_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write sql_firewall file \"%s\": %m",
					PGSS_STATEMENTS_TEMP_FILE)));
fail:
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	if (qfile)
		FreeFile(qfile);
	/* If possible, throw away the bogus file; ignore any error */
#ifdef NOT_USED
	unlink(PGSS_STATEMENTS_FILE);
#endif

	/*
	 * Don't unlink PGSS_STATEMENTS_TEMP_FILE here; it should always be around while the
	 * server is running with pg_stat_statements enabled
	 */
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
update_firewall_rule_file(void)
{
	FILE	   *file;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	pgssEntry  *entry;

	file = AllocateFile(PGSS_STATEMENTS_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSS_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&PGSS_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(pgss_hash);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto error;

	/*
	 * When serializing to disk, we store query texts immediately after their
	 * entry data.  Any orphaned query texts are thereby excluded.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->query_len;
		char	   *qstr = qtext_fetch(entry->query_offset, len,
									   qbuffer, qbuffer_size);

		if (qstr == NULL)
			continue;			/* Ignore any entries with bogus texts */

		if (fwrite(entry, sizeof(pgssEntry), 1, file) != 1 ||
			fwrite(qstr, 1, len + 1, file) != len + 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	free(qbuffer);
	qbuffer = NULL;

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	if (rename(PGSS_STATEMENTS_FILE ".tmp", PGSS_STATEMENTS_FILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_FILE ".tmp")));

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write sql_firewall file \"%s\": %m",
					PGSS_STATEMENTS_FILE ".tmp")));
	if (qbuffer)
		free(qbuffer);
	if (file)
		FreeFile(file);
}

static void
update_firewall_counter_file(void)
{
	FILE	   *file;

	/*
	 * Update the counter file
	 */
	file = AllocateFile(PGSS_COUNTER_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write sql_firewall file \"%s\": %m",
						PGSS_COUNTER_FILE ".tmp")));
		goto error;
	}

	{
		int64 warnings = 0;
		int64 errors = 0;

		SpinLockAcquire(&pgss->mutex);
		warnings = pgss->warning_count;
		errors = pgss->error_count;
		SpinLockRelease(&pgss->mutex);

		fprintf(file, "%ld %ld", warnings, errors);
	}

	FreeFile(file);

	if (rename(PGSS_COUNTER_FILE ".tmp", PGSS_COUNTER_FILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_FILE ".tmp")));

error:
	return;
}

static void
pgss_shmem_shutdown(int code, Datum arg)
{
	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgss || !pgss_hash)
		return;

	/* Don't dump if told not to. */
	if (!pgss_save)
		return;

	/* Update the firewall rule file*/
	/* rule can be added during learning mode or inserted manually by
	 * database administrator, or imported from human maintained file.
	 * */
	update_firewall_rule_file();

	update_firewall_counter_file();

	/* Unlink query-texts file; it's not needed while shutdown */
	unlink(PGSS_STATEMENTS_TEMP_FILE);
	unlink(PGSS_STATEMENTS_FILE ".tmp");
	unlink(PGSS_COUNTER_FILE ".tmp");

	elog(LOG, "sql_firewall file \"%s\" has been updated.", PGSS_STATEMENTS_FILE);

	return;
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query)
{
	pgssJumbleState jstate;

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query);

	/* queryId could be set by other module, like pg_stat_statements */
	if (query->queryId != 0)
		return;

	/* Assert we didn't do this already */
	Assert(query->queryId == 0);

	/* Safety check... */
	if (!pgss || !pgss_hash)
		return;

	/*
	 * Utility statements get queryId zero.  We do this even in cases where
	 * the statement contains an optimizable statement for which a queryId
	 * could be derived (such as EXPLAIN or DECLARE CURSOR).  For such cases,
	 * runtime control will first go through ProcessUtility and then the
	 * executor, and we don't want the executor hooks to do anything, since we
	 * are already measuring the statement's costs at the utility level.
	 */
	if (query->utilityStmt)
	{
		query->queryId = 0;
		return;
	}

	/* Set up workspace for query jumbling */
	jstate.jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate.jumble_len = 0;
	jstate.clocations_buf_size = 32;
	jstate.clocations = (pgssLocationLen *)
		palloc(jstate.clocations_buf_size * sizeof(pgssLocationLen));
	jstate.clocations_count = 0;

	/* Compute query ID and mark the Query node with it */
	JumbleQuery(&jstate, query);
	query->queryId = hash_any(jstate.jumble, jstate.jumble_len);
	elog(LOG, "query \'%s\' query id %u", pstate->p_sourcetext, query->queryId);

	/*
	 * If we are unlucky enough to get a hash of zero, use 1 instead, to
	 * prevent confusion with the utility-statement case.
	 */
	if (query->queryId == 0)
		query->queryId = 1;

	/*
	 * If we were able to identify any ignorable constants, we immediately
	 * create a hash table entry for the query, so that we can record the
	 * normalized form of the query string.  If there were no such constants,
	 * the normalized string would be the same as the query text anyway, so
	 * there's no need for an early entry.
	 */
	if (jstate.clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   0,
				   0,
				   NULL,
				   &jstate);
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
	if (pgss_enabled() && queryDesc->plannedstmt->queryId != 0)
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, long count)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	uint32		queryId = queryDesc->plannedstmt->queryId;

	if (queryId != 0 && queryDesc->totaltime && pgss_enabled())
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->totaltime->total * 1000.0,		/* convert to msec */
				   queryDesc->estate->es_processed,
				   &queryDesc->totaltime->bufusage,
				   NULL);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
pgss_ProcessUtility(Node *parsetree, const char *queryString,
					ProcessUtilityContext context, ParamListInfo params,
					DestReceiver *dest, char *completionTag)
{
	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing, and inconsistent with other
	 * cases where planning time is not included at all.
	 *
	 * Likewise, we don't track execution of DEALLOCATE.
	 */
	if (pgss_enabled() &&
		!IsA(parsetree, ExecuteStmt) &&
		!IsA(parsetree, PrepareStmt) &&
		!IsA(parsetree, DeallocateStmt))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
		uint32		queryId;

		bufusage_start = pgBufferUsage;
		INSTR_TIME_SET_CURRENT(start);

		nested_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(parsetree, queryString,
									context, params,
									dest, completionTag);
			else
				standard_ProcessUtility(parsetree, queryString,
										context, params,
										dest, completionTag);
			nested_level--;
		}
		PG_CATCH();
		{
			nested_level--;
			PG_RE_THROW();
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* parse command tag to retrieve the number of affected rows. */
		if (completionTag &&
			strncmp(completionTag, "COPY ", 5) == 0)
		{
#ifdef HAVE_STRTOULL
			rows = strtoull(completionTag + 5, NULL, 10);
#else
			rows = strtoul(completionTag + 5, NULL, 10);
#endif
		}
		else
			rows = 0;

		/* calc differences of buffer counters. */
		bufusage.shared_blks_hit =
			pgBufferUsage.shared_blks_hit - bufusage_start.shared_blks_hit;
		bufusage.shared_blks_read =
			pgBufferUsage.shared_blks_read - bufusage_start.shared_blks_read;
		bufusage.shared_blks_dirtied =
			pgBufferUsage.shared_blks_dirtied - bufusage_start.shared_blks_dirtied;
		bufusage.shared_blks_written =
			pgBufferUsage.shared_blks_written - bufusage_start.shared_blks_written;
		bufusage.local_blks_hit =
			pgBufferUsage.local_blks_hit - bufusage_start.local_blks_hit;
		bufusage.local_blks_read =
			pgBufferUsage.local_blks_read - bufusage_start.local_blks_read;
		bufusage.local_blks_dirtied =
			pgBufferUsage.local_blks_dirtied - bufusage_start.local_blks_dirtied;
		bufusage.local_blks_written =
			pgBufferUsage.local_blks_written - bufusage_start.local_blks_written;
		bufusage.temp_blks_read =
			pgBufferUsage.temp_blks_read - bufusage_start.temp_blks_read;
		bufusage.temp_blks_written =
			pgBufferUsage.temp_blks_written - bufusage_start.temp_blks_written;
		bufusage.blk_read_time = pgBufferUsage.blk_read_time;
		INSTR_TIME_SUBTRACT(bufusage.blk_read_time, bufusage_start.blk_read_time);
		bufusage.blk_write_time = pgBufferUsage.blk_write_time;
		INSTR_TIME_SUBTRACT(bufusage.blk_write_time, bufusage_start.blk_write_time);

		/* For utility statements, we just hash the query string directly */
		queryId = pgss_hash_string(queryString);

		pgss_store(queryString,
				   queryId,
				   INSTR_TIME_GET_MILLISEC(duration),
				   rows,
				   &bufusage,
				   NULL);
	}
	else
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(parsetree, queryString,
								context, params,
								dest, completionTag);
		else
			standard_ProcessUtility(parsetree, queryString,
									context, params,
									dest, completionTag);
	}
}

/*
 * Calculate hash value for a key
 */
static uint32
pgss_hash_fn(const void *key, Size keysize)
{
	const pgssHashKey *k = (const pgssHashKey *) key;

	return hash_uint32((uint32) k->userid) ^
		hash_uint32((uint32) k->queryid) ^
		hash_uint32((uint32) k->type);
}

/*
 * Compare two keys - zero means match
 */
static int
pgss_match_fn(const void *key1, const void *key2, Size keysize)
{
	const pgssHashKey *k1 = (const pgssHashKey *) key1;
	const pgssHashKey *k2 = (const pgssHashKey *) key2;

	if (k1->userid == k2->userid &&
		k1->queryid == k2->queryid &&
		k1->type    == k2->type)
		return 0;
	else
		return 1;
}

/*
 * Given an arbitrarily long query string, produce a hash for the purposes of
 * identifying the query, without normalizing constants.  Used when hashing
 * utility statements.
 */
static uint32
pgss_hash_string(const char *str)
{
	return hash_any((const unsigned char *) str, strlen(str));
}

static void
stat_warning_increment(void)
{
	volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

	SpinLockAcquire(&s->mutex);
	s->warning_count++;
	SpinLockRelease(&s->mutex);
}

static void
stat_error_increment(void)
{
	volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

	SpinLockAcquire(&s->mutex);
	s->error_count++;
	SpinLockRelease(&s->mutex);
}

/*
 * Store some statistics for a statement.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage are ignored in this case.
 */
static void
pgss_store(const char *query, uint32 queryId,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   pgssJumbleState *jstate)
{
	pgssHashKey key;
	char	   *norm_query = NULL;
	int			encoding = GetDatabaseEncoding();
	int			query_len;

	Assert(query != NULL);

	elog(DEBUG1, "pgss_store: query=\"%s\" queryid=%u", query, queryId);

	/* Safety check... */
	if (!pgss || !pgss_hash)
		return;

	query_len = strlen(query);

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.queryid = queryId;

	LWLockAcquire(pgss->lock, LW_SHARED);
	/*
	 * for disabled mode, we did not need to search the rule table at all,
	 * this should be an optimization consideration here.
	 */
	if ((pgfw_mode == PGFW_MODE_ENFORCING) && to_be_prohibited(key.userid, key.queryid)) {
		stat_error_increment();
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("Prohibited SQL statement - sql firewall violation"),
				 errhint("SQL statement : %s", query)));
	} else if ((pgfw_mode == PGFW_MODE_PERMISSIVE) && to_be_prohibited(key.userid, key.queryid)) {
		ereport(WARNING,
				(errmsg("Prohibited SQL statement - sql firewall violation"), 
				 errhint("SQL statement : %s", query)));
		stat_warning_increment();
		goto done;
	} else if ((pgfw_mode == PGFW_MODE_LEARNING) && !lookup_whitelist(key.userid, key.queryid)) {
		/* Create new entry, if not present */
		Size		query_offset;
		int			gc_count;
		bool		stored;
		bool		do_gc;
		pgssEntry  *entry;

		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 */
		if (jstate)
		{
			norm_query = generate_normalized_query(jstate, query,
												   &query_len,
												   encoding);
		}

		/* Append new query text to file with only shared lock held */
		stored = qtext_store(norm_query ? norm_query : query, query_len,
							 &query_offset, &gc_count);

		/*
		 * Determine whether we need to garbage collect external query texts
		 * while the shared lock is still held.  This micro-optimization
		 * avoids taking the time to decide this while holding exclusive lock.
		 */
		do_gc = need_gc_qtexts();

		/* Need exclusive lock to make a new hashtable entry - promote */
		LWLockRelease(pgss->lock);
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

		/*
		 * A garbage collection may have occurred while we weren't holding the
		 * lock.  In the unlikely event that this happens, the query text we
		 * stored above will have been garbage collected, so write it again.
		 * This should be infrequent enough that doing it while holding
		 * exclusive lock isn't a performance problem.
		 */
		if (!stored || pgss->gc_count != gc_count)
			stored = qtext_store(norm_query ? norm_query : query, query_len,
								 &query_offset, NULL);

		/* If we failed to write to the text file, give up */
		if (!stored) {
			goto done;
		}

		/* OK to create a new hashtable entry */
		/* learned firewall rule is whitelist one */
		key.type = (uint32)PGFW_WHITELIST_ENTRY;
		entry = entry_alloc(&key, query_offset, query_len, encoding,
							jstate != NULL);
		entry->type = (uint32)PGFW_WHITELIST_ENTRY;

		/* If needed, perform garbage collection while exclusive lock held */
		if (do_gc)
			gc_qtexts();
	}

done:
	LWLockRelease(pgss->lock);

	/* We postpone this clean-up until we're out of the lock */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset all statement statistics.
 *
 * keep blacklist entries ?
 */
Datum
sql_firewall_reset(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use sql_firewall_reset"))));

	if (pgfw_mode != PGFW_MODE_DISABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_reset() is available only under the disable mode")));

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));
	entry_reset();

	update_firewall_rule_file();

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions */
#define SQL_FIREWALL_COLS			    6		/* maximum of above */
#define SQL_FIREWALL_CSV_COLS			6		/* maximum of above */

/*
 * Retrieve statement statistics.
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.  For 1.2 and later, the
 * expected API version is identified by embedding it in the C name of the
 * function.  Unfortunately we weren't bright enough to do that for 1.1.
 */
Datum
sql_firewall_statements(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, showtext);

	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements() */
static void
pg_stat_statements_internal(FunctionCallInfo fcinfo,
							bool showtext)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Oid			userid = GetUserId();
	bool		is_superuser = superuser();
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	Size		extent = 0;
	int			gc_count = 0;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * Check we have the expected number of output arguments.  Aside from
	 * being a good safety check, we need a kluge here to detect API version
	 * 1.1, which was wedged into the code in an ill-considered way.
	 */
	Assert(tupdesc->natts == SQL_FIREWALL_COLS);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * We'd like to load the query text file (if needed) while not holding any
	 * lock on pgss->lock.  In the worst case we'll have to do this again
	 * after we have the lock, but it's unlikely enough to make this a win
	 * despite occasional duplicated work.  We need to reload if anybody
	 * writes to the file (either a retail qtext_store(), or a garbage
	 * collection) between this point and where we've gotten shared lock.  If
	 * a qtext_store is actually in progress when we look, we might as well
	 * skip the speculative load entirely.
	 */
	if (showtext)
	{
		int			n_writers;

		/* Take the mutex so we can examine variables */
		{
			volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

			SpinLockAcquire(&s->mutex);
			extent = s->extent;
			n_writers = s->n_writers;
			gc_count = s->gc_count;
			SpinLockRelease(&s->mutex);
		}

		/* No point in loading file now if there are active writers */
		if (n_writers == 0)
			qbuffer = qtext_load_file(&qbuffer_size);
	}

	/*
	 * Get shared lock, load or reload the query text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	if (showtext)
	{
		/*
		 * Here it is safe to examine extent and gc_count without taking the
		 * mutex.  Note that although other processes might change
		 * pgss->extent just after we look at it, the strings they then write
		 * into the file cannot yet be referenced in the hashtable, so we
		 * don't care whether we see them or not.
		 *
		 * If qtext_load_file fails, we just press on; we'll return NULL for
		 * every query text.
		 */
		if (qbuffer == NULL ||
			pgss->extent != extent ||
			pgss->gc_count != gc_count)
		{
			if (qbuffer)
				free(qbuffer);
			qbuffer = qtext_load_file(&qbuffer_size);
		}
	}

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[SQL_FIREWALL_COLS];
		bool		nulls[SQL_FIREWALL_COLS];
		int			i = 0;
		Counters	tmp;
		int64		queryid = entry->key.queryid;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);

		if (is_superuser || entry->key.userid == userid)
		{
			values[i++] = Int64GetDatumFast(queryid);

			if (showtext)
			{
				char	   *qstr = qtext_fetch(entry->query_offset,
											   entry->query_len,
											   qbuffer,
											   qbuffer_size);

				if (qstr)
				{
					char	   *enc;

					enc = pg_any_to_server(qstr,
										   entry->query_len,
										   entry->encoding);

					values[i++] = CStringGetTextDatum(enc);

					if (enc != qstr)
						pfree(enc);
				}
				else
				{
					/* Just return a null if we fail to find the text */
					nulls[i++] = true;
				}
			}
			else
			{
				/* Query text not requested */
				nulls[i++] = true;
			}
		}
		else
		{
			/* Don't show queryid */
			nulls[i++] = true;

			/*
			 * Don't show query text, but hint as to the reason for not doing
			 * so if it was requested
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgssEntry *e = (volatile pgssEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		/* if (tmp.calls == 0)
		   continue; */

		values[i++] = Int64GetDatumFast(tmp.calls);
		values[i++] = Int64GetDatumFast(tmp.banned);
		/* rule entry type */
		values[i++] = CStringGetTextDatum(rule_typename(entry->type));

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	LWLockRelease(pgss->lock);

	if (qbuffer)
		free(qbuffer);

	tuplestore_donestoring(tupstore);
}

Datum
sql_firewall_stat_warning_count(PG_FUNCTION_ARGS)
{
	int64 count;

	volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

	SpinLockAcquire(&s->mutex);
	count = s->warning_count;
	SpinLockRelease(&s->mutex);

	PG_RETURN_INT64(count);
}

Datum
sql_firewall_stat_error_count(PG_FUNCTION_ARGS)
{
	int64 count;

	volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

	SpinLockAcquire(&s->mutex);
	count = s->error_count;
	SpinLockRelease(&s->mutex);

	PG_RETURN_INT64(count);
}

Datum
sql_firewall_stat_reset(PG_FUNCTION_ARGS)
{
	volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

	SpinLockAcquire(&s->mutex);
	s->warning_count = 0;
	s->error_count = 0;
	SpinLockRelease(&s->mutex);

	PG_RETURN_VOID();
}

/*
 * Export firewall rule in the sql_firewall_statements
 *
 * sql_firewall_export_rule() exports only part of pgssEntry members
 * (userid, queryid, query string, and number of calls) in CSV format.
 *
 * To import the rule, query_offset, query_len and encoding need to be
 * re-computed.
 */
Datum
sql_firewall_export_rule(PG_FUNCTION_ARGS)
{
	char	   *rule_file = text_to_cstring(PG_GETARG_TEXT_P(0));
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	FILE *filep;

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use sql_firewall_export_rule"))));

	if (pgfw_mode != PGFW_MODE_DISABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_export_rule() is available only under the disable mode")));

	elog(DEBUG1, "rule file=%s", rule_file);

	hash_seq_init(&hash_seq, pgss_hash);

	LWLockAcquire(pgss->lock, LW_SHARED);

	if (qbuffer)
		free(qbuffer);
	qbuffer = qtext_load_file(&qbuffer_size);

	filep = AllocateFile(rule_file, PG_BINARY_W);
	if (filep == NULL)
		ereport(ERROR,
			(errmsg("could not open file \"%s\": %m",
				rule_file)));

	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Counters	tmp;
		char	   *qstr;
		int		need_quote = 0;

		{
			volatile pgssEntry *e = (volatile pgssEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		qstr = qtext_fetch(entry->query_offset,
				   entry->query_len,
				   qbuffer,
				   qbuffer_size);

		if (qstr)
		{
			char *enc = pg_any_to_server(qstr,
					       entry->query_len,
					       entry->encoding);

			if (enc != qstr)
				pfree(enc);
		}

		if (strchr(qstr, '\n') != NULL || strchr(qstr, '\r') != NULL ||
		    strchr(qstr, ',') != NULL || strchr(qstr, '"') != NULL)
			need_quote = 1;

		fprintf(filep, "%d,%u,", entry->key.userid, entry->key.queryid);
		if (need_quote)
			fprintf(filep, "\"");

		{
			int i;
			for (i = 0 ; i < strlen(qstr) ; i++)
			{
				if (qstr[i] == '"')
					fputc('"', filep);
				fputc(qstr[i], filep);
			}
		}

		if (need_quote)
			fprintf(filep, "\"");

		fprintf(filep, ",%ld,%ld,%c\n", tmp.calls, tmp.banned, entry->type);

		//#ifdef NOT_USED
		elog(DEBUG1, "user=%d, queryid=%u, query=%s, len=%zd, query_len=%d, calls=%ld, "
			 "banned=%ld, type=%c",
			 entry->key.userid,
		     entry->key.queryid, qstr, strlen(qstr), entry->query_len, tmp.calls,
			 tmp.banned,
			 entry->type);
		//#endif
	}

	if (FreeFile(filep))
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not close file \"%s\": %m",
				rule_file)));

	LWLockRelease(pgss->lock);

	PG_RETURN_BOOL(true);
}

/*
 * rule type can be whitelist or blacklist
 *
 */
static bool
pgss_restore(Oid userid, uint32 queryid, const char *query, int64 calls,
			 int64 banned,
			 uint32 rule_type)
{
	pgssHashKey key = {0};
	pgssEntry  *entry;
	int			query_len;
	Size		query_offset;
	int			gc_count;
	bool		stored;
	int			encoding = GetDatabaseEncoding();
	bool ret = false;

	/*
	 * 10 | 3294787656 | select * from k1 where uid = ?; |     2
	 */
	key.userid  = userid;
	key.queryid = queryid;
	key.type    = rule_type;

	/*
	 * Check the entry in the hash table.
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);
	if (entry)
	{
		elog(DEBUG1, "userid %d, queryid %u type %u already exists.", key.userid, key.queryid, key.type);
		ret = true;
		goto done;
	}

	LWLockRelease(pgss->lock);
	/*
	 * Restore the entry
	 */
	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

	query_len = strlen(query);
	stored = qtext_store(query, query_len, &query_offset, &gc_count);
	if (!stored)
	{
		elog(ERROR, "Could not store a query text to the file.");
		goto done;
	}

	entry = entry_alloc(&key, query_offset, query_len, encoding, false);
	if (!entry)
	{
		elog(ERROR, "Could not allocate an entry in the hash table.");
		goto done;
	}

	entry->type = rule_type;

	SpinLockAcquire(&entry->mutex);
	entry->counters.calls  = calls;
	entry->counters.banned = banned;
	SpinLockRelease(&entry->mutex);

	if (need_gc_qtexts())
		gc_qtexts();

	ret = true;

done:
	LWLockRelease(pgss->lock);
	return ret;
}

#define CSV_DEFAULT     1
#define CSV_SEPARATOR   2
#define CSV_NON_QUOTED  3
#define CSV_QUOTED      4

static int
parse_csv_values(const char *buf, char **values)
{
	int i;
	int cols = 1;
	int off = 0;
	int buflen = strlen(buf);
	int len = 0;
	int mode = CSV_DEFAULT;

	int buf2len = 512;
	char *buf2 = palloc(buf2len);
	memset(buf2, 0, buf2len);

	/*
	 * the last character of the input buf is \n
	 */
	for (i = 0 ; i < buflen ; i++)
	{
		int field_end = false;

		switch (buf[i])
		{
			case '"':
				switch (mode)
				{
					case CSV_DEFAULT:
					case CSV_SEPARATOR:
						mode = CSV_QUOTED;
						off++;
						/* len should still be zero. */
						break;

					case CSV_NON_QUOTED:
						/* it's not correct csv format, but ok. */
						buf2[len++] = buf[i];
						break;

					case CSV_QUOTED:
						if (i < buflen - 1 && buf[i+1] == '"')
						{
							/* skip next char, and continue reading */
							i++;
							buf2[len++] = buf[i];
						}
						else
						{
							field_end = true;
						
							mode = CSV_DEFAULT;
						}
						break;
				}
				break;

			case '\n':
			case ',':
				switch (mode)
				{
					case CSV_DEFAULT:
						break;

					case CSV_SEPARATOR:
					case CSV_NON_QUOTED:
						/* end of the field */
						field_end = true;
						
						mode = CSV_SEPARATOR;
						break;

					case CSV_QUOTED:
						buf2[len++] = buf[i];
						break;
				}
				break;

			default:
				switch (mode)
				{
					case CSV_DEFAULT:
					case CSV_SEPARATOR:
						off = i;
						mode = CSV_NON_QUOTED;
						break;
				}
				buf2[len++] = buf[i];
				break;
		}
		if (field_end == true)
		{
			/* end of the field */
			values[cols-1] = palloc(len + 1);
			memset(values[cols-1], 0, len + 1);
			strncpy(values[cols-1], buf2, len);

			memset(buf2, 0, buf2len);
			
			off = i + 1;
			len = 0;
			cols++;

			field_end = false;
		}

		if (strlen(buf2) == buf2len - 1)
		{
			buf2 = repalloc(buf2, buf2len * 2);
			memset(buf2 + buf2len, 0, buf2len);
			buf2len = buf2len * 2;
		}
	}

	/*
	 * we should not take the last two characters " \n\0" as a field
	 */
	if ((len > 0) && (off > 0))
	{
		/* end of the field */
		values[cols-1] = palloc(len + 1);
		memset(values[cols-1], 0, len + 1);
		strncpy(values[cols-1], buf2, len);

		memset(buf2, 0, buf2len);
						
		cols++;
	}

	pfree(buf2);

	return cols - 1;
}

/*
 * the rules file may be sourced from a human maintained file,
 * so we re calculate the query id from the query text, and
 * ignore the query id.
 *
 *  */
Datum
sql_firewall_import_rule(PG_FUNCTION_ARGS)
{
	char	   *rule_file = text_to_cstring(PG_GETARG_TEXT_P(0));
	FILE *filep;
	bool  ret = false;
	char *buf, *line;
	size_t buflen;

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use sql_firewall_import_rule"))));

	if (pgfw_mode != PGFW_MODE_DISABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_import_rule() is available only under the disable mode")));

	{
		struct stat st;

		if (stat(rule_file, &st) != 0)
		{
			ereport(ERROR,
				(errmsg("could not stat file \"%s\": %m",
					rule_file)));
		}
		if (!S_ISREG(st.st_mode))
		{
			ereport(ERROR,
				(errmsg("\"%s\" is not a regular file",
					rule_file)));
		}
	}

	filep = AllocateFile(rule_file, PG_BINARY_R);
	if (filep == NULL)
		ereport(ERROR,
			(errmsg("could not open file \"%s\": %m",
				rule_file)));

	elog(DEBUG1, "sql_firewall_import_rule: file open, %s", rule_file);

	/*
	 * 10 | 3294787656 | select * from k1 where uid = ?; |     2
	ret = pgss_restore(10, 3294787656, "select * from k1 where uid = ?;", 7);
	 */

	buflen = 256;
	buf = palloc(buflen);

	line = NULL;
	while (fgets(buf, buflen, filep) != NULL)
	{
		int len;

		/*
		 * extend a line buffer while reading from the file.
		 */
		if (line == NULL)
		{
			line = pstrdup(buf);
		}
		else
		{
			len = strlen(line) + strlen(buf) + 1;
			line = repalloc(line, len);
			strncat(line, buf, len);
		}
		elog(DEBUG1, "line: %s", line);

		len = strlen(line);
		if (line[len-1] == '\r' || line[len-1] == '\n')
		{
			char *values[SQL_FIREWALL_CSV_COLS];

			/*
			 * if a complete csv record found, parse it, register to the rule,
			 * and free a memory space.
			 */
			if (parse_csv_values(line, values) == SQL_FIREWALL_CSV_COLS)
			{
				int j;
				uint32 queryid;
				char  *normalized_query = NULL;
				char  *query            = NULL;


				elog(DEBUG1, "sql_firewall_import_rule: complete csv record. ready for parsing.");

				/*
				 * no query id generated yet, the rule's query is a plain query with constant parameter
				 * not normalized, we calculate the query id from the plain query and normalize it.
				 */
				if (values[1] == NULL || strlen(values[1]) == 0)
				{
					/*
					 * calculate query id from query text
					 */
					queryid = sql_firewall_queryid(values[2], &normalized_query);
				}
				else
				{
					queryid = atol(values[1]);
				}
				query   = normalized_query ? normalized_query : values[2];
				ret = pgss_restore(atoi(values[0]),
								   queryid,
								   query,
								   atol(values[3]),
								   atol(values[4]),  /* values[4] is blacklist rule banned query times */
								   values[5][0]);    /* values[5][0] is entry type, either whitelist or
													  * blacklist, 'b' - blacklist, 'w' - whitelist.
													  */

				for (j = 0 ; j < SQL_FIREWALL_CSV_COLS ; j++)
				{
					elog(DEBUG1, "sql_firewall_import_rule: values[%d] = %s", j, values[j]);
					pfree(values[j]);
				}

				pfree(line);
				line = NULL;
				if (normalized_query)
					pfree(normalized_query);
			}
		}
	}

	if (line != NULL)
		pfree(line);

	if (FreeFile(filep))
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not close file \"%s\": %m",
				rule_file)));

	elog(DEBUG1, "sql_firewall_import_rule: file close, %s", rule_file);

	update_firewall_rule_file();

	PG_RETURN_BOOL(ret);
}

/*
 * Estimate shared memory space needed.
 */
static Size
pgss_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size = add_size(size, hash_estimate_size(pgss_max, sizeof(pgssEntry)));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgss->lock
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.  This is because pgss_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static pgssEntry *
entry_alloc(pgssHashKey *key, Size query_offset, int query_len, int encoding,
			bool sticky)
{
	pgssEntry  *entry;
	bool		found;

	/* Ignore queries which exeed the max limit of the learning table */
	if (hash_get_num_entries(pgss_hash) >= pgss_max)
	{
		ereport(WARNING,(errmsg("Number of queries exceeded the <sql_firewall.max> limit.")));
		return NULL;
	}

	/* Find or create an entry with desired hash code */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata */
		Assert(query_len >= 0);
		entry->query_offset = query_offset;
		entry->query_len = query_len;
		entry->encoding = encoding;
	}

	return entry;
}

#ifdef NOT_USED
/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgssEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgssEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}
#endif /* NOT_USED */

#ifdef NOT_USED
/*
 * Deallocate least used entries.
 * Caller must hold an exclusive lock on pgss->lock.
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry **entries;
	pgssEntry  *entry;
	int			nvictims;
	int			i;
	Size		totlen = 0;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.
	 */

	entries = palloc(hash_get_num_entries(pgss_hash) * sizeof(pgssEntry *));

	i = 0;
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate. */
		if (entry->counters.calls == 0)
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
		/* Accumulate total size, too. */
		totlen += entry->query_len + 1;
	}

	qsort(entries, i, sizeof(pgssEntry *), entry_cmp);

	if (i > 0)
	{
		/* Record the (approximate) median usage */
		pgss->cur_median_usage = entries[i / 2]->counters.usage;
		/* Record the mean query length */
		pgss->mean_query_len = totlen / i;
	}

	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgss_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);
}
#endif /* NOT_USED */

/*
 * Given a null-terminated string, allocate a new entry in the external query
 * text file and store the string there.
 *
 * Although we could compute the string length via strlen(), callers already
 * have it handy, so we require them to pass it too.
 *
 * If successful, returns true, and stores the new entry's offset in the file
 * into *query_offset.  Also, if gc_count isn't NULL, *gc_count is set to the
 * number of garbage collections that have occurred so far.
 *
 * On failure, returns false.
 *
 * At least a shared lock on pgss->lock must be held by the caller, so as
 * to prevent a concurrent garbage collection.  Share-lock-holding callers
 * should pass a gc_count pointer to obtain the number of garbage collections,
 * so that they can recheck the count after obtaining exclusive lock to
 * detect whether a garbage collection occurred (and removed this entry).
 */
static bool
qtext_store(const char *query, int query_len,
			Size *query_offset, int *gc_count)
{
	Size		off;
	int			fd;

	/*
	 * We use a spinlock to protect extent/n_writers/gc_count, so that
	 * multiple processes may execute this function concurrently.
	 */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		off = s->extent;
		s->extent += query_len + 1;
		s->n_writers++;
		if (gc_count)
			*gc_count = s->gc_count;
		SpinLockRelease(&s->mutex);
	}

	*query_offset = off;

	/* Now write the data into the successfully-reserved part of the file */
	fd = OpenTransientFile(PGSS_STATEMENTS_TEMP_FILE, O_RDWR | O_CREAT | PG_BINARY,
						   S_IRUSR | S_IWUSR);
	if (fd < 0)
		goto error;

	if (lseek(fd, off, SEEK_SET) != off)
		goto error;

	if (write(fd, query, query_len + 1) != query_len + 1)
		goto error;

	CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write sql_firewall file \"%s\": %m",
					PGSS_STATEMENTS_TEMP_FILE)));

	if (fd >= 0)
		CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return false;
}

/*
 * Read the external query text file into a malloc'd buffer.
 *
 * Returns NULL (without throwing an error) if unable to read, eg
 * file not there or insufficient memory.
 *
 * On success, the buffer size is also returned into *buffer_size.
 *
 * This can be called without any lock on pgss->lock, but in that case
 * the caller is responsible for verifying that the result is sane.
 */
static char *
qtext_load_file(Size *buffer_size)
{
	char	   *buf;
	int			fd;
	struct stat stat;

	fd = OpenTransientFile(PGSS_STATEMENTS_TEMP_FILE, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
				   errmsg("could not read sql_firewall file \"%s\": %m",
						  PGSS_STATEMENTS_TEMP_FILE)));
		return NULL;
	}

	/* Get file length */
	if (fstat(fd, &stat))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_TEMP_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/* Allocate buffer; beware that off_t might be wider than size_t */
	if (stat.st_size <= MaxAllocSize)
		buf = (char *) malloc(stat.st_size);
	else
		buf = NULL;
	if (buf == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.  If we get a short read and errno doesn't get
	 * set, the reason is probably that garbage collection truncated the file
	 * since we did the fstat(), so we don't log a complaint --- but we don't
	 * return the data, either, since it's most likely corrupt due to
	 * concurrent writes from garbage collection.
	 */
	errno = 0;
	if (read(fd, buf, stat.st_size) != stat.st_size)
	{
		if (errno)
			ereport(LOG,
					(errcode_for_file_access(),
				   errmsg("could not read sql_firewall file \"%s\": %m",
						  PGSS_STATEMENTS_TEMP_FILE)));
		free(buf);
		CloseTransientFile(fd);
		return NULL;
	}

	CloseTransientFile(fd);

	*buffer_size = stat.st_size;
	return buf;
}

/*
 * Locate a query text in the file image previously read by qtext_load_file().
 *
 * We validate the given offset/length, and return NULL if bogus.  Otherwise,
 * the result points to a null-terminated string within the buffer.
 */
static char *
qtext_fetch(Size query_offset, int query_len,
			char *buffer, Size buffer_size)
{
	/* File read failed? */
	if (buffer == NULL)
		return NULL;
	/* Bogus offset/length? */
	if (query_len < 0 ||
		query_offset + query_len >= buffer_size)
		return NULL;
	/* As a further sanity check, make sure there's a trailing null */
	if (buffer[query_offset + query_len] != '\0')
		return NULL;
	/* Looks OK */
	return buffer + query_offset;
}

/*
 * Do we need to garbage-collect the external query text file?
 *
 * Caller should hold at least a shared lock on pgss->lock.
 */
static bool
need_gc_qtexts(void)
{
	Size		extent;

	/* Read shared extent pointer */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		extent = s->extent;
		SpinLockRelease(&s->mutex);
	}

	/* Don't proceed if file does not exceed 512 bytes per possible entry */
	if (extent < 512 * pgss_max)
		return false;

	/*
	 * Don't proceed if file is less than about 50% bloat.  Nothing can or
	 * should be done in the event of unusually large query texts accounting
	 * for file's large size.  We go to the trouble of maintaining the mean
	 * query length in order to prevent garbage collection from thrashing
	 * uselessly.
	 */
	if (extent < pgss->mean_query_len * pgss_max * 2)
		return false;

	return true;
}

/*
 * Garbage-collect orphaned query texts in external file.
 *
 * This won't be called often in the typical case, since it's likely that
 * there won't be too much churn, and besides, a similar compaction process
 * occurs when serializing to disk at shutdown or as part of resetting.
 * Despite this, it seems prudent to plan for the edge case where the file
 * becomes unreasonably large, with no other method of compaction likely to
 * occur in the foreseeable future.
 *
 * The caller must hold an exclusive lock on pgss->lock.
 */
static void
gc_qtexts(void)
{
	char	   *qbuffer;
	Size		qbuffer_size;
	FILE	   *qfile;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	Size		extent;
	int			nentries;

	/*
	 * When called from pgss_store, some other session might have proceeded
	 * with garbage collection in the no-lock-held interim of lock strength
	 * escalation.  Check once more that this is actually necessary.
	 */
	if (!need_gc_qtexts())
		return;

	/*
	 * Load the old texts file.  If we fail (out of memory, for instance) just
	 * skip the garbage collection.
	 */
	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		return;

	/*
	 * We overwrite the query texts file in place, so as to reduce the risk of
	 * an out-of-disk-space failure.  Since the file is guaranteed not to get
	 * larger, this should always work on traditional filesystems; though we
	 * could still lose on copy-on-write filesystems.
	 */
	qfile = AllocateFile(PGSS_STATEMENTS_TEMP_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_TEMP_FILE)));
		goto gc_fail;
	}

	extent = 0;
	nentries = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			query_len = entry->query_len;
		char	   *qry = qtext_fetch(entry->query_offset,
									  query_len,
									  qbuffer,
									  qbuffer_size);

		if (qry == NULL)
		{
			/* Trouble ... drop the text */
			entry->query_offset = 0;
			entry->query_len = -1;
			continue;
		}

		if (fwrite(qry, 1, query_len + 1, qfile) != query_len + 1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
				  errmsg("could not write sql_firewall file \"%s\": %m",
						 PGSS_STATEMENTS_TEMP_FILE)));
			hash_seq_term(&hash_seq);
			goto gc_fail;
		}

		entry->query_offset = extent;
		extent += query_len + 1;
		nentries++;
	}

	/*
	 * Truncate away any now-unused space.  If this fails for some odd reason,
	 * we log it, but there's no need to fail.
	 */
	if (ftruncate(fileno(qfile), extent) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
			   errmsg("could not truncate sql_firewall file \"%s\": %m",
					  PGSS_STATEMENTS_TEMP_FILE)));

	if (FreeFile(qfile))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_TEMP_FILE)));
		qfile = NULL;
		goto gc_fail;
	}

	elog(DEBUG1, "pgss gc of queries file shrunk size from %zu to %zu",
		 pgss->extent, extent);

	/* Reset the shared extent pointer */
	pgss->extent = extent;

	/*
	 * Also update the mean query length, to be sure that need_gc_qtexts()
	 * won't still think we have a problem.
	 */
	if (nentries > 0)
		pgss->mean_query_len = extent / nentries;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	free(qbuffer);

	/*
	 * OK, count a garbage collection cycle.  (Note: even though we have
	 * exclusive lock on pgss->lock, we must take pgss->mutex for this, since
	 * other processes may examine gc_count while holding only the mutex.
	 * Also, we have to advance the count *after* we've rewritten the file,
	 * else other processes might not realize they read a stale file.)
	 */
	record_gc_qtexts();

	return;

gc_fail:
	/* clean up resources */
	if (qfile)
		FreeFile(qfile);
	if (qbuffer)
		free(qbuffer);

	/*
	 * Since the contents of the external file are now uncertain, mark all
	 * hashtable entries as having invalid texts.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entry->query_offset = 0;
		entry->query_len = -1;
	}

	/* Seems like a good idea to bump the GC count even though we failed */
	record_gc_qtexts();
}

/*
 * Release all entries.
 */
static void
entry_reset(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	FILE	   *qfile;

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
	}

	/*
	 * Write new empty query file, perhaps even creating a new one to recover
	 * if the file was missing.
	 */
	qfile = AllocateFile(PGSS_STATEMENTS_TEMP_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create sql_firewall file \"%s\": %m",
						PGSS_STATEMENTS_TEMP_FILE)));
		goto done;
	}

	/* If ftruncate fails, log it, but it's not a fatal problem */
	if (ftruncate(fileno(qfile), 0) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
			   errmsg("could not truncate sql_firewall file \"%s\": %m",
					  PGSS_STATEMENTS_TEMP_FILE)));

	FreeFile(qfile);

done:
	pgss->extent = 0;
	/* This counts as a query text garbage collection for our purposes */
	record_gc_qtexts();

	LWLockRelease(pgss->lock);
}

/*
 * AppendJumble: Append a value that is substantive in a given query to
 * the current jumble.
 */
static void
AppendJumble(pgssJumbleState *jstate, const unsigned char *item, Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * Whenever the jumble buffer is full, we hash the current contents and
	 * reset the buffer to contain just that hash value, thus relying on the
	 * hash to summarize everything so far.
	 */
	while (size > 0)
	{
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE)
		{
			uint32		start_hash = hash_any(jumble, JUMBLE_SIZE);

			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;
	}
	jstate->jumble_len = jumble_len;
}

/*
 * Wrappers around AppendJumble to encapsulate details of serialization
 * of individual local variable elements.
 */
#define APP_JUMB(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define APP_JUMB_STRING(str) \
	AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1)

/*
 * JumbleQuery: Selectively serialize the query tree, appending significant
 * data to the "query jumble" while ignoring nonsignificant data.
 *
 * Rule of thumb for what to include is that we should ignore anything not
 * semantically significant (such as alias names) as well as anything that can
 * be deduced from child nodes (else we'd just be double-hashing that piece
 * of information).
 */
void
JumbleQuery(pgssJumbleState *jstate, Query *query)
{
	Assert(IsA(query, Query));
	Assert(query->utilityStmt == NULL);

	APP_JUMB(query->commandType);
	/* resultRelation is usually predictable from commandType */
	JumbleExpr(jstate, (Node *) query->cteList);
	JumbleRangeTable(jstate, query->rtable);
	JumbleExpr(jstate, (Node *) query->jointree);
	JumbleExpr(jstate, (Node *) query->targetList);
	JumbleExpr(jstate, (Node *) query->returningList);
	JumbleExpr(jstate, (Node *) query->groupClause);
	JumbleExpr(jstate, query->havingQual);
	JumbleExpr(jstate, (Node *) query->windowClause);
	JumbleExpr(jstate, (Node *) query->distinctClause);
	JumbleExpr(jstate, (Node *) query->sortClause);
	JumbleExpr(jstate, query->limitOffset);
	JumbleExpr(jstate, query->limitCount);
	/* we ignore rowMarks */
	JumbleExpr(jstate, query->setOperations);
}

/*
 * Jumble a range table
 */
static void
JumbleRangeTable(pgssJumbleState *jstate, List *rtable)
{
	ListCell   *lc;
	Relation rel;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		Assert(IsA(rte, RangeTblEntry));
		APP_JUMB(rte->rtekind);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				rel = RelationIdGetRelation(rte->relid);
				APP_JUMB_STRING(RelationGetRelationName(rel));
				RelationClose(rel);
				break;
			case RTE_SUBQUERY:
				JumbleQuery(jstate, rte->subquery);
				break;
			case RTE_JOIN:
				APP_JUMB(rte->jointype);
				break;
			case RTE_FUNCTION:
				JumbleExpr(jstate, (Node *) rte->functions);
				break;
			case RTE_VALUES:
				JumbleExpr(jstate, (Node *) rte->values_lists);
				break;
			case RTE_CTE:

				/*
				 * Depending on the CTE name here isn't ideal, but it's the
				 * only info we have to identify the referenced WITH item.
				 */
				APP_JUMB_STRING(rte->ctename);
				APP_JUMB(rte->ctelevelsup);
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}
	}
}

/*
 * Jumble an expression tree
 *
 * In general this function should handle all the same node types that
 * expression_tree_walker() does, and therefore it's coded to be as parallel
 * to that function as possible.  However, since we are only invoked on
 * queries immediately post-parse-analysis, we need not handle node types
 * that only appear in planning.
 *
 * Note: the reason we don't simply use expression_tree_walker() is that the
 * point of that function is to support tree walkers that don't care about
 * most tree node types, but here we care about all types.  We should complain
 * about any unrecognized node type.
 */
static void
JumbleExpr(pgssJumbleState *jstate, Node *node)
{
	ListCell   *temp;

	if (node == NULL)
		return;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/*
	 * We always emit the node's NodeTag, then any additional fields that are
	 * considered significant, and then we recurse to any child nodes.
	 */
	APP_JUMB(node->type);

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				APP_JUMB(var->varno);
				APP_JUMB(var->varattno);
				APP_JUMB(var->varlevelsup);
			}
			break;
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/* We jumble only the constant's type, not its value */
				APP_JUMB(c->consttype);
				/* Also, record its parse location for query normalization */
				RecordConstLocation(jstate, c->location);
			}
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				APP_JUMB(p->paramkind);
				APP_JUMB(p->paramid); /* FIXME */
				APP_JUMB(p->paramtype);
			}
			break;
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				APP_JUMB(expr->aggfnoid); /* FIXME */
				JumbleExpr(jstate, (Node *) expr->aggdirectargs);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggorder);
				JumbleExpr(jstate, (Node *) expr->aggdistinct);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				APP_JUMB(expr->winfnoid); /* FIXME */
				APP_JUMB(expr->winref);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;

				JumbleExpr(jstate, (Node *) aref->refupperindexpr);
				JumbleExpr(jstate, (Node *) aref->reflowerindexpr);
				JumbleExpr(jstate, (Node *) aref->refexpr);
				JumbleExpr(jstate, (Node *) aref->refassgnexpr);
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				char *funcname = get_func_name(expr->funcid);

				APP_JUMB_STRING(funcname);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_NamedArgExpr:
			{
				NamedArgExpr *nae = (NamedArgExpr *) node;

				APP_JUMB(nae->argnumber);
				JumbleExpr(jstate, (Node *) nae->arg);
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				APP_JUMB(expr->opno);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				APP_JUMB(expr->opno);
				APP_JUMB(expr->useOr);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				APP_JUMB(expr->boolop);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				APP_JUMB(sublink->subLinkType);
				JumbleExpr(jstate, (Node *) sublink->testexpr);
				JumbleQuery(jstate, (Query *) sublink->subselect);
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fs = (FieldSelect *) node;

				APP_JUMB(fs->fieldnum);
				JumbleExpr(jstate, (Node *) fs->arg);
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				JumbleExpr(jstate, (Node *) fstore->arg);
				JumbleExpr(jstate, (Node *) fstore->newvals);
			}
			break;
		case T_RelabelType:
			{
				RelabelType *rt = (RelabelType *) node;

				APP_JUMB(rt->resulttype);
				JumbleExpr(jstate, (Node *) rt->arg);
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *cio = (CoerceViaIO *) node;

				APP_JUMB(cio->resulttype);
				JumbleExpr(jstate, (Node *) cio->arg);
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acexpr = (ArrayCoerceExpr *) node;

				APP_JUMB(acexpr->resulttype);
				JumbleExpr(jstate, (Node *) acexpr->arg);
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *crexpr = (ConvertRowtypeExpr *) node;

				APP_JUMB(crexpr->resulttype);
				JumbleExpr(jstate, (Node *) crexpr->arg);
			}
			break;
		case T_CollateExpr:
			{
				CollateExpr *ce = (CollateExpr *) node;

				APP_JUMB(ce->collOid); /* FIXME */
				JumbleExpr(jstate, (Node *) ce->arg);
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				JumbleExpr(jstate, (Node *) caseexpr->arg);
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);

					Assert(IsA(when, CaseWhen));
					JumbleExpr(jstate, (Node *) when->expr);
					JumbleExpr(jstate, (Node *) when->result);
				}
				JumbleExpr(jstate, (Node *) caseexpr->defresult);
			}
			break;
		case T_CaseTestExpr:
			{
				CaseTestExpr *ct = (CaseTestExpr *) node;

				APP_JUMB(ct->typeId);
			}
			break;
		case T_ArrayExpr:
			JumbleExpr(jstate, (Node *) ((ArrayExpr *) node)->elements);
			break;
		case T_RowExpr:
			JumbleExpr(jstate, (Node *) ((RowExpr *) node)->args);
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				APP_JUMB(rcexpr->rctype);
				JumbleExpr(jstate, (Node *) rcexpr->largs);
				JumbleExpr(jstate, (Node *) rcexpr->rargs);
			}
			break;
		case T_CoalesceExpr:
			JumbleExpr(jstate, (Node *) ((CoalesceExpr *) node)->args);
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *mmexpr = (MinMaxExpr *) node;

				APP_JUMB(mmexpr->op);
				JumbleExpr(jstate, (Node *) mmexpr->args);
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				APP_JUMB(xexpr->op);
				JumbleExpr(jstate, (Node *) xexpr->named_args);
				JumbleExpr(jstate, (Node *) xexpr->args);
			}
			break;
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				APP_JUMB(nt->nulltesttype);
				JumbleExpr(jstate, (Node *) nt->arg);
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *bt = (BooleanTest *) node;

				APP_JUMB(bt->booltesttype);
				JumbleExpr(jstate, (Node *) bt->arg);
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *cd = (CoerceToDomain *) node;

				APP_JUMB(cd->resulttype);
				JumbleExpr(jstate, (Node *) cd->arg);
			}
			break;
		case T_CoerceToDomainValue:
			{
				CoerceToDomainValue *cdv = (CoerceToDomainValue *) node;

				APP_JUMB(cdv->typeId); /* FIXME */
			}
			break;
		case T_SetToDefault:
			{
				SetToDefault *sd = (SetToDefault *) node;

				APP_JUMB(sd->typeId); /* FIXME */
			}
			break;
		case T_CurrentOfExpr:
			{
				CurrentOfExpr *ce = (CurrentOfExpr *) node;

				APP_JUMB(ce->cvarno);
				if (ce->cursor_name)
					APP_JUMB_STRING(ce->cursor_name);
				APP_JUMB(ce->cursor_param);
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				APP_JUMB(tle->resno);
				APP_JUMB(tle->ressortgroupref);
				JumbleExpr(jstate, (Node *) tle->expr);
			}
			break;
		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) node;

				APP_JUMB(rtr->rtindex);
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				APP_JUMB(join->jointype);
				APP_JUMB(join->isNatural);
				APP_JUMB(join->rtindex);
				JumbleExpr(jstate, join->larg);
				JumbleExpr(jstate, join->rarg);
				JumbleExpr(jstate, join->quals);
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				JumbleExpr(jstate, (Node *) from->fromlist);
				JumbleExpr(jstate, from->quals);
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				JumbleExpr(jstate, (Node *) lfirst(temp));
			}
			break;
		case T_SortGroupClause:
			{
				SortGroupClause *sgc = (SortGroupClause *) node;

				APP_JUMB(sgc->tleSortGroupRef);
				APP_JUMB(sgc->eqop);
				APP_JUMB(sgc->sortop);
				APP_JUMB(sgc->nulls_first);
			}
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				APP_JUMB(wc->winref);
				APP_JUMB(wc->frameOptions);
				JumbleExpr(jstate, (Node *) wc->partitionClause);
				JumbleExpr(jstate, (Node *) wc->orderClause);
				JumbleExpr(jstate, wc->startOffset);
				JumbleExpr(jstate, wc->endOffset);
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				/* we store the string name because RTE_CTE RTEs need it */
				APP_JUMB_STRING(cte->ctename);
				JumbleQuery(jstate, (Query *) cte->ctequery);
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				APP_JUMB(setop->op);
				APP_JUMB(setop->all);
				JumbleExpr(jstate, setop->larg);
				JumbleExpr(jstate, setop->rarg);
			}
			break;
		case T_RangeTblFunction:
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) node;

				JumbleExpr(jstate, rtfunc->funcexpr);
			}
			break;
		default:
			/* Only a warning, since we can stumble along anyway */
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * Record location of constant within query string of query tree
 * that is currently being walked.
 */
static void
RecordConstLocation(pgssJumbleState *jstate, int location)
{
	/* -1 indicates unknown or undefined location */
	if (location >= 0)
	{
		/* enlarge array if needed */
		if (jstate->clocations_count >= jstate->clocations_buf_size)
		{
			jstate->clocations_buf_size *= 2;
			jstate->clocations = (pgssLocationLen *)
				repalloc(jstate->clocations,
						 jstate->clocations_buf_size *
						 sizeof(pgssLocationLen));
		}
		jstate->clocations[jstate->clocations_count].location = location;
		/* initialize lengths to -1 to simplify fill_in_constant_lengths */
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length (which cannot be longer) on exit.
 *
 * Returns a palloc'd string.
 */
static char *
generate_normalized_query(pgssJumbleState *jstate, const char *query,
						  int *query_len_p, int encoding)
{
	char	   *norm_query;
	int			query_len = *query_len_p;
	int			i,
				len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				n_quer_loc = 0, /* Normalized query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;		/* Length (in bytes) of that tok */

	/*
	 * Get constants' lengths (core system only gives us locations).  Note
	 * this also ensures the items are sorted by location.
	 */
	fill_in_constant_lengths(jstate, query);

	/* Allocate result buffer */
	norm_query = palloc(query_len + 1);

	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		off = jstate->clocations[i].location;
		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;

		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/* And insert a '?' in place of the constant token */
		norm_query[n_quer_loc++] = '?';

		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= query_len);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Given a valid SQL string and an array of constant-location records,
 * fill in the textual lengths of those constants.
 *
 * The constants may use any allowed constant syntax, such as float literals,
 * bit-strings, single-quoted strings and dollar-quoted strings.  This is
 * accomplished by using the public API for the core scanner.
 *
 * It is the caller's job to ensure that the string is a valid SQL statement
 * with constants at the indicated locations.  Since in practice the string
 * has already been parsed, and the locations that the caller provides will
 * have originated from within the authoritative parser, this should not be
 * a problem.
 *
 * Duplicate constant pointers are possible, and will have their lengths
 * marked as '-1', so that they are later ignored.  (Actually, we assume the
 * lengths were initialized as -1 to start with, and don't change them here.)
 *
 * N.B. There is an assumption that a '-' character at a Const location begins
 * a negative numeric constant.  This precludes there ever being another
 * reason for a constant to start with a '-'.
 */
static void
fill_in_constant_lengths(pgssJumbleState *jstate, const char *query)
{
	pgssLocationLen *locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			last_loc = -1;
	int			i;

	/*
	 * Sort the records by location so that we can process them in order while
	 * scanning the query text.
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			  sizeof(pgssLocationLen), comp_location);
	locs = jstate->clocations;

	/* initialize the flex scanner --- should match raw_parser() */
	yyscanner = scanner_init(query,
							 &yyextra,
							 ScanKeywords,
							 NumScanKeywords);

	/* Search for each constant, in sequence */
	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			loc = locs[i].location;
		int			tok;

		Assert(loc >= 0);

		if (loc <= last_loc)
			continue;			/* Duplicate constant, ignore */

		/* Lex tokens until we find the desired constant */
		for (;;)
		{
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely */
			if (tok == 0)
				break;			/* out of inner for-loop */

			/*
			 * We should find the token position exactly, but if we somehow
			 * run past it, work with that.
			 */
			if (yylloc >= loc)
			{
				if (query[loc] == '-')
				{
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 *
					 * Do not compensate for the core system's special-case
					 * adjustment of location to that of the leading '-'
					 * operator in the event of a negative constant.  It is
					 * also useful for our purposes to start from the minus
					 * symbol.  In this way, queries like "select * from foo
					 * where bar = 1" and "select * from foo where bar = -2"
					 * will have identical normalized query strings.
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* out of inner for-loop */
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* out of inner for-loop */
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1 */
		if (tok == 0)
			break;

		last_loc = loc;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: comparator for qsorting pgssLocationLen structs by location
 */
static int
comp_location(const void *a, const void *b)
{
	int			l = ((const pgssLocationLen *) a)->location;
	int			r = ((const pgssLocationLen *) b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return +1;
	else
		return 0;
}

/*
 * parse a sql firewall rule engine name to its engine id
 *
 * TODO: capital consideration
 */
static uint32
rule_typeid(const char *rule_type_name)
{
	uint32 rule_type = (uint32)PGFW_DUMMY_ENTRY;

	for (int i = 0; rule_type_options[i].name != NULL; i++)
	{
		if (!strcmp(rule_type_name, rule_type_options[i].name))
		{
			rule_type = rule_type_options[i].val;
			break;
		}
	}

	return rule_type;
}

/*
 * used for user display only, make a more readable display
 */
static char *
rule_typename(char rule_type)
{
	char *typename = "unknown";

	switch (rule_type)
	{
	case PGFW_DUMMY_ENTRY:
		typename = "dummy";
		break;
	case PGFW_WHITELIST_ENTRY:
		typename = "whitelist";
		break;
	case PGFW_BLACKLIST_ENTRY:
		typename = "blacklist";
		break;
	}

	return typename;
}

/*
 * add a sql firewall rule, it is a blacklist rule. This rule would
 * prohibit user to query database.
 *
 * user name is empty '', means the rule would prohibit all user's query.
 *
 * engine can be one of [white, black]:
 *   white: means it is a rule entry to be inserted to whitelist
 *   black: means it is a rule entry to be inserted to blacklist
 *
 */
Datum
sql_firewall_add_rule(PG_FUNCTION_ARGS)
{
	char	   *username        = text_to_cstring(PG_GETARG_TEXT_P(0));
	char	   *query_string    = text_to_cstring(PG_GETARG_TEXT_P(1));
	char	   *rule_type_name  = text_to_cstring(PG_GETARG_TEXT_P(2));
	uint32	    rule_type       = rule_typeid(rule_type_name);

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use sql_firewall_add_rule"))));

	if (pgfw_mode != PGFW_MODE_DISABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_add_rule() is available only under the disable mode")));

	if (rule_type == PGFW_DUMMY_ENTRY)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_add_rule() engine must be one of [\'whitelist\', \'blacklist\']")));


	add_rule(username, query_string, rule_type);

	PG_RETURN_BOOL(true);
}

/*
 * delete a sql firewall rule, it is a blacklist rule.
 *
 * user name is empty '', means the rule would prohibit all user's query.
 *
 * engine can be one of [white, black, both]:
 *   white: means only delete the mathed rule entry from whitelist
 *   black: means only delete the mathed rule entry from blacklist
 *   both : means only delete the mathed rule entry from whitelist and blacklist
 */
Datum
sql_firewall_del_rule(PG_FUNCTION_ARGS)
{
	char	   *username        = text_to_cstring(PG_GETARG_TEXT_P(0));
	char	   *query_string    = text_to_cstring(PG_GETARG_TEXT_P(1));
	char	   *rule_type_name  = text_to_cstring(PG_GETARG_TEXT_P(2));
	uint32	    rule_type       = rule_typeid(rule_type_name);

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sql_firewall must be loaded via shared_preload_libraries")));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use sql_firewall_del_rule"))));

	if (pgfw_mode != PGFW_MODE_DISABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("sql_firewall_del_rule() is available only under the disable mode")));

	del_rule(username, query_string, rule_type);

	PG_RETURN_BOOL(true);
}

static int
__add_rule(Oid user, uint32 queryid, const char *query_string, uint32 rule_type)
{
	bool ret;

	elog(DEBUG1, "sql firewall: __add_rule: [:user_id %u, :query_id %u, query: %s, rule_type: %c]",
		 user, queryid, query_string, rule_type);

	ret = pgss_restore(user,
					   queryid,
					   query_string,
					   0,
					   0,
					   rule_type);

	elog(DEBUG1, "sql firewall: __add_rule: result %d", ret);

	return 0;
}

/*
 * add a firewall rule
 */
static int
add_rule(const char* user, const char *query_string, uint32 rule_type)
{
	Oid          userid;
	uint32       queryid;
	int          ret;
	char        *normalized_query = NULL;
	char        *query            = NULL;

	/*
	 * InvalidOid as user id, means any user.
	 */
	userid   = get_role_oid(user, true);
	queryid  = sql_firewall_queryid(query_string, &normalized_query);
	query    = normalized_query ? normalized_query : (char *)query_string;
	ret      = __add_rule(userid, queryid, query, rule_type);
	
	if (normalized_query)
		pfree(normalized_query);

	return ret;
}

/*
 * delete an entry of the given key (userid, queryid) from the hash table.
 */
static int
entry_delete(Oid userid, uint32 queryid, uint32 rule_type)
{
	pgssHashKey key;

	/*
	 * 10 | 3294787656 | select * from k1 where uid = ?; |     2
	 */
	key.userid   = userid;
	key.queryid  = queryid;
	key.type     = rule_type;

	/*
	 * remove the entry from the hash table.
	 */
	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
	hash_search(pgss_hash, &key, HASH_REMOVE, NULL);
	LWLockRelease(pgss->lock);

	return 0;
}

static int
__del_rule(Oid userid, uint32 queryid, const char *query_string, uint32 rule_type)
{
	int      ret;

	elog(DEBUG1, "sql firewall: __del_rule: [:user_id %u, :query_id %u, query: %s, rule_type:%c]",
		 userid, queryid, query_string, rule_type);

	ret = entry_delete(userid, queryid, rule_type);

	elog(DEBUG1, "sql firewall: __del_rule: result %d", ret);

	return ret;
}

/*
 * delete a firewall rule
 */
static int
del_rule(const char* user, const char *query_string, uint32 rule_type)
{
	Oid          userid;
	uint32       queryid;
	int          ret;

	/*
	 * InvalidOid as user id, means any user.
	 */
	userid   = get_role_oid(user, true);
	queryid  = sql_firewall_queryid(query_string, NULL);
	ret      = __del_rule(userid, queryid, query_string, rule_type);
	
	return ret;
}




/*
 * calculate a query's queryid
 *
 * called when the database administrator import sql firewall rules or insert a
 * sql firewall rule manually.
 *
 * in:
 *   query_string
 *
 * out:
 *   normalized_query
 */
static uint32
sql_firewall_queryid(const char *query_string, char **normalized_query)
{
	List         *parsetree = pg_parse_query(query_string);
	Node	     *parsenode = NULL;
	Query	     *query     = NULL;
	pgssJumbleState jstate  = {0};
	uint32        queryid   = 0;

	if (list_length(parsetree) != 1) {
		elog(ERROR, "sql firewall: error - statement result in only one parsetree is supported."
			 "but \'%s\' result in %d.", query_string, list_length(parsetree));
	}

	parsenode = (Node *) linitial(parsetree);
	query     = parse_analyze(parsenode, query_string, NULL, 0);

	/*
	 * calculate the query id of the given query
	 */
	/* Set up workspace for query jumbling */
	jstate.jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate.jumble_len = 0;
	jstate.clocations_buf_size = 32;
	jstate.clocations = (pgssLocationLen *)
		palloc(jstate.clocations_buf_size * sizeof(pgssLocationLen));
	jstate.clocations_count = 0;

	/* Compute query ID and mark the Query node with it */
	JumbleQuery(&jstate, query);
	queryid = hash_any(jstate.jumble, jstate.jumble_len);

	/*
	 * If we are unlucky enough to get a hash of zero, use 1 instead, to
	 * prevent confusion with the utility-statement case.
	 */
	if (queryid == 0)
		queryid = 1;

	if (normalized_query) {
		int encoding  = GetDatabaseEncoding();
		int query_len = strlen(query_string);

		*normalized_query = generate_normalized_query(&jstate,
													  query_string,
													  &query_len,
													  encoding);
	}

	pfree(jstate.jumble);
	pfree(jstate.clocations);

	return queryid;
}


/*
 * we count hit also for PERMISSIVE mode
 */
static void
collect_entry_statistics(pgssEntry *entry)
{
	/*
	 * Grab the spinlock while updating the counters (see comment about
	 * locking rules at the head of the file)
	 */
	/*
	 * If the entry is not found, it means the statement is not in
	 * the firewall list.
	 */
	if (!entry)
		return;

	SpinLockAcquire(&entry->mutex);

	switch (entry->type) {
	case PGFW_WHITELIST_ENTRY:
		entry->counters.calls++;
		break;
	case PGFW_BLACKLIST_ENTRY:
		entry->counters.banned++;
		break;
	}

	SpinLockRelease(&entry->mutex);
}

/*
 * the core logic of the sql firewall rule engine is here.
 *
 * and collect necessary staticstics.
 */
static bool
__to_be_prohibited(pgssEntry *whitelist_entry, pgssEntry *blacklist_entry)
{
	bool  whitelist_hit = (whitelist_entry != NULL);
	bool  blacklist_hit = (blacklist_entry != NULL);
	bool  prohibited    = true;

	switch (pgfw_rule_engine) {
	case PGFW_ENGINE_WHITELIST:
		prohibited = !whitelist_hit;
		collect_entry_statistics(whitelist_entry);
		break;
	case PGFW_ENGINE_BLACKLIST:
		prohibited = blacklist_hit;
		collect_entry_statistics(blacklist_entry);
		break;
	case PGFW_ENGINE_HYBRID:
		prohibited = (!whitelist_hit || blacklist_hit);
		if (whitelist_hit && !blacklist_hit) {
			/*
			 * this is the only case of hybrid rule engine that allow a query
			 */
			collect_entry_statistics(whitelist_entry);
		} else if (blacklist_hit) {
			/*
			 * query is being banned by the blacklist entry
			 */
			collect_entry_statistics(blacklist_entry);
		}
		break;
	default:
		elog(ERROR, "sql firewall: error must have a rule engine when it is enabled.");
	}
	
	return prohibited;
}

/*
 * search the rule table for both whitelist or blacklist
 *
 */
static pgssEntry *
__lookup_rule(pgssHashKey *key)
{
	pgssEntry   *entry = NULL;

	entry = (pgssEntry *)hash_search(pgss_hash, key, HASH_FIND, NULL);

	return entry;
}


/*
 * caller should hold lock of the hash table
 *
 */
static pgssEntry *
lookup_rule(Oid userid, uint32 queryid, uint32 rule_type)
{
	pgssHashKey  key   = {0};
	pgssEntry   *entry = NULL;

	/*
	 * fill the common key field
	 */
	key.queryid    = queryid;
	key.type       = rule_type;

	/*
	 * try exactly matched entry for a specified user
	 *
	 * userid == InvalidOid, means the rule should be applied to
	 * all users.
	 */
	if (userid != InvalidOid) {
		key.userid     = userid;
		entry           = __lookup_rule(&key);
		
		if (entry != NULL)
			return entry;
	}

	/*
	 * then try the rule should be applied to all users.
	 */
	key.userid = InvalidOid;
	entry       = __lookup_rule(&key);

	return entry;
}

/*
 * lookup if there exists matched rule entry for (userid, queryid)
 * in the whitelist rule.
 *
 */
static pgssEntry *
lookup_whitelist(Oid userid, uint32 queryid)
{
	return lookup_rule(userid, queryid, (uint32)PGFW_WHITELIST_ENTRY);
}

/*
 * lookup if there exists matched rule entry for (userid, queryid)
 * in the blacklist rule.
 *
 */
static pgssEntry *
lookup_blacklist(Oid userid, uint32 queryid)
{
	return lookup_rule(userid, queryid, (uint32)PGFW_BLACKLIST_ENTRY);
}

/*
 * given a (userid, queryid) vector, we decide whether to prohibit
 * the query
 *
 * return:
 *   false   :   allow to execute the query
 *   true    :   prohibit to execute
 *
 * out:
 *   entry   :   the matched rule entry, which is applying to the query
 *
 * note:
 *   caller should has held at least a shared lock on pgss
 */
static bool
to_be_prohibited(Oid userid, uint32 queryid)
{
	pgssEntry    *whitelist_entry = NULL;
	pgssEntry    *blacklist_entry = NULL;

	/*
	 * first we search the blacklist rules, if there exists one
	 * the query of user is prohibited according to the blacklist
	 * rule engine.
	 *
	 */
	if (pgfw_rule_engine == PGFW_ENGINE_BLACKLIST ||
		pgfw_rule_engine == PGFW_ENGINE_HYBRID)
	{
		blacklist_entry = lookup_blacklist(userid, queryid);
	}

	if (blacklist_entry == NULL) {
		/*
		 * then we search the whitelist rules, if there exists one
		 * the query of user is allowed according to the whitelist
		 * rule engine.
		 *
		 */
		if (pgfw_rule_engine == PGFW_ENGINE_WHITELIST ||
			pgfw_rule_engine == PGFW_ENGINE_HYBRID)
		{
			whitelist_entry = lookup_whitelist(userid, queryid);
		}
	}

	return __to_be_prohibited(whitelist_entry, blacklist_entry);
}

