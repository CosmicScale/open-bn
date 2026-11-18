/*
 * HDD application management: deleting and uninstalling applications, and
 * the game import scan that keeps the sce_game table of game.db up to date
 * with the PFS partitions.
 */

#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list>
#include <iostream.h>
#include "lib/scepfsd.h"

#include "fs/bbfileaccess.h"
#include "fs/bbmount.h"
#include <unistd.h>
#include "lib/resuid.h"
#include "fs/bbfile.h"
#include <sys/stat.h>
#include "thread/commandinfo.h"
#include "thread/thposixthread.h"
#include "app/bnapp.h"

/* Name table (1024 entries): type 1 = 'P'/'C' entry, 2 = data. */
struct NameRec {
	/* 0x00 */ short type;
	/* 0x02 */ char name[34];
	/* 0x24 */ short id;
};

/* Application table (1024 entries). */
struct DirentDesc {
	/* 0x00 */ int tag;  /* always 0x8407 */
	/* 0x04 */ unsigned half;
	/* 0x08 */ int index;
	/* 0x0C */ iox_dirent_t *src;
};

struct InfoKey {
	int isString;        /* 1 = varchar column of sce_game */
	const char *name;
};

static int g_instCount;
static struct NameRec g_nameTab[1024];
static pthread_mutex_t g_appTabLock;  /* unused, but part of the layout */
static struct DirentDesc g_appSlots[1024];
/* Directory cache filled by scepfsDread; attr bit 0 = installed application. */
static iox_dirent_t g_direntBuf[1024];
static int g_direntCount;
static int g_appSlotCount;
static char g_infoBuf[0x10000];
static char g_valBuf[28][1024];
static char g_infoKey[1040];
static char g_infoVal[1040];
static int g_uidRc;
static uid_t g_ruid;
static uid_t g_euid;
static uid_t g_suid;
static pthread_t g_scanThread;
static char g_bootId[256];

static struct DirentDesc *g_appSlotBase = g_appSlots;
static int g_appSlotMax = 1024;
/* Nonzero stops the scan; set from another thread. */
static volatile int g_direntStop = 0;

/* The info.sys keys, in the order of the sce_game columns. */
static struct InfoKey g_infoKeys[28] = {
	{ 1, "title" },
	{ 1, "title_id" },
	{ 1, "title_sub_id" },
	{ 1, "release_date" },
	{ 1, "developer_id" },
	{ 1, "publisher_id" },
	{ 1, "note" },
	{ 1, "content_web" },
	{ 1, "install_date" },
	{ 0, "table_version" },
	{ 0, "version" },
	{ 1, "url" },
	{ 1, "resource_path" },
	{ 0, "image_topviewflag" },
	{ 0, "image_type" },
	{ 0, "image_count" },
	{ 0, "image_viewsec" },
	{ 0, "copyright_viewflag" },
	{ 0, "copyright_imgcount" },
	{ 1, "genre" },
	{ 0, "parental_lock" },
	{ 0, "effective_date" },
	{ 0, "expire_date" },
	{ 1, "area" },
	{ 0, "violence_flag" },
	{ 0, "content_type" },
	{ 0, "content_subtype" },
	{ 0, "content_attr" },
};

/* Scan state: 2 = reading the directory, 3 = processing, 4 = error,
 * 0 = done. */
static int g_direntState = 0;

/* Debug log, compiled out. */
#define BN_DPRINTF(x)

/* Raises the process to root, saving the three uids; the result is kept in
 * a global. */
static int becomeRoot(void)
{
	g_uidRc = getresuid(&g_ruid, &g_euid, &g_suid);
	if (g_uidRc == 0)
		g_uidRc = setresuid(0, 0, 0);
	return g_uidRc;
}

/* Restores the three uids saved by becomeRoot. */
static int restoreUid(void)
{
	g_uidRc = setresuid(g_ruid, g_euid, g_suid);
	return g_uidRc;
}

/* Deletes the application partition `path` ("pfsN:/<name>"): disables the
   automatic mounter, unmounts it (retrying up to 1000 times) and removes
   "hdd0:<name>". */
int bnDeleteApplication(char *path)
{
	static char g_appPathBuf[256];
	becomeRoot();
	string s(path);
	s.erase(0, 4);
	unsigned int n = s.find_first_not_of('/', 0);
	if (n != 0 && n != (unsigned int)-1)
		s.erase(0, n);
	BBpfsDisableMounter(s);
	int i = 0;
	int r;
	do {
		r = BBpfsUnMount(s);
		if (r == 0 || r == 2)
			break;
		i++;
	} while (i < 1000);
	if (r != 0 && r != 2) {
		restoreUid();
		return -1;
	}
	sprintf(g_appPathBuf, "hdd0:%s", path + 5);
	int ret = scepfsRemove(g_appPathBuf);
	BBpfsClearDisableMounter();
	restoreUid();
	return ret;
}

/* Uninstalls the application `path`: unmounts and removes, as
   bnDeleteApplication does, every partition (type 1 name entry) that carries
   its application id. */
