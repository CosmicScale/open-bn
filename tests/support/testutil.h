/*
 * Helpers of the tests: running the game import to the end, and reading the
 * database it writes.
 */

#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <string>

/* A test program shares the console with bn, whose kernel has no
   out-of-memory killer: a program that takes all the memory freezes the
   console. So it is killed (SIGALRM) after TEST_SECONDS, and an allocation
   past TEST_MEMORY_KB of address space fails. The programs peak at about
   16.5 MB, most of it the libraries and the thread stacks. */
#define TEST_SECONDS 300
#define TEST_MEMORY_KB (24 * 1024)

/* Line-buffered stdout and the limits above: the first call of a test
   program. */
void startTests(void);

/* Runs bnGameImportStart and waits for the scan to end; returns the final
   bnGameImportStat (0 done, 4 stopped), -1 if it is still running after a
   minute (it is cancelled; the program exits if it does not stop). */
int runImport(void);

/* The rows of `sql` on bnGameDbPath, one per line, columns separated by '|'
   (NULL for a null value); "error: ..." if the statement fails. */
string dbRows(const char *sql);
/* The single integer `sql` returns, -1 on error. */
int dbInt(const char *sql);
/* Runs `sql`; returns 1 on success. */
int dbExec(const char *sql);
/* Inserts a sce_game row as an installer page does. */
int dbInsertGame(const char *uri, const char *title, int contentAttr);

/* The bytes of a file, empty if it cannot be read. */
string readFile(const char *path);
/* Copies a file; returns 1 on success. */
int copyFile(const char *from, const char *to);
/* Removes the database and its journal. */
void removeDb(const char *path);
/* Milliseconds since an arbitrary start. */
long nowMs(void);

#endif
