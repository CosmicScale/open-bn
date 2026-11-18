/*
 * The disk in memory of fakepfs.h and the scepfs* functions over it. The
 * import runs on its own thread while the test thread only waits, so the
 * state needs no lock.
 */

#include <stdio.h>
#include <string.h>
#include <vector>
#include "lib/scepfsd.h"
#include "fakepfs.h"

#define FAKE_ENOENT -2
#define FAKE_EIO -5
#define FAKE_EBADF -9
#define FAKE_ENODEV -19

struct FakeFd {
	int dir;        /* 1: a listing of hdd0:, `pos` is the next entry */
	string data;    /* a file: its content */
	long pos;
	int fail;       /* its reads fail with -EIO */
};

static vector<FakePart *> s_parts;
static map<string, string> s_mounts; /* "pfsN:" -> partition name */
static map<int, FakeFd> s_fds;
static map<string, int> s_accesses;
static int s_nextFd = 3;

int fakeUnexpected = 0;
int fakeListFails = 0;
void (*fakeOnAccess)(const char *name) = 0;

void fakeReset(void)
{
	unsigned i;

	for (i = 0; i < s_parts.size(); i++)
		delete s_parts[i];
	s_parts.clear();
	s_mounts.clear();
	s_fds.clear();
	s_accesses.clear();
	fakeUnexpected = 0;
	fakeListFails = 0;
	fakeOnAccess = 0;
}

FakePart &fakeAddPartition(const char *name, unsigned size)
{
	FakePart *p = new FakePart;

	p->name = name;
	p->attr = 0;
	p->size = size;
	p->failReads = 0;
	p->header.assign(512, '\0'); /* no PS2ICON3D header */
	fakeSetCtime(*p, 2026, 1, 1, 12, 0, 0);
	s_parts.push_back(p);
	return *p;
}

FakePart *fakeFind(const char *name)
{
	unsigned i;

	for (i = 0; i < s_parts.size(); i++)
		if (s_parts[i]->name == name)
			return s_parts[i];
	return 0;
}

void fakeRemove(const char *name)
{
	unsigned i;

	for (i = 0; i < s_parts.size(); i++)
		if (s_parts[i]->name == name) {
			delete s_parts[i];
			s_parts.erase(s_parts.begin() + i);
			return;
		}
}

void fakeSetCtime(FakePart &p, int year, int month, int day, int hour, int min, int sec)
{
	p.ctime[0] = 0;
	p.ctime[1] = sec;
	p.ctime[2] = min;
	p.ctime[3] = hour;
	p.ctime[4] = day;
	p.ctime[5] = month;
	p.ctime[6] = year & 0xff;
	p.ctime[7] = year >> 8;
}

/* The header goes in the first 512 bytes, each file at the next 512-byte
   boundary, as the installer writes them. */
string fakeHeader(const string &systemCnf, const string &iconSys)
{
	PS2Icon3DHeader h;
	string out;

	memset(&h, 0, sizeof(h));
	strcpy(h.magic, "PS2ICON3D");
	h.systemCnf.offset = 0x200;
	h.systemCnf.size = systemCnf.length();
	h.iconSys.offset = 0x200 + (systemCnf.length() + 511) / 512 * 512;
	h.iconSys.size = iconSys.length();
	out.assign((const char *)&h, sizeof(h));
	out += systemCnf;
	out.append(h.iconSys.offset - out.length(), '\0');
	out += iconSys;
	out.append((out.length() + 511) / 512 * 512 - out.length(), '\0');
	return out;
}

string fakeInfoSys(const char *title, const char *titleId, int contentType)
{
	char buf[2048];

	sprintf(buf,
	    "title = %s\ntitle_id = %s\ntitle_sub_id = 0\nrelease_date = \ndeveloper_id = \n"
	    "publisher_id = Publisher\nnote = \ncontent_web = \nimage_topviewflag = 0\nimage_type = 0\n"
	    "image_count = 1\nimage_viewsec = 600\ncopyright_viewflag = 0\ncopyright_imgcount = 0\n"
	    "genre = \nparental_lock = 1\neffective_date = 0\nexpire_date = 0\nviolence_flag = 0\n"
	    "content_type = %d\ncontent_subtype = 0\n",
	    title, titleId, contentType);
	return buf;
}