int bnUninstallApplication(char *path)
{
	static char g_appPathBuf2[64];
	int ret = 0;
	int idx = -1;
	int i;

	becomeRoot();
	string s(path);
	s.erase(0, 4);
	unsigned int n = s.find_first_not_of('/', 0);
	if (n != 0 && n != (unsigned int)-1)
		s.erase(0, n);

	for (i = 0; i < g_appSlotCount; i++) {
		if (strcmp(s.c_str(), g_appSlots[i].src->name) == 0) {
			idx = g_appSlots[i].index;
			break;
		}
	}
	if (idx == -1) {
		restoreUid();
		return -1;
	}
	for (i = 0; i < g_instCount; i++) {
		if (idx == g_nameTab[i].id && g_nameTab[i].type == 1) {
			int r;
			int j;

			s = g_nameTab[i].name;
			BBpfsDisableMounter(s);
			j = 0;
			do {
				r = BBpfsUnMount(s);
				if (r == 0 || r == 2)
					break;
				j++;
			} while (j < 1000);
			if (r != 0 && r != 2) {
				ret = -2;
				break;
			}
			sprintf(g_appPathBuf2, "hdd0:%s", s.c_str());
			ret = scepfsRemove(g_appPathBuf2);
			BBpfsClearDisableMounter();
			if (ret < 0)
				break;
		}
	}
	restoreUid();
	return ret;
}

/* Reads SYSTEM.CNF from the "PS2ICON3D" header of partition `path` and
 * stores the game id in g_bootId ("BOOT2 = cdrom0:\SLPS_123.45;1" ->
 * "SLPS-12345"). Returns 0 when the id was found, 1 if BOOT2 does not point to
 * cdrom0:, negative on error. */
static int readBootId(const char *path)
{
	string s(path);
	string dev;
	s.erase(0, 4);
	unsigned int n = s.find_first_not_of('/', 0);
	if (n != 0 && n != (unsigned int)-1)
		s.erase(0, n);
	dev = "hdd0:";
	dev += s;
	BBpfsDisableMounter(s);
	int i = 0;
	int r;
	do {
		r = BBpfsUnMount(s);
		if (r == 0 || r == 2)
			break;
		i++;
	} while (i < 1000);
	if (r != 0 && r != 2) {
		BBpfsClearDisableMounter();
		return -1;
	}

	int fd = scepfsOpen(dev.c_str(), 1);
	if (fd < 0) {
		BBpfsClearDisableMounter();
		return -2;
	}
	PS2Icon3DHeader hdr;
	if (scepfsRead(fd, &hdr, 512) < 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -3;
	}
	if (strncmp(hdr.magic, "PS2ICON3D", 9) != 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -4;
	}
	if (hdr.systemCnf.offset == 0 || hdr.systemCnf.size == 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -5;
	}
	int size = (hdr.systemCnf.size + 511) / 512 * 512;
	scepfsLseek(fd, hdr.systemCnf.offset, 0);
	int got = scepfsRead(fd, g_infoBuf, size);
	scepfsClose(fd);
	BBpfsClearDisableMounter();
	if (got < 0)
		return -7;

	char *cur = g_infoBuf;
	memset(g_bootId, 0, 256);
	char *line;
	int len;
	int k;
	while ((line = strsep(&cur, "\n")) != 0 && strlen(line) != 0) {
		if (strncasecmp(line, "BOOT2", 5) == 0) {
			char *p = strstr(line, "=");
			if (p != 0) {
				p++;
				len = strlen(p);
				for (k = 0; k < len; k++, p++)
					if (*p != ' ' && *p != '\t')
						break;
				strcpy(g_bootId, p);
			}
		}
	}
	for (k = strlen(g_bootId) - 1; k >= 0; k--) {
		if (g_bootId[k] == '\r' || g_bootId[k] == '\n')
			g_bootId[k] = 0;
		else
			break;
	}
	if (strstr(g_bootId, "cdrom0:") != 0) {
		strcpy(g_infoBuf, g_bootId + strlen(g_bootId) - 13);
		memset(g_bootId, 0, 256);
		g_bootId[0] = g_infoBuf[0];
		g_bootId[1] = g_infoBuf[1];
		g_bootId[2] = g_infoBuf[2];
		g_bootId[3] = g_infoBuf[3];
		g_bootId[4] = '-';
		g_bootId[5] = g_infoBuf[5];
		g_bootId[6] = g_infoBuf[6];
		g_bootId[7] = g_infoBuf[7];
		g_bootId[8] = g_infoBuf[9];
		g_bootId[9] = g_infoBuf[10];
	} else
		return 1;
	return 0;
}

/* Reads the "PS2X" icon.sys file from the "PS2ICON3D" header of partition
 * `path` and stores "title0 title1" in the `title` row of g_valBuf. Returns -8
 * if the magic is missing. */
