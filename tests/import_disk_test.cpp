/*
 * The Game Collection import on the console's own disk, through pfsd. It
 * reads the disk as bn's own import does, and writes to copies of game.db in
 * the working directory: the console's game.db is only copied.
 *
 * bn may be running, idle: pfsd serves several clients, each with its own
 * mount points. But bn keeps the partitions of the last covers it showed
 * mounted, and pfsd refuses (-EBUSY) to open a partition raw while another
 * client has it mounted, so this import cannot read their SYSTEM.CNF (bn's
 * own import unmounts them first). Those partitions are found first and left
 * out of the comparisons.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "app/bnapp.h"
#include "fs/bbmount.h"
#include "lib/scepfsd.h"
#include "../ref/sqlite-2.7.2/gen/sqlite.h"
#include "support/testutil.h"

#define GAME_DB "/opt1/database/sqlite/game.db"
#define INCREMENTAL "import_disk_inc.db"
#define FULL "import_disk_full.db"

static string s_busy; /* "'<name>', ..." of the partitions another client has mounted */
static int s_busyCount;
static string s_fullGames;
static string s_fullScan;

/* Every column but the rowid, which depends on the history of the table. */
static string gamesQuery(void)
{
	return "select type, key_flag, inst_size, title, title_id, title_sub_id, release_date, developer_id, "
	       "publisher_id, note, content_web, install_date, table_version, version, uri, resource_path, "
	       "image_topviewflag, image_type, image_count, image_viewsec, copyright_viewflag, copyright_imgcount, "
	       "genre, parental_lock, effective_date, expire_date, area, violence_flag, content_type, content_subtype, "
	       "content_attr from sce_game where content_attr = 0 and substr(uri, 6, 64) not in ("
	    + s_busy + ") order by uri";
}

static string scanQuery(void)
{
	return "select part, ctime, size, state, uri from sce_game_scan where part not in (" + s_busy
	    + ") order by part";
}

/* The application partitions of hdd0: (those bn lists, but for the XC ones
   it adds to their game) that cannot be opened raw because another client has
   them mounted. */
static void findBusyPartitions(void)
{
	iox_dirent_t d;
	char raw[300];
	char *q;
	int dir = scepfsDopen("hdd0:");
	int fd;

	s_busy = "''";
	s_busyCount = 0;
	TEST_ASSERT_TRUE_MESSAGE(dir >= 0, "cannot list hdd0:");
	while (scepfsDread(dir, &d) > 0) {
		if (d.name[0] == '_' || (d.stat.attr & 1) || (d.name[0] >= 'A' && d.name[0] <= 'Z' && d.name[1] == 'C'))
			continue;
		sprintf(raw, "hdd0:%s", d.name);
		fd = scepfsOpen(raw, 1, 0);
		if (fd >= 0) {
			scepfsClose(fd);
			continue;
		}
		if (fd == -16) {
			q = sqlite_mprintf(", '%q'", d.name);
			s_busy += q;
			sqlite_freemem(q);
			s_busyCount++;
			printf("mounted by another client, left out: %s\n", d.name);
		}
	}
	scepfsDclose(dir);
}

static long timedImport(const char *db)
{
	long t = nowMs();

	bnGameDbPath = db;
	TEST_ASSERT_EQUAL_INT(0, runImport());
	return nowMs() - t;
}

void setUp(void)
{}

void tearDown(void)
{}

/* An import into an empty database reads every game of the disk. */
static void test_full_import(void)
{
	char msg[96];
	long ms;

	findBusyPartitions();
	removeDb(FULL);
	ms = timedImport(FULL);
	s_fullGames = dbRows(gamesQuery().c_str());
	s_fullScan = dbRows(scanQuery().c_str());
	sprintf(msg, "%d games in %ld ms", dbInt("select count(*) from sce_game"), ms);
	TEST_MESSAGE(msg);
	TEST_ASSERT_TRUE_MESSAGE(dbInt("select count(*) from sce_game") > 0, "no game on the disk");
	TEST_ASSERT_EQUAL_STRING_MESSAGE("",
	    dbRows(("select part from sce_game_scan where state = 2 and part not in (" + s_busy + ")").c_str()).c_str(),
	    "partitions whose reads failed");
}

/* The import of the console's game.db, which holds what bn saw so far, ends
   with the same games as the full import. */
static void test_incremental_import_matches_full(void)
{
	char msg[64];
	long ms;

	removeDb(INCREMENTAL);
	TEST_ASSERT_TRUE_MESSAGE(copyFile(GAME_DB, INCREMENTAL), "cannot copy " GAME_DB);
	ms = timedImport(INCREMENTAL);
	sprintf(msg, "import of game.db in %ld ms", ms);
	TEST_MESSAGE(msg);
	TEST_ASSERT_EQUAL_STRING(s_fullGames.c_str(), dbRows(gamesQuery().c_str()).c_str());
	TEST_ASSERT_EQUAL_STRING(s_fullScan.c_str(), dbRows(scanQuery().c_str()).c_str());
}

/* Once up to date, an import writes nothing. A partition the full import
   could not read is read again each time, so this needs none. */
static void test_second_import_writes_nothing(void)
{
	string before = readFile(FULL);
	char msg[64];
	long ms;

	if (s_busyCount != 0)
		TEST_IGNORE_MESSAGE("partitions mounted by another client: they are read again each time");
	ms = timedImport(FULL);
	sprintf(msg, "import with nothing changed in %ld ms", ms);
	TEST_MESSAGE(msg);
	TEST_ASSERT_TRUE_MESSAGE(before == readFile(FULL), "the database changed");
}

/* crt1 calls main, which is bn's: the link wraps it (ld --wrap main). */
extern "C" int __wrap_main(int, char **)
{
	int r;

	startTests();
	bnGameDbPath = FULL; /* never the console's game.db */
	if (scepfsInit() != 0) {
		printf("cannot connect to pfsd\n");
		return 2;
	}
	UNITY_BEGIN();
	RUN_TEST(test_full_import);
	RUN_TEST(test_incremental_import_matches_full);
	RUN_TEST(test_second_import_writes_nothing);
	r = UNITY_END();
	BBpfsAllUnMounter(); /* the partitions the imports left mounted */
	scepfsExit();
	if (r == 0) {
		removeDb(FULL);
		removeDb(INCREMENTAL);
	}
	return r;
}
