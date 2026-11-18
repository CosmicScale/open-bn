/*
 * The Game Collection import (bnGameImportStart, src/app/bnapp.cpp) on a disk
 * in memory (support/fakepfs.h): what it writes to sce_game and
 * sce_game_scan, and what it reads again.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "app/bnapp.h"
#include "fs/bbmount.h"
#include "support/fakepfs.h"
#include "support/testutil.h"

#define DB "import_test.db"

#define PES "PP.SLES-50462.01.PRO_EVOLUTION_S"
#define GTA "PP.SLUS-20946.01.GTA_SAN_ANDREAS"
#define JAK "PP.SCES-50760.01.JAK_II"

#define GAMES "select title, title_id, uri, content_type from sce_game order by uri"
#define SCAN "select part, state from sce_game_scan order by part"

static void addPes(void)
{
	fakeAddGame(PES, "Pro Evolution Soccer", "SLES_504.62", 255);
}

static void addGta(void)
{
	fakeAddGame(GTA, "Grand Theft Auto: San Andreas", "SLUS_209.46", 255);
}

static void addJak(void)
{
	fakeAddGame(JAK, "Jak II", "SCES_507.60", 255);
}

/* Starts the disk over; the mounts bn keeps are released first. */
static void resetDisk(void)
{
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, BBpfsAllUnMounter(), "a mount is still in use");
	fakeReset();
	fakeAddPartition("__mbr", 0x40000);
	fakeAddPartition("__common", 0x100000);
}

static void importOk(void)
{
	TEST_ASSERT_EQUAL_INT(0, runImport());
}

void setUp(void)
{
	removeDb(DB);
	resetDisk();
}

void tearDown(void)
{
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, fakeUnexpected, "unexpected PFS call");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, fakeOpenFds(), "a file is still open");
	resetDisk();
}

/* Every game gets a row with the values of its info.sys, and a scan row that
   points to it. */
static void test_first_import_adds_every_game(void)
{
	addPes();
	addGta();
	addJak();
	importOk();
	TEST_ASSERT_EQUAL_STRING("Jak II|SCES-50760|pfs:/" JAK "|255\n"
	                         "Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n"
	                         "Grand Theft Auto: San Andreas|SLUS-20946|pfs:/" GTA "|255\n",
	    dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING(JAK "|0\n" PES "|0\n" GTA "|0\n", dbRows(SCAN).c_str());
	TEST_ASSERT_EQUAL_INT(3,
	    dbInt("select count(*) from sce_game_scan s, sce_game g where s.game_oid = g.oid and s.uri = g.uri"));
}

/* Without info.sys the title comes from icon.sys and the rest are defaults. */
static void test_game_without_info_sys_uses_icon_sys(void)
{
	fakeAddGame(PES, "Pro Evolution Soccer", "SLES_504.62", 255).files.clear();
	importOk();
	TEST_ASSERT_EQUAL_STRING("0|Pro Evolution Soccer |SLES-50462|pfs:/" PES "|255\n",
	    dbRows("select type, title, title_id, uri, content_type from sce_game").c_str());
}

/* Without a PS2ICON3D header the title id comes from the partition name. */
static void test_title_id_from_partition_name(void)
{
	fakeAddPartition("PP.SLPS-25001.01.NO_HEADER", 0x100000);
	importOk();
	TEST_ASSERT_EQUAL_STRING("-1||SLPS-25001\n", dbRows("select type, title, title_id from sce_game").c_str());
	TEST_ASSERT_EQUAL_STRING("PP.SLPS-25001.01.NO_HEADER|0\n", dbRows(SCAN).c_str());
}

/* A second import with nothing changed reads no game and writes nothing. */
static void test_unchanged_disk_is_not_read_again(void)
{
	string before;

	addPes();
	addGta();
	importOk();
	before = readFile(DB);
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(0));
	TEST_ASSERT_TRUE_MESSAGE(before == readFile(DB), "game.db changed");
}