static int readTitle(const char *path)
{
	static char g_title0Buf[72];
	static char g_title1Buf[72];
	string s(path);
	string dev;
	s.erase(0, 4);
	unsigned int n = s.find_first_not_of('/', 0);
	if (n != 0 && n != (unsigned int)-1)
		s.erase(0, n);
	dev = "hdd0:";
	dev += s;
	BBpfsDisableMounter(s);
	int i = 0;
	int r;
	do {
		r = BBpfsUnMount(s);
		if (r == 0 || r == 2)
			break;
		i++;
	} while (i < 1000);
	if (r != 0 && r != 2) {
		BBpfsClearDisableMounter();
		return -1;
	}

	int fd = scepfsOpen(dev.c_str(), 1);
	if (fd < 0) {
		BBpfsClearDisableMounter();
		return -2;
	}
	PS2Icon3DHeader hdr;
	if (scepfsRead(fd, &hdr, 512) < 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -3;
	}
	if (strncmp(hdr.magic, "PS2ICON3D", 9) != 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -4;
	}
	if (hdr.iconSys.offset == 0 || hdr.iconSys.size == 0) {
		scepfsClose(fd);
		BBpfsClearDisableMounter();
		return -5;
	}
	int size = (hdr.iconSys.size + 511) / 512 * 512;
	scepfsLseek(fd, hdr.iconSys.offset, 0);
	int got = scepfsRead(fd, g_infoBuf, size);
	scepfsClose(fd);
	BBpfsClearDisableMounter();
	if (got < 0)
		return -7;

	if (g_infoBuf[0] != 'P' || g_infoBuf[1] != 'S' || g_infoBuf[2] != '2' || g_infoBuf[3] != 'X')
		return -8;

	char *cur = g_infoBuf;
	memset(g_title0Buf, 0, 72);
	memset(g_title1Buf, 0, 72);
	char *line;
	int len;
	int k;
	while ((line = strsep(&cur, "\n")) != 0 && strlen(line) != 0) {
		if (strncasecmp(line, "title0", 6) == 0) {
			char *p = strstr(line, "=");
			if (p != 0) {
				p++;
				len = strlen(p);
				for (k = 0; k < len; k++, p++)
					if (*p != ' ' && *p != '\t')
						break;
				strcpy(g_title0Buf, p);
			}
		} else if (strncasecmp(line, "title1", 6) == 0) {
			char *p = strstr(line, "=");
			if (p != 0) {
				p++;
				len = strlen(p);
				for (k = 0; k < len; k++, p++)
					if (*p != ' ' && *p != '\t')
						break;
				strcpy(g_title1Buf, p);
			}
		}
	}
	for (k = strlen(g_title0Buf) - 1; k >= 0; k--) {
		if (g_title0Buf[k] == '\r' || g_title0Buf[k] == '\n')
			g_title0Buf[k] = 0;
		else
			break;
	}
	for (k = strlen(g_title1Buf) - 1; k >= 0; k--) {
		if (g_title1Buf[k] == '\r' || g_title1Buf[k] == '\n')
			g_title1Buf[k] = 0;
		else
			break;
	}
	sprintf(g_valBuf[0], "%s %s", g_title0Buf, g_title1Buf);
	return 0;
}

/* Parses one "key=value" line of /res/info.sys into g_infoKey and g_infoVal
 * (leading blanks dropped, cut at CR/LF). Returns the index of the key in
 * g_infoKeys, or -1 if unknown, empty, too long or without '='. */
static int infoParseLine(const char *line)
{
	int i;
	const char *p;

	if (strlen(line) == 0 || strlen(line) >= 1024)
		return -1;
	memset(g_infoKey, 0, 1040);
	memset(g_infoVal, 0, 1040);
	for (i = 0; i < (int)strlen(line); i++) {
		if (line[i] != '=')
			g_infoKey[i] = line[i];
		else {
			g_infoKey[i] = 0;
			break;
		}
	}
	if (line[i] != '=')
		return -1;
	p = &line[i + 1];
	for (i = 0; i < (int)strlen(p); i++)
		if (p[i] != ' ' && p[i] != '\t')
			break;
	p += i;
	if (strlen(p) != 0) {
		for (i = 0; i < (int)strlen(p); i++) {
			if (p[i] == '\r' || p[i] == '\n') {
				g_infoVal[i] = 0;
				break;
			}
			g_infoVal[i] = p[i];
		}
	}
	for (i = 0; i < (int)strlen(g_infoKey); i++)
		if (g_infoKey[i] == ' ')
			g_infoKey[i] = 0;
	for (i = 0; i < 28; i++)
		if (strcasecmp(g_infoKeys[i].name, g_infoKey) == 0)
			return i;
	return -1;
}

#include "fs/bburl.h"

/* Reads "<dir>/res/info.sys" and stores every known value in the row of its
 * key in g_valBuf.
 * Returns 1 if the content_type read (key 25, 255 when missing) differs from
 * `type`, 0 if it matches, -1 if the file is missing or empty, -2 if it could
 * not be read (the partition cannot be mounted, an I/O error). */
static int readInfoSys(const char *dir, int type)
{
	struct stat st;
	BBUrl url;
	string path;
	int fd;
	int size;
	int ctype;
	int r;
	char *cur;
	char *line;

	path = dir;
	path += "/res/info.sys";
	url.setUrlString(path);
	path = url.getPathString();
	/* -2 is ENOENT from pfsd: no res/ or no info.sys; anything else is an error */
	r = BBlstatCPP(path.c_str(), &st);
	if (r == -2)
		return -1;
	if (r < 0)
		return -2;
	if (st.st_size <= 0)
		return -1;
	fd = BBopenCPP(path.c_str(), 0);
	if (fd < 0)
		return -2;
	size = BBlseekCPP(fd, 0, 2);
	BBlseekCPP(fd, 0, 0);
	if (size > 0x10000)
		size = 0x10000;
	memset(g_infoBuf, 0, 0x10000);
	r = BBreadCPP(fd, g_infoBuf, size);
	BBcloseCPP(fd);
	if (r < 0)
		return -2;
	ctype = 255;
	cur = g_infoBuf;
	while ((line = strsep(&cur, "\n")) != 0 && strlen(line) != 0) {
		int k = infoParseLine(line);

		if ((unsigned int)k < 28)
			strcpy(g_valBuf[k], g_infoVal);
		if (k == 25)
			ctype = strtol(g_infoVal, 0, 10);
	}
	if (ctype == type)
		return 0;
	return 1;
}