FakePart &fakeAddGame(const char *name, const char *title, const char *bootFile, int contentType)
{
	FakePart &p = fakeAddPartition(name, 0x200000);
	char cnf[256];
	char icon[512];
	char titleId[16];

	sprintf(cnf, "BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = PAL\r\n", bootFile);
	sprintf(icon, "PS2X\ntitle0 = %s\ntitle1 = \n", title);
	/* "SLES_504.62" -> "SLES-50462" */
	sprintf(titleId, "%.4s-%.3s%.2s", bootFile, bootFile + 5, bootFile + 9);
	p.header = fakeHeader(cnf, icon);
	p.files["res/info.sys"] = fakeInfoSys(title, titleId, contentType);
	return p;
}

int fakeAccesses(const char *name)
{
	map<string, int>::iterator i;
	int n = 0;

	if (name != 0)
		return s_accesses.find(name) != s_accesses.end() ? s_accesses[name] : 0;
	for (i = s_accesses.begin(); i != s_accesses.end(); i++)
		n += (*i).second;
	return n;
}

void fakeResetCounts(void)
{
	s_accesses.clear();
}

int fakeOpenFds(void)
{
	return s_fds.size();
}

static void noteAccess(FakePart *p)
{
	s_accesses[p->name]++;
	if (fakeOnAccess != 0)
		fakeOnAccess(p->name.c_str());
}

/* "pfsN:/res/info.sys": the mounted partition and the path inside it. */
static FakePart *resolve(const char *path, string &inside)
{
	const char *colon = strchr(path, ':');
	map<string, string>::iterator m;

	if (colon == 0)
		return 0;
	m = s_mounts.find(string(path, colon + 1 - path));
	if (m == s_mounts.end())
		return 0;
	inside = colon + 1;
	while (inside.length() != 0 && inside[0] == '/')
		inside.erase(0, 1);
	return fakeFind((*m).second.c_str());
}

static int isMounted(FakePart *p)
{
	map<string, string>::iterator m;

	for (m = s_mounts.begin(); m != s_mounts.end(); m++)
		if ((*m).second == p->name)
			return 1;
	return 0;
}

static int newFd(const FakeFd &f)
{
	s_fds[s_nextFd] = f;
	return s_nextFd++;
}