static void test_new_game_is_the_only_one_read(void)
{
	addPes();
	addGta();
	importOk();
	addJak();
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(PES));
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(GTA));
	TEST_ASSERT_TRUE(fakeAccesses(JAK) > 0);
	TEST_ASSERT_EQUAL_INT(3, dbInt("select count(*) from sce_game"));
}

static void test_removed_game_is_deleted(void)
{
	addPes();
	addGta();
	addJak();
	importOk();
	fakeRemove(GTA);
	importOk();
	TEST_ASSERT_EQUAL_STRING("Jak II|SCES-50760|pfs:/" JAK "|255\n"
	                         "Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n",
	    dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING(JAK "|0\n" PES "|0\n", dbRows(SCAN).c_str());
}

/* A partition created again (new time) or resized is read again and its row
   replaced. */
static void test_changed_partition_is_read_again(void)
{
	addPes();
	addGta();
	addJak();
	importOk();
	fakeSetCtime(*fakeFind(PES), 2026, 11, 18, 10, 30, 0);
	fakeFind(PES)->files["res/info.sys"] = fakeInfoSys("Pro Evolution Soccer 2", "SLES-50462", 255);
	fakeFind(GTA)->size += 0x1000;
	fakeFind(GTA)->files["res/info.sys"] = fakeInfoSys("GTA San Andreas", "SLUS-20946", 255);
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(JAK));
	TEST_ASSERT_EQUAL_STRING("Jak II|SCES-50760|pfs:/" JAK "|255\n"
	                         "Pro Evolution Soccer 2|SLES-50462|pfs:/" PES "|255\n"
	                         "GTA San Andreas|SLUS-20946|pfs:/" GTA "|255\n",
	    dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING("20261118103000",
	    dbRows("select install_date from sce_game where title_id = 'SLES-50462'").substr(0, 14).c_str());
}

/* A partition whose info.sys has another content_type gets no row, and is
   not read again while it does not change. */
static void test_other_content_type_is_skipped_once(void)
{
	addPes();
	fakeAddGame("PP.APP-00001.01.SOME_APP", "Some App", "SLPM_000.01", 1);
	importOk();
	TEST_ASSERT_EQUAL_STRING("Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n", dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING("PP.APP-00001.01.SOME_APP|1\n" PES "|0\n", dbRows(SCAN).c_str());
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(0));
}

/* A game whose reads fail keeps a row (it is on the disk) and is read again
   at the next import. */
static void test_read_error_is_retried(void)
{
	addPes();
	addGta();
	fakeFind(PES)->failReads = 1;
	importOk();
	TEST_ASSERT_EQUAL_STRING(PES "|2\n" GTA "|0\n", dbRows(SCAN).c_str());
	TEST_ASSERT_EQUAL_INT(1, dbInt("select count(*) from sce_game where uri = 'pfs:/" PES "'"));
	fakeFind(PES)->failReads = 0;
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(GTA));
	TEST_ASSERT_TRUE(fakeAccesses(PES) > 0);
	TEST_ASSERT_EQUAL_STRING("Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n"
	                         "Grand Theft Auto: San Andreas|SLUS-20946|pfs:/" GTA "|255\n",
	    dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING(PES "|0\n" GTA "|0\n", dbRows(SCAN).c_str());
}

/* Only info.sys fails: the game is read again too, not kept with the title
   of icon.sys. */
static void test_info_sys_read_error_is_retried(void)
{
	addPes();
	fakeFind(PES)->failFile = "res/info.sys";
	importOk();
	TEST_ASSERT_EQUAL_STRING(PES "|2\n", dbRows(SCAN).c_str());
	fakeFind(PES)->failFile = "";
	fakeResetCounts();
	importOk();
	TEST_ASSERT_TRUE(fakeAccesses(PES) > 0);
	TEST_ASSERT_EQUAL_STRING("Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n", dbRows(GAMES).c_str());
	TEST_ASSERT_EQUAL_STRING(PES "|0\n", dbRows(SCAN).c_str());
}

static const char *s_cancelAt;

static void cancelOnAccess(const char *name)
{
	if (s_cancelAt != 0 && strcmp(name, s_cancelAt) == 0) {
		s_cancelAt = 0;
		bnGameImportCancel();
	}
}

/* Leaving the page stops the import: what was read is written, the row an
   installer page inserted for a game not read yet stays, the next import
   reads only the rest. */
static void test_cancel_keeps_what_was_read(void)
{
	importOk();
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/" JAK, "Installed by the page", 0));
	addPes();
	addGta();
	addJak();
	fakeAddGame("PP.SLES-52054.01.BURNOUT_3", "Burnout 3", "SLES_520.54", 255);
	s_cancelAt = GTA;
	fakeOnAccess = cancelOnAccess;
	TEST_ASSERT_EQUAL_INT(4, runImport());
	TEST_ASSERT_EQUAL_STRING(PES "|0\n" GTA "|0\n", dbRows(SCAN).c_str());
	TEST_ASSERT_EQUAL_STRING("Installed by the page\n",
	    dbRows("select title from sce_game where uri = 'pfs:/" JAK "'").c_str());
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(PES));
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(GTA));
	TEST_ASSERT_EQUAL_INT(4, dbInt("select count(*) from sce_game"));
}