/* The dirent cache and the number of valid records in it. */

/* Fills the dirent cache with the entries of "hdd0:" (at most 1024); returns
 * their number, -1 if hdd0: cannot be listed. */
int bnReadDirent(void)
{
	int fd;
	int r;

	g_direntCount = 0;
	memset(g_direntBuf, 0, sizeof(g_direntBuf));

	fd = scepfsDopen("hdd0:");
	if (fd < 0)
		return -1;

	r = 0;
	while (g_direntCount < 1024 && g_direntStop == 0) {
		r = scepfsDread(fd, &g_direntBuf[g_direntCount]);
		if (r <= 0)
			break;
		g_direntCount++;
	}

	scepfsDclose(fd);
	return r < 0 ? -1 : g_direntCount;
}

/* Fills an application descriptor from a cache record; the size is
 * counted in halves. */
static void makeDirentDesc(iox_dirent_t *rec, struct DirentDesc *out, short index)
{
	out->half = rec->stat.size >> 1;
	out->tag = 0x8407;
	out->index = index;
	out->src = rec;
}

/* Writes entry i of the name table. */
static void setNameRec(int i, short id, short type, const char *name)
{
	g_nameTab[i].id = id;
	g_nameTab[i].type = type;
	strcpy(g_nameTab[i].name, name);
}

/* Copies field `n` (counting dots) of `src` into `dst`, up to the next dot
 * or the end of the string. */
static void extractDotField(int n, const char *src, char *dst)
{
	*dst = 0;
	while (n > 0) {
		if (*src == '.')
			n--;
		else if (*src == 0)
			return;
		src++;
	}
	while (*src != '.' && *src != 0)
		*dst++ = *src++;
	*dst = 0;
}

/* Returns 1 if fields 1 and 2 (dot-separated) of the two names differ in
 * their first 32 bytes, 0 otherwise. */
static int direntFieldsDiffer(const char *a, const char *b)
{
	char fa[48];
	char fb[48];

	extractDotField(1, a, fa);
	extractDotField(1, b, fb);
	if (strncmp(fa, fb, 32) != 0)
		return 1;
	extractDotField(2, a, fa);
	extractDotField(2, b, fb);
	return strncmp(fa, fb, 32) != 0;
}

/* Builds the name and application tables from the dirent cache in three
 * passes (entries starting with '_' are skipped):
 *  1. uninstalled "XP..." entries (capital letter + 'P') each start a new
 *     application, ids from 2;
 *  2. uninstalled "XC..." entries join the application with the same fields
 *     1 and 2 of the name, or start a new one with id + 1026;
 *  3. installed entries join, by name, the application of their type 1 entry.
 * Each join adds half of the entry size to the descriptor. Stops as soon as
 * g_direntStop is set. */
static void buildAppTables(void)
{
	static int g_scanCount;
	static int g_appCount;
	static int g_linkCount;
	static int g_dataCount;
	static short g_nextId;
	iox_dirent_t *rec;
	struct DirentDesc *slot;
	int i;
	int j;
	int id = 0;

	g_scanCount = g_direntCount;
	g_instCount = g_direntCount;
	memset(g_nameTab, 0, sizeof(g_nameTab));
	memset(g_appSlots, 0, sizeof(g_appSlots));

	slot = g_appSlotBase;
	g_nextId = 2;
	g_appCount = 0;
	for (i = 0, rec = g_direntBuf; i < g_scanCount; i++, rec++) {
		if (g_direntStop)
			return;
		if (rec->name[0] != '_' && !(rec->stat.attr & 1) && (rec->name[0] >= 'A' && rec->name[0] <= 'Z')
		    && rec->name[1] == 'P') {
			makeDirentDesc(rec, slot, g_nextId);
			setNameRec(i, g_nextId, 1, rec->name);
			g_nextId++;
			g_appCount++;
			slot++;
			if (g_appCount >= g_appSlotMax)
				break;
		}
	}

	g_linkCount = 0;
	for (i = 0, rec = g_direntBuf; i < g_scanCount; i++, rec++) {
		if (g_direntStop)
			return;
		if (rec->name[0] != '_' && !(rec->stat.attr & 1)) {
			if (g_appCount >= g_appSlotMax)
				break;
			if ((rec->name[0] >= 'A' && rec->name[0] <= 'Z') && rec->name[1] == 'C') {
				slot = g_appSlotBase;
				for (j = 0; j < g_appCount; slot++, j++) {
					if (direntFieldsDiffer(slot->src->name, rec->name) == 0) {
						slot->half += rec->stat.size >> 1;
						g_linkCount++;
						setNameRec(i, slot->index, 1, rec->name);
						break;
					}
				}
				if (j == g_appCount) {
					makeDirentDesc(rec, &g_appSlotBase[j], g_nextId + 1026);
					setNameRec(i, g_nextId + 1026, 1, rec->name);
					g_nextId++;
					g_appCount++;
				}
			}
		}
	}

	g_dataCount = 0;
	for (i = 0, rec = g_direntBuf; i < g_scanCount; i++, rec++) {
		if (g_direntStop)
			return;
		if (rec->name[0] != '_' && (rec->stat.attr & 1)) {
			for (j = 0; j < g_instCount; j++)
				if (g_nameTab[j].type == 1 && strcmp(g_nameTab[j].name, rec->name) == 0) {
					id = g_nameTab[j].id;
					break;
				}
			if (j == g_instCount)
				break;
			slot = g_appSlotBase;
			for (j = 0; j < g_appCount; slot++, j++) {
				if (slot->index == id) {
					slot->half += rec->stat.size >> 1;
					g_dataCount++;
					setNameRec(i, id, 2, rec->name);
					break;
				}
			}
		}
	}
	g_appSlotCount = g_appCount;
}

