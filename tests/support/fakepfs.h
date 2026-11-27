/*
 * A hard disk in memory behind the PFS interface (the scepfs* functions of
 * src/lib/scepfsd.h): the units that list hdd0:, mount partitions and read
 * their files run unchanged, without pfsd. Linking fakepfs.o replaces the
 * pfsd client, whose scepfs* names are weak aliases.
 */

#ifndef FAKEPFS_H
#define FAKEPFS_H

#include <string>
#include <map>

/* One partition of hdd0:. */
struct FakePart {
	string name;
	unsigned attr;           /* iox_stat_t attr: bit 0 = installed data partition */
	unsigned size;           /* iox_stat_t size, as pfsd reports it */
	unsigned char ctime[8];  /* sceMcStDateTime layout */
	string header;           /* the start of the raw partition: PS2ICON3D header and its files */
	map<string, string> files; /* path inside the partition ("res/info.sys") -> content */
	int failReads;           /* nonzero: every read of the partition fails with -EIO */
	string failFile;         /* a path of `files` whose reads fail with -EIO */
	int writable;            /* nonzero: its files can be written, created and truncated */
};

/* Empties the disk, the counters and the hooks. */
void fakeReset(void);

/* Appends a partition to the listing of hdd0: (in listing order). */
FakePart &fakeAddPartition(const char *name, unsigned size);
FakePart *fakeFind(const char *name);
void fakeRemove(const char *name);
void fakeSetCtime(FakePart &p, int year, int month, int day, int hour, int min, int sec);

/* A game partition as the Definitive Patch installs it: a PS2ICON3D header
   with SYSTEM.CNF (BOOT2 = cdrom0:\<bootFile>;1) and icon.sys, and
   res/info.sys with the title, the title id and content_type. */
FakePart &fakeAddGame(const char *name, const char *title, const char *bootFile, int contentType);
string fakeHeader(const string &systemCnf, const string &iconSys);
string fakeInfoSys(const char *title, const char *titleId, int contentType);

/* Accesses (mounts, opens and stats) of partition `name`, or of all of them
   with 0, since the last fakeResetCounts. */
int fakeAccesses(const char *name);
void fakeResetCounts(void);

/* Files and listings open now. */
int fakeOpenFds(void);

/* Calls of the PFS interface the fake does not implement, and writes outside
   a writable partition. */
extern int fakeUnexpected;
/* Nonzero: listing hdd0: fails. */
extern int fakeListFails;
/* Called on every access to a partition, with its name. */
extern void (*fakeOnAccess)(const char *name);

#endif
