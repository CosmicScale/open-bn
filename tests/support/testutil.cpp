/*
 * Helpers of the tests (testutil.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "app/bnapp.h"
#include "../../ref/sqlite-2.7.2/gen/sqlite.h"
#include "testutil.h"

void startTests(void)
{
	struct rlimit lim;

	setvbuf(stdout, 0, _IOLBF, 0);
	lim.rlim_cur = lim.rlim_max = TEST_MEMORY_KB * 1024L;
	if (setrlimit(RLIMIT_AS, &lim) != 0)
		printf("warning: cannot limit the memory of the program\n");
	lim.rlim_cur = lim.rlim_max = 0; /* no core file on /bntmp */
	setrlimit(RLIMIT_CORE, &lim);
	alarm(TEST_SECONDS);
}

/* The state of the import once it ends, -1 if it is still running at `end`. */
static int waitImport(long end)
{
	int s;

	while (nowMs() < end) {
		s = bnGameImportStat();
		if (s == 0 || s == 4)
			return s;
		usleep(5000);
	}
	return -1;
}

int runImport(void)
{
	int s;

	if (bnGameImportStart() != 0)
		return -1;
	s = waitImport(nowMs() + 60000);
	if (s != -1)
		return s;
	/* The next test would run with this import still writing: stop it, or
	   stop the program. */
	bnGameImportCancel();
	if (waitImport(nowMs() + 10000) == -1) {
		printf("the import does not stop: giving up\n");
		_exit(3);
	}
	return -1;
}

static int collectRow(void *arg, int n, char **v, char **)
{
	string *out = (string *)arg;
	int i;

	for (i = 0; i < n; i++) {
		if (i != 0)
			*out += "|";
		*out += v[i] != 0 ? v[i] : "NULL";
	}
	*out += "\n";
	return 0;
}

string dbRows(const char *sql)
{
	string out;
	char *err = 0;
	sqlite *db = sqlite_open(bnGameDbPath, 0666, &err);

	if (db == 0)
		return string("error: open: ") + (err != 0 ? err : "?");
	if (sqlite_exec(db, sql, collectRow, &out, &err) != SQLITE_OK)
		out = string("error: ") + (err != 0 ? err : "?");
	sqlite_close(db);
	return out;
}

int dbInt(const char *sql)
{
	string s = dbRows(sql);

	if (s.length() == 0 || s.substr(0, 6) == "error:")
		return -1;
	return atoi(s.c_str());
}

int dbExec(const char *sql)
{
	return dbRows(sql).substr(0, 6) != "error:";
}

int dbInsertGame(const char *uri, const char *title, int contentAttr)
{
	char *sql = sqlite_mprintf(
	    "insert into sce_game values(0, 0, 1024, '%q', 'SLPM-00000', '0', '', '', '', '', '', '20260101120000', "
	    "4096, 1, '%q', '%q/res', 0, 0, 1, 600, 0, 0, '', 1, '0', '0', 'J', 0, 255, 0, %d)",
	    title, uri, uri, contentAttr);
	int ok = dbExec(sql);

	sqlite_freemem(sql);
	return ok;
}

string readFile(const char *path)
{
	string out;
	char buf[8192];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == 0)
		return out;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return out;
}

int copyFile(const char *from, const char *to)
{
	string data = readFile(from);
	FILE *f;
	int ok;

	if (data.length() == 0)
		return 0;
	f = fopen(to, "wb");
	if (f == 0)
		return 0;
	ok = fwrite(data.data(), 1, data.length(), f) == data.length();
	return fclose(f) == 0 && ok;
}

void removeDb(const char *path)
{
	string journal = string(path) + "-journal";

	unlink(path);
	unlink(journal.c_str());
}

long nowMs(void)
{
	struct timeval tv;

	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}