#include <map>
#include <vector>
#include "../../ref/sqlite-2.7.2/gen/sqlite.h"

/* The database the import keeps up to date; the tests point it elsewhere. */
const char *bnGameDbPath = "/opt1/database/sqlite/game.db";

/* The columns of sce_game. The pages read it with `select *` and the
   installers insert rows positionally, so they never change. */
static const char g_sceGameColumns[] =
    "type int, key_flag int, inst_size int8, title varchar, title_id varchar, title_sub_id varchar, "
    "release_date varchar, developer_id varchar, publisher_id varchar, note varchar, content_web varchar, "
    "install_date varchar, table_version int, version int, uri varchar, resource_path varchar, "
    "image_topviewflag int, image_type int, image_count int, image_viewsec int, copyright_viewflag int, "
    "copyright_imgcount int, genre varchar, parental_lock int, effective_date varchar, expire_date varchar, "
    "area varchar, violence_flag int, content_type int, content_subtype int, content_attr int";

/* sce_game_scan remembers what the import saw of each application: its main
   partition, creation time and total size, what the import did with it, and
   the rowid and uri of its sce_game row. An application whose partition, time
   and size are unchanged, and whose row is still there, is not read again. */
static const char g_scanColumns[] = "part varchar, ctime varchar, size int, state int, game_oid int, uri varchar";

/* State of an application in sce_game_scan. */
enum {
	SCAN_INSERTED = 0, /* it has a row in sce_game */
	SCAN_SKIPPED = 1,  /* left out: its info.sys has another content_type */
	SCAN_RETRY = 2     /* a read failed: it has a row, but it is read again next time */
};

struct ScanRow {
	string part;
	string ctime;
	unsigned size;
	int state;
	int gameOid;
	string uri;
	int seen;
};

/* An application read by this import, and what to write for it. */
struct GameChange {
	int app;          /* index in g_appSlots */
	ScanRow *old;     /* its sce_game_scan row, if any */
	int state;
	string insert;    /* the INSERT of its sce_game row, empty when skipped */
	string uri;
};

static int collectScanRow(void *arg, int n, char **v, char **)
{
	vector<ScanRow> *rows = (vector<ScanRow> *)arg;
	ScanRow r;

	if (n != 6 || v[0] == 0)
		return 0;
	r.part = v[0];
	r.ctime = v[1] ? v[1] : "";
	r.size = v[2] ? strtoul(v[2], 0, 10) : 0;
	r.state = v[3] ? atoi(v[3]) : SCAN_RETRY;
	r.gameOid = v[4] ? atoi(v[4]) : 0;
	r.uri = v[5] ? v[5] : "";
	r.seen = 0;
	rows->push_back(r);
	return 0;
}

static int collectGameRow(void *arg, int n, char **v, char **)
{
	if (n == 2 && v[0] != 0)
		(*(map<int, string> *)arg)[atoi(v[0])] = v[1] ? v[1] : "";
	return 0;
}

static int collectName(void *arg, int n, char **v, char **)
{
	if (n == 1 && v[0] != 0)
		((vector<string> *)arg)->push_back(v[0]);
	return 0;
}

/* Runs one statement built with sqlite_mprintf (%q escapes the quotes);
   returns 1 on success. */
static int dbRun(sqlite *db, char *sql)
{
	int rc;

	if (sql == 0)
		return 0;
	rc = sqlite_exec(db, sql, 0, 0, 0);
	sqlite_freemem(sql);
	return rc == SQLITE_OK;
}

/* Creates sce_game and sce_game_scan if they are missing. Without
   sce_game_scan every application is read again, and the rows left by an
   import that kept no record go away as orphans (updateGameTable). */
