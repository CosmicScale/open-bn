/*
 * DNAS1 "AR nokey" layer: archive open, header and body reads and
 * personalization, mostly thin wrappers over DNAS2 functions.
 * Archives whose members are stored in plaintext are read directly from the file.
 */

#include <string.h>
#include <unistd.h>

#include "lib/scepfsd.h"
#include "dnas_ar.h"
#include "dnas2read.h"

static int g_ar_fd = -1; /* -1: no file open */
static int g_ar_plain;    /* members stored in plaintext, no DNAS2 profile */
static unsigned char g_ar_buf[16];
static const char g_ar_key_scei[] = "SCEI";
static const char g_ar_key_title[] = "title";
static const char g_ar_key_subtitle[] = "sub_title";

static int ar_nokey_read_raw(void *buf, int len, int flag);

/* Returns nonzero if the n bytes read at the start of the stream are the header of a
 * plaintext member: a directory or a file, not personalized, with a name. An encrypted
 * stream starts with the profile word 0x20005 instead. */
static int ar_plain_first(const struct dnas_archive_format *fmt, int n)
{
	if (n != 24)
		return 0;
	if (fmt->type != 1 && fmt->type != 2)
		return 0;
	return fmt->attr == 0 && fmt->namelen > 0 && fmt->namelen < 1024;
}

/* Reads the 16-byte archive header of the open file fd and returns the 64-bit
 * field at header + 8 in *size. When the stream after the header starts with a
 * plaintext member, the members are read as plaintext; otherwise loads the profile
 * (from prof when given, else from fd: __sce_dnas2_symbol65037). */
int _sce_dnas_ar_nokey_init(int fd, unsigned char *hdr, void *hdrArg, unsigned char *prof, unsigned int profLen,
    void *unused5, void *unused6, void *unused7, long long *size)
{
	struct dnas_archive_format first;
	int n;

	g_ar_fd = fd;
	g_ar_plain = 0;
	memset(g_ar_buf, 0, 16);
	if (scepfsRead(g_ar_fd, g_ar_buf, 16) != 16)
		return -2;
	n = scepfsRead(g_ar_fd, &first, 24);
	if (n < 0)
		return -2;
	if (n != 0 && scepfsLseek(g_ar_fd, -n, SEEK_CUR) < 0)
		return -2;
	if (ar_plain_first(&first, n))
		g_ar_plain = 1;
	else if (__sce_dnas2_symbol65037(prof, profLen, hdr, hdrArg, g_ar_fd, g_ar_key_scei, g_ar_key_title,
	             g_ar_key_subtitle, 1)
	    < 0)
		return -1;
	*size = *(long long *)(g_ar_buf + 8);
	return 0;
}

/* Returns nonzero if the n bytes at p are all zero. */
static int ar_is_zero(const void *p, int n)
{
	const unsigned char *b = p;
	int i;

	for (i = 0; i < n; i++) {
		if (b[i])
			return 0;
	}
	return 1;
}

/* Reads a 24-byte member header into `fmt` and the name that follows (at most
 * 1023 bytes, `name` holds 1024) into `name`; -10000 at the end of the archive. A plaintext archive
 * may end with zero padding, which also ends it; its members cannot be personalized. */
int _sce_dnas_ar_nokey_read_format(char *name, struct dnas_archive_format *fmt)
{
	int n;

	n = ar_nokey_read_raw(fmt, 24, 0);
	if (n == 0)
		return -10000;
	if (g_ar_plain && n >= 0 && ar_is_zero(fmt, n))
		return -10000;
	if (n != 24)
		return -2;
	if (fmt->namelen < 0 || fmt->namelen >= 1024)
		return -2;
	if (g_ar_plain && fmt->attr != 0)
		return -2;
	n = ar_nokey_read_raw(name, fmt->namelen, 0);
	if (n != fmt->namelen)
		return -2;
	name[n] = 0;
	return 0;
}

/* Skips `len` bytes of entry body, 1 KB at a time. */
int _sce_dnas_ar_nokey_skip_body(int len)
{
	unsigned char buf[1024];
	int chunk;
	int n;

	if (len == 0)
		return 0;
	do {
		chunk = (len < 1025) ? len : 1024;
		n = ar_nokey_read_raw(buf, chunk, 0);
		len -= n;
		if (n != chunk)
			return -1;
	} while (len != 0);
	return 0;
}

/* Reads `len` bytes of entry body. */
int _sce_dnas_ar_nokey_read_body(void *buf, int len, int flag)
{
	return ar_nokey_read_raw(buf, len, flag);
}

/* Reads len bytes straight from the file; returns the count read, short at the end of the file. */
static int ar_plain_read(void *buf, int len)
{
	unsigned char *p = buf;
	int done = 0;
	int n;

	while (done < len) {
		n = scepfsRead(g_ar_fd, p + done, len - done);
		if (n < 0)
			return n;
		if (n == 0)
			break;
		done += n;
	}
	return done;
}

/* Reads through the DNAS2 record reader (__sce_dnas2_symbol65039), or from the file
 * for a plaintext archive. */
static int ar_nokey_read_raw(void *buf, int len, int flag)
{
	if (g_ar_plain)
		return ar_plain_read(buf, len);
	return __sce_dnas2_symbol65039(buf, len);
}

/* Opens the personalization session (__sce_dnas2_symbol65038). */
int _sce_dnas_ar_nokey_init_personalize(void)
{
	return __sce_dnas2_symbol65038();
}

/* Returns the total length of the personalized data. */
int _sce_dnas_ar_nokey_get_personalize_length(void)
{
	return __sce_dnas2_symbol65036();
}

/* Closes the personalization session. */
int _sce_dnas_ar_nokey_exit_personalize(void)
{
	return __sce_dnas2_symbol65035();
}