/* With more than 1024 entries the listing is cut: a game past the cut is not
   deleted. */
static void test_cut_listing_deletes_nothing(void)
{
	char name[32];
	int i;

	addPes();
	addGta();
	importOk();
	resetDisk();
	addPes();
	for (i = 0; i < 1024; i++) {
		sprintf(name, "__fill%04d", i);
		fakeAddPartition(name, 0x1000);
	}
	addGta();
	importOk();
	TEST_ASSERT_EQUAL_INT(2, dbInt("select count(*) from sce_game"));
	TEST_ASSERT_EQUAL_STRING(PES "|0\n" GTA "|0\n", dbRows(SCAN).c_str());
}

/* When hdd0: cannot be listed game.db is left as it is. */
static void test_listing_error_leaves_db_alone(void)
{
	string before;

	addPes();
	importOk();
	before = readFile(DB);
	fakeListFails = 1;
	importOk();
	TEST_ASSERT_TRUE_MESSAGE(before == readFile(DB), "game.db changed");
}

/* An installer page inserts a row for what it installs: the import replaces it
   with its own, and deletes the row of a game no longer on the disk. */
static void test_installer_rows_are_replaced_or_deleted(void)
{
	importOk();
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/" PES, "Installed by the page", 0));
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/PP.SLES-00000.01.GONE", "Gone", 0));
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/PP.OTHER", "Not a game row", 1));
	addPes();
	importOk();
	TEST_ASSERT_EQUAL_STRING("Pro Evolution Soccer|0\n"
	                         "Not a game row|1\n",
	    dbRows("select title, content_attr from sce_game order by content_attr").c_str());
}

/* Another page deletes a game row and SQLite gives its rowid to a new row:
   the import reads the game again and leaves the new row alone. */
static void test_reused_rowid_is_not_taken_for_the_game(void)
{
	int oid;

	addPes();
	addGta();
	importOk();
	oid = dbInt("select oid from sce_game where uri = 'pfs:/" GTA "'");
	TEST_ASSERT_TRUE(oid > 0);
	TEST_ASSERT_TRUE(dbExec("delete from sce_game where uri = 'pfs:/" GTA "'"));
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/PP.OTHER", "Not a game row", 1));
	TEST_ASSERT_EQUAL_INT_MESSAGE(oid, dbInt("select oid from sce_game where uri = 'pfs:/PP.OTHER'"),
	    "SQLite did not reuse the rowid");
	fakeResetCounts();
	importOk();
	TEST_ASSERT_EQUAL_INT(0, fakeAccesses(PES));
	TEST_ASSERT_TRUE(fakeAccesses(GTA) > 0);
	TEST_ASSERT_EQUAL_STRING("Pro Evolution Soccer|0\n"
	                         "Grand Theft Auto: San Andreas|0\n"
	                         "Not a game row|1\n",
	    dbRows("select title, content_attr from sce_game order by content_attr, title desc").c_str());
}