static int createGameTables(sqlite *db)
{
	vector<string> names;
	unsigned i;
	int game = 0;
	int scan = 0;

	if (sqlite_exec(db, "select name from sqlite_master where type = 'table'", collectName, &names, 0) != SQLITE_OK)
		return 0;
	for (i = 0; i < names.size(); i++) {
		if (names[i] == "sce_game")
			game = 1;
		else if (names[i] == "sce_game_scan")
			scan = 1;
	}
	if (!game && !dbRun(db, sqlite_mprintf("create table sce_game(%s)", g_sceGameColumns)))
		return 0;
	/* a table of another layout is recreated */
	if (scan
	    && sqlite_exec(db, "select part, ctime, size, state, game_oid, uri from sce_game_scan where 0", 0, 0, 0)
	        == SQLITE_ERROR) {
		if (!dbRun(db, sqlite_mprintf("drop table sce_game_scan")))
			return 0;
		scan = 0;
	}
	if (!scan && !dbRun(db, sqlite_mprintf("create table sce_game_scan(%s)", g_scanColumns)))
		return 0;
	return 1;
}

/* Appends `value` to `sql` as a quoted SQL string. */
static void sqlString(string &sql, const char *value)
{
	char *q = sqlite_mprintf("'%q'", value);

	sql += q;
	sqlite_freemem(q);
}

/* Appends `value` to `sql` as an integer, or `def` if it is empty. */
static void sqlInt(string &sql, const char *value, int def)
{
	char buf[16];

	sprintf(buf, "%d", *value != 0 ? (int)strtol(value, 0, 10) : def);
	sql += buf;
}

/* The creation time of application i as yyyymmddhhmmss. */
static void appCtime(int i, char *buf)
{
	unsigned char *t = g_appSlots[i].src->stat.ctime;

	sprintf(buf, "%04d%02d%02d%02d%02d%02d", t[7] << 8 | t[6], t[5], t[4], t[3], t[2], t[1]);
}

/* Reads application i like the stock import did (SYSTEM.CNF for the title id,
   /res/info.sys, icon.sys when there is no info.sys) and builds the INSERT of
   its sce_game row in `sql`, with its uri in `uri`. The values come from
   g_valBuf, filled here. Returns its SCAN_* state: SCAN_RETRY when a read
   failed, so that a game is not kept with what an error left out. */
static int readApplication(int i, string &sql, string &uri)
{
	char size[16];
	int state = SCAN_INSERTED;
	int r;

	memset(g_valBuf, 0, sizeof(g_valBuf));
	appCtime(i, g_valBuf[8]);
	sprintf(g_valBuf[11], "pfs:/%s", g_appSlots[i].src->name);
	sprintf(g_valBuf[12], "pfs:/%s/%s", g_appSlots[i].src->name, "res");
	r = 0;
	if (strlen(g_appSlots[i].src->name) >= 13)
		r = readBootId(g_valBuf[11]);
	if (r == -1 || r == -2 || r == -3 || r == -7)
		state = SCAN_RETRY;
	if (strlen(g_appSlots[i].src->name) < 13)
		g_valBuf[1][0] = 0;
	else if (r == 0)
		sprintf(g_valBuf[1], "%s", g_bootId);
	else if (g_appSlots[i].src->name[2] == '.') {
		g_valBuf[1][0] = g_appSlots[i].src->name[3];
		g_valBuf[1][1] = g_appSlots[i].src->name[4];
		g_valBuf[1][2] = g_appSlots[i].src->name[5];
		g_valBuf[1][3] = g_appSlots[i].src->name[6];
		g_valBuf[1][4] = '-';
		g_valBuf[1][5] = g_appSlots[i].src->name[8];
		g_valBuf[1][6] = g_appSlots[i].src->name[9];
		g_valBuf[1][7] = g_appSlots[i].src->name[10];
		g_valBuf[1][8] = g_appSlots[i].src->name[11];
		g_valBuf[1][9] = g_appSlots[i].src->name[12];
	}
	sprintf(size, "%u", g_appSlots[i].half >> 10);

	r = readInfoSys(g_valBuf[11], 255);
	if (r == 1)
		return state == SCAN_RETRY ? SCAN_RETRY : SCAN_SKIPPED;
	if (r == -2)
		state = SCAN_RETRY;
	sql = "insert into sce_game values(";
	if (r < 0) {
		/* no info.sys: the title of icon.sys, defaults for the rest */
		r = readTitle(g_valBuf[11]);
		if (r == -1 || r == -2 || r == -3 || r == -7)
			state = SCAN_RETRY;
		if (strlen(g_valBuf[1]) == 0)
			g_valBuf[0][0] = 0;
		sql += strlen(g_valBuf[0]) == 0 ? "-1, 0, " : "0, 0, ";
		sql += size;
		sql += ", ";
		sqlString(sql, g_valBuf[0]);
		sql += ", ";
		sqlString(sql, g_valBuf[1]);
		sql += ", '0', '', '', '', '', '', ";
		sqlString(sql, g_valBuf[8]);
		sql += ", 4096, 1, ";
		sqlString(sql, g_valBuf[11]);
		sql += ", ";
		sqlString(sql, g_valBuf[12]);
		sql += ", 0, 0, 0, 600, 0, 0, '', 1, '0', '0', 'J', 0, 255, 0, 0)";
		uri = g_valBuf[11];
		return state;
	}
	/* info.sys: its values, with the defaults above where a key is missing */
	sql += "0, 0, ";
	sql += size;
	for (r = 0; r <= 8; r++) {
		sql += ", ";
		sqlString(sql, g_valBuf[r]);
	}
	sql += ", ";
	sqlInt(sql, g_valBuf[9], 4096);
	sql += ", ";
	sqlInt(sql, g_valBuf[10], 1);
	sql += ", ";
	sqlString(sql, g_valBuf[11]);
	sql += ", ";
	sqlString(sql, g_valBuf[12]);
	sql += ", ";
	sqlInt(sql, g_valBuf[13], 0);
	sql += ", ";
	sqlInt(sql, g_valBuf[14], 0);
	sql += ", ";
	sqlInt(sql, g_valBuf[15], 0);
	sql += ", ";
	sqlInt(sql, g_valBuf[16], 600);
	sql += ", ";
	sqlInt(sql, g_valBuf[17], 0);
	sql += ", ";
	sqlInt(sql, g_valBuf[18], 0);
	sql += ", ";
	sqlString(sql, g_valBuf[19]);
	sql += ", ";
	sqlInt(sql, g_valBuf[20], 1);
	sql += ", ";
	sqlString(sql, g_valBuf[21]);
	sql += ", ";
	sqlString(sql, g_valBuf[22]);
	sql += ", ";
	sqlString(sql, *g_valBuf[23] ? g_valBuf[23] : "J");
	sql += ", ";
	sqlInt(sql, g_valBuf[24], 0);
	sql += ", ";
	sqlInt(sql, g_valBuf[25], 255);
	sql += ", ";
	sqlInt(sql, g_valBuf[26], 0);
	sql += ", 0)";
	uri = g_valBuf[11];
	return state;
}