extern "C" {

int scepfsInit(void)
{
	return 0;
}

void scepfsExit(void)
{}

int scepfsMount(const char *mp, const char *dev, int, void *, unsigned int)
{
	FakePart *p;

	if (strncmp(dev, "hdd0:", 5) != 0) {
		fakeUnexpected++;
		return -6;
	}
	p = fakeFind(dev + 5);
	if (p == 0)
		return FAKE_ENOENT;
	if (s_mounts.find(mp) != s_mounts.end())
		return -16;
	noteAccess(p);
	s_mounts[mp] = p->name;
	return 0;
}

int scepfsUmount(const char *mp)
{
	if (s_mounts.find(mp) == s_mounts.end())
		return FAKE_ENODEV;
	s_mounts.erase(mp);
	return 0;
}

int scepfsGetstat(const char *path, void *stat)
{
	iox_stat_t *st = (iox_stat_t *)stat;
	string inside;
	FakePart *p = resolve(path, inside);

	if (p == 0)
		return FAKE_ENODEV;
	noteAccess(p);
	if (p->files.find(inside) == p->files.end())
		return FAKE_ENOENT;
	memset(st, 0, sizeof(*st));
	st->mode = 0x2000 | 0x1ff;
	st->size = p->files[inside].length();
	return 0;
}

/* "hdd0:<name>" opens the raw partition, "pfsN:/<path>" a file of a mounted one. */
int scepfsOpen(const char *path, int flags, ...)
{
	FakeFd f;
	FakePart *p;
	string inside;

	if (flags & 2) {
		fakeUnexpected++; /* the import only reads */
		return -13;
	}
	if (strncmp(path, "hdd0:", 5) == 0) {
		p = fakeFind(path + 5);
		if (p == 0)
			return FAKE_ENOENT;
		if (isMounted(p))
			return -16; /* pfsd: the partition is mounted */
		f.data = p->header;
	} else {
		p = resolve(path, inside);
		if (p == 0)
			return FAKE_ENODEV;
		if (p->files.find(inside) == p->files.end()) {
			noteAccess(p);
			return FAKE_ENOENT;
		}
		f.data = p->files[inside];
	}
	noteAccess(p);
	f.fail = p->failReads || (inside.length() != 0 && inside == p->failFile);
	f.dir = 0;
	f.pos = 0;
	return newFd(f);
}

int scepfsRead(int fd, void *buf, unsigned int size)
{
	map<int, FakeFd>::iterator i = s_fds.find(fd);
	FakeFd *f;
	unsigned n;

	if (i == s_fds.end() || (*i).second.dir)
		return FAKE_EBADF;
	f = &(*i).second;
	if (f->fail)
		return FAKE_EIO;
	n = f->pos < (long)f->data.length() ? f->data.length() - f->pos : 0;
	if (n > size)
		n = size;
	memcpy(buf, f->data.data() + f->pos, n);
	f->pos += n;
	return n;
}

long scepfsLseek(int fd, long offset, int whence)
{
	map<int, FakeFd>::iterator i = s_fds.find(fd);
	FakeFd *f;

	if (i == s_fds.end() || (*i).second.dir)
		return FAKE_EBADF;
	f = &(*i).second;
	if (whence == 0)
		f->pos = offset;
	else if (whence == 1)
		f->pos += offset;
	else
		f->pos = f->data.length() + offset;
	if (f->pos < 0)
		f->pos = 0;
	return f->pos;
}

int scepfsClose(int fd)
{
	if (s_fds.find(fd) == s_fds.end())
		return FAKE_EBADF;
	s_fds.erase(fd);
	return 0;
}

int scepfsDopen(const char *path)
{
	FakeFd f;

	if (strcmp(path, "hdd0:") != 0 || fakeListFails)
		return FAKE_EIO;
	f.dir = 1;
	f.pos = 0;
	f.fail = 0;
	return newFd(f);
}

int scepfsDread(int fd, void *dirent)
{
	iox_dirent_t *d = (iox_dirent_t *)dirent;
	map<int, FakeFd>::iterator i = s_fds.find(fd);
	FakePart *p;

	if (i == s_fds.end() || !(*i).second.dir)
		return FAKE_EBADF;
	if ((*i).second.pos >= (long)s_parts.size())
		return 0;
	p = s_parts[(*i).second.pos++];
	memset(d, 0, sizeof(*d));
	d->stat.mode = 0x0100;
	d->stat.attr = p->attr;
	d->stat.size = p->size;
	memcpy(d->stat.ctime, p->ctime, 8);
	strncpy(d->name, p->name.c_str(), sizeof(d->name) - 1);
	return 1;
}

int scepfsDclose(int fd)
{
	return scepfsClose(fd);
}

/* BBclose syncs the mount point after every close, reads included. */
int scepfsSync(const char *mp, int)
{
	return s_mounts.find(mp) != s_mounts.end() ? 0 : FAKE_ENODEV;
}

/* The rest of the interface: the code under test must not call it. */
#define FAKE_UNEXPECTED(proto)                                                                                         \
	int proto                                                                                                      \
	{                                                                                                              \
		fakeUnexpected++;                                                                                      \
		return FAKE_EIO;                                                                                       \
	}

FAKE_UNEXPECTED(scepfsWrite(int, const void *, unsigned int))
FAKE_UNEXPECTED(scepfsChstat(const char *, void *, unsigned int))
FAKE_UNEXPECTED(scepfsMkdir(const char *, int))
FAKE_UNEXPECTED(scepfsRmdir(const char *))
FAKE_UNEXPECTED(scepfsChdir(const char *))
FAKE_UNEXPECTED(scepfsRemove(const char *))
FAKE_UNEXPECTED(scepfsRename(const char *, const char *))
FAKE_UNEXPECTED(scepfsIoctl(int, int, int))
FAKE_UNEXPECTED(scepfsIoctl2(int, int, void *, int, void *, int))
FAKE_UNEXPECTED(scepfsSymlink(const char *, const char *))
FAKE_UNEXPECTED(scepfsReadlink(const char *, char *, int))
FAKE_UNEXPECTED(scepfsFormat(const char *, const char *, void *, unsigned int))
FAKE_UNEXPECTED(scepfsDevctl(const char *, int, void *, unsigned int, void *, unsigned int))
FAKE_UNEXPECTED(scepfsSetReadAhead(int))
FAKE_UNEXPECTED(scepfsGetReadAhead(void))
FAKE_UNEXPECTED(scepfsGetMountPoint(const char *, char *))

long long scepfsLseek64(int, long long, int)
{
	fakeUnexpected++;
	return FAKE_EIO;
}

} /* extern "C" */