/* The same, when the new row is a game row an installer page inserted: the
   import does not take it for the deleted game's. */
static void test_reused_rowid_of_a_game_row(void)
{
	int oid;

	addPes();
	addGta();
	importOk();
	oid = dbInt("select oid from sce_game where uri = 'pfs:/" GTA "'");
	TEST_ASSERT_TRUE(oid > 0);
	TEST_ASSERT_TRUE(dbExec("delete from sce_game where uri = 'pfs:/" GTA "'"));
	TEST_ASSERT_TRUE(dbInsertGame("pfs:/" JAK, "Installed by the page", 0));
	TEST_ASSERT_EQUAL_INT_MESSAGE(oid, dbInt("select oid from sce_game where uri = 'pfs:/" JAK "'"),
	    "SQLite did not reuse the rowid");
	addJak();
	importOk();
	TEST_ASSERT_EQUAL_STRING("Jak II|SCES-50760|pfs:/" JAK "|255\n"
	                         "Pro Evolution Soccer|SLES-50462|pfs:/" PES "|255\n"
	                         "Grand Theft Auto: San Andreas|SLUS-20946|pfs:/" GTA "|255\n",
	    dbRows(GAMES).c_str());
}

/* Quotes and Japanese titles reach the row byte for byte. */
static void test_titles_are_stored_intact(void)
{
	const char *quoted = "Tom's \"Quoted\" Game; drop table sce_game";
	const char *japanese = "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xb4\xe3\x83\xb3\xe3\x82\xaf\xe3\x82\xa8\xe3\x82\xb9"
	                       "\xe3\x83\x88V \xe5\xa4\xa9\xe7\xa9\xba\xe3\x81\xae\xe8\x8a\xb1\xe5\xab\x81";
	string want;

	fakeAddGame(PES, quoted, "SLES_504.62", 255);
	fakeAddGame("PP.SLPM-65888.01.DQ5", japanese, "SLPM_658.88", 255);
	importOk();
	want = string(quoted) + "\n" + japanese + "\n";
	TEST_ASSERT_EQUAL_STRING(want.c_str(), dbRows("select title from sce_game order by title_id").c_str());
}

/* crt1 calls main, which is bn's: the link wraps it (ld --wrap main). */
extern "C" int __wrap_main(int, char **)
{
	int r;

	startTests();
	bnGameDbPath = DB;
	UNITY_BEGIN();
	RUN_TEST(test_first_import_adds_every_game);
	RUN_TEST(test_game_without_info_sys_uses_icon_sys);
	RUN_TEST(test_title_id_from_partition_name);
	RUN_TEST(test_unchanged_disk_is_not_read_again);
	RUN_TEST(test_new_game_is_the_only_one_read);
	RUN_TEST(test_removed_game_is_deleted);
	RUN_TEST(test_changed_partition_is_read_again);
	RUN_TEST(test_other_content_type_is_skipped_once);
	RUN_TEST(test_read_error_is_retried);
	RUN_TEST(test_info_sys_read_error_is_retried);
	RUN_TEST(test_cancel_keeps_what_was_read);
	RUN_TEST(test_cut_listing_deletes_nothing);
	RUN_TEST(test_listing_error_leaves_db_alone);
	RUN_TEST(test_installer_rows_are_replaced_or_deleted);
	RUN_TEST(test_reused_rowid_is_not_taken_for_the_game);
	RUN_TEST(test_reused_rowid_of_a_game_row);
	RUN_TEST(test_titles_are_stored_intact);
	r = UNITY_END();
	if (r == 0)
		removeDb(DB); /* kept after a failure: the last test's */
	return r;
}