/* Whether the sce_game row that `row` points to is still there and still the
   one the import wrote (anything else may have deleted or rebuilt rows, and
   SQLite reuses the rowid of a deleted last row). */
static int ownsRow(map<int, string> &games, ScanRow &row)
{
	map<int, string>::iterator g = games.find(row.gameOid);

	return g != games.end() && (*g).second == row.uri;
}

/* The rows of games that no sce_game_scan row points to. */
#define ORPHANS(verb) verb " from sce_game where content_attr = 0 and oid not in (select game_oid from sce_game_scan)"

/* Brings sce_game up to date with the applications of the disk, in three
   steps, so that game.db is locked only while it is written:
   1. each application is matched with its sce_game_scan row; it is read again
      if it has none, if its creation time or size changed, if its last read
      failed, or if its sce_game row is gone or holds another game;
   2. those applications are read from the disk;
   3. in one transaction, opened only if something changed: their rows are
      replaced; after a complete listing, the rows of the applications no
      longer on the disk are deleted; after a complete scan, the rows that no
      scan row points to (an installer page inserts one for what it installs)
      are deleted too.
   A stop request ends step 2 early: what was read is written. */
static void updateGameTable(int listingComplete)
{
	vector<ScanRow> rows;
	map<string, ScanRow *> byPart;
	map<int, string> games;
	vector<GameChange> changes;
	vector<string> orphans;
	sqlite *db;
	int scanComplete;
	int inTr = 0;
	int ok = 1;
	int i;
	unsigned j;

	db = sqlite_open(bnGameDbPath, 0666, 0);
	if (db == 0)
		return;
	sqlite_busy_timeout(db, 10000);
	if (!createGameTables(db)
	    || sqlite_exec(db, "select part, ctime, size, state, game_oid, uri from sce_game_scan", collectScanRow,
	           &rows, 0)
	        != SQLITE_OK
	    || sqlite_exec(db, "select oid, uri from sce_game where content_attr = 0", collectGameRow, &games, 0)
	        != SQLITE_OK) {
		sqlite_close(db);
		return;
	}

	/* 1: the scan rows by partition name (the first one if a name repeats);
	   rows does not change from here on, so the pointers stay valid */
	for (j = 0; j < rows.size(); j++)
		if (byPart.find(rows[j].part) == byPart.end())
			byPart[rows[j].part] = &rows[j];
	for (i = 0; i < g_appSlotCount; i++) {
		map<string, ScanRow *>::iterator r = byPart.find(g_appSlots[i].src->name);
		GameChange c;
		char ctime[32];

		c.app = i;
		c.old = r != byPart.end() ? (*r).second : 0;
		if (c.old != 0) {
			c.old->seen = 1;
			appCtime(i, ctime);
			if (c.old->ctime == ctime && c.old->size == g_appSlots[i].half
			    && (c.old->state == SCAN_SKIPPED
			        || (c.old->state == SCAN_INSERTED && ownsRow(games, *c.old))))
				continue;
		}
		changes.push_back(c);
	}

	/* 2: the database is closed, so the pages can use it meanwhile */
	sqlite_close(db);
	for (j = 0; j < changes.size() && g_direntStop == 0; j++)
		changes[j].state = readApplication(changes[j].app, changes[j].insert, changes[j].uri);
	scanComplete = listingComplete && j == changes.size();
	changes.erase(changes.begin() + j, changes.end());

	/* 3: the rows may have changed while the database was closed */
	db = sqlite_open(bnGameDbPath, 0666, 0);
	if (db == 0)
		return;
	sqlite_busy_timeout(db, 10000);
	games.clear();
	if (sqlite_exec(db, "select oid, uri from sce_game where content_attr = 0", collectGameRow, &games, 0)
	        != SQLITE_OK
	    || (scanComplete && sqlite_exec(db, ORPHANS("select oid"), collectName, &orphans, 0) != SQLITE_OK)) {
		sqlite_close(db);
		return;
	}
	for (j = 0; ok && j < changes.size(); j++) {
		GameChange &c = changes[j];
		char ctime[32];
		int oid = 0;

		if (!inTr)
			ok = inTr = dbRun(db, sqlite_mprintf("begin"));
		if (ok && c.old != 0 && ownsRow(games, *c.old))
			ok = dbRun(db, sqlite_mprintf("delete from sce_game where oid = %d", c.old->gameOid));
		if (ok && c.old != 0)
			ok = dbRun(db,
			    sqlite_mprintf("delete from sce_game_scan where part = '%q'", c.old->part.c_str()));
		if (ok && c.state != SCAN_SKIPPED && c.insert.length() != 0) {
			/* a row of the same game that no scan row owns (left by an older
			   import or inserted by an installer page) would be a duplicate */
			ok = dbRun(db, sqlite_mprintf(ORPHANS("delete") " and uri = '%q'", c.uri.c_str()))
			    && sqlite_exec(db, c.insert.c_str(), 0, 0, 0) == SQLITE_OK;
			oid = sqlite_last_insert_rowid(db);
		} else
			c.uri = "";
		appCtime(c.app, ctime);
		if (ok)
			ok = dbRun(db,
			    sqlite_mprintf("insert into sce_game_scan values('%q', '%q', %u, %d, %d, '%q')",
			        g_appSlots[c.app].src->name, ctime, g_appSlots[c.app].half, c.state, oid,
			        c.uri.c_str()));
	}
	for (j = 0; ok && listingComplete && j < rows.size(); j++) {
		if (rows[j].seen)
			continue;
		if (!inTr)
			ok = inTr = dbRun(db, sqlite_mprintf("begin"));
		if (ok && ownsRow(games, rows[j]))
			ok = dbRun(db, sqlite_mprintf("delete from sce_game where oid = %d", rows[j].gameOid));
		if (ok)
			ok = dbRun(db,
			    sqlite_mprintf("delete from sce_game_scan where part = '%q'", rows[j].part.c_str()));
	}
	if (ok && scanComplete && (orphans.size() != 0 || inTr)) {
		if (!inTr)
			ok = inTr = dbRun(db, sqlite_mprintf("begin"));
		if (ok)
			ok = dbRun(db, sqlite_mprintf(ORPHANS("delete")));
	}
	if (inTr)
		sqlite_exec(db, ok ? "commit" : "rollback", 0, 0, 0);
	sqlite_close(db);
}

/* g_scanRunning: a scan thread runs; g_scanAgain: a start arrived meanwhile,
   so the thread scans once more before it ends. Both under g_scanLock. */
static pthread_mutex_t g_scanLock = PTHREAD_MUTEX_INITIALIZER;
static int g_scanRunning = 0;
static int g_scanAgain = 0;

/* Thread entry of bnGameImportStart: the whole scan, run as root. When hdd0:
   cannot be listed game.db is left as it is (an empty list would delete every
   game); when the listing may be partial (the cache is full), nothing is
   deleted. A stop request ends the scan with state 4, unless a start arrived
   after it: then the scan starts over. */
static void bnScanDirents(void)
{
	becomeRoot();
	for (;;) {
		g_direntState = 2;
		int listed = bnReadDirent();
		if (g_direntStop == 0) {
			g_direntState = 3;
			buildAppTables();
			if (g_direntStop == 0 && listed >= 0)
				updateGameTable(listed < 1024 && g_appSlotCount < g_appSlotMax);
		}
		pthread_mutex_lock(&g_scanLock);
		if (!g_scanAgain)
			break;
		g_scanAgain = 0;
		g_direntStop = 0;
		pthread_mutex_unlock(&g_scanLock);
	}
	restoreUid();
	if (g_direntStop == 0)
		g_direntState = 0;
	else
		g_direntState = 4;
	g_direntStop = 0;
	g_scanRunning = 0;
	pthread_mutex_unlock(&g_scanLock);
}

/* Starts the scan on a detached thread; Stat returns its state, Cancel
 * stops it. While a scan runs no second thread starts (they share the
 * tables): the running one is stopped and starts over. */
int bnGameImportStart()
{
	pthread_attr_t attr;
	int r;

	pthread_mutex_lock(&g_scanLock);
	if (g_scanRunning) {
		g_scanAgain = 1;
		g_direntStop = 1;
		pthread_mutex_unlock(&g_scanLock);
		return 0;
	}
	g_scanRunning = 1;
	g_direntStop = 0;
	g_direntState = 1;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	r = pthread_create(&g_scanThread, &attr, (void *(*)(void *))bnScanDirents, 0);
	if (r != 0)
		g_scanRunning = 0;
	pthread_mutex_unlock(&g_scanLock);
	return r;
}

int bnGameImportStat()
{
	return g_direntState;
}

int bnGameImportCancel()
{
	pthread_mutex_lock(&g_scanLock);
	g_scanAgain = 0;
	g_direntStop = 1;
	pthread_mutex_unlock(&g_scanLock);
	return 0;
}
