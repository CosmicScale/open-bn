/*
 * The archive of a download package (src/dnas/dnas_ar.c) read from a file on
 * the disk in memory: members stored in plaintext, and the switch to the DNAS2
 * reader when the stream after the header is encrypted.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "dnas/dnas_ar.h"
#include "lib/scepfsd.h"
#include "support/fakepfs.h"
#include "support/testutil.h"

#define PART "__contents"
#define FILE_PATH "pfs0:/pkg/archive"

static char s_name[1025];
static dnas_archive_format s_fmt;

static string le32(unsigned v)
{
	string s;

	for (int i = 0; i < 4; i++)
		s += (char)((v >> (8 * i)) & 0xff);
	return s;
}

/* The 16-byte archive header: magic, then the 64-bit value init returns. */
static string arHeader(unsigned size)
{
	return string("DAR1.00", 8) + le32(size) + le32(0);
}

/* A member: its 24-byte header, the name, then the body. */
static string member(int type, int attr, const char *name, const string &body)
{
	return le32(type) + le32(attr) + le32(strlen(name)) + le32(0) + le32(body.length()) + le32(0) + name + body;
}

/* The members of a package as the installer reads them: the empty dnas.dat
   marker, the install definition (a member without a name), a directory and
   a file. */
static string plainMembers(void)
{
	return member(2, 0, "../dnas.dat", "") + member(2, 0, "", string(276, 'd')) + member(1, 0, "res", "")
	    + member(2, 0, "res/info.sys", "title = Test\n");
}

/* Puts `data` in the file of the package and opens it; returns the descriptor. */
static int openArchive(const string &data)
{
	FakePart &p = fakeAddPartition(PART, 128);
	int fd;

	p.files["pkg/archive"] = data;
	TEST_ASSERT_EQUAL_INT(0, scepfsMount("pfs0:", "hdd0:" PART, 0, 0, 0));
	fd = scepfsOpen(FILE_PATH, 0);
	TEST_ASSERT_TRUE(fd >= 0);
	return fd;
}

static int initArchive(int fd, long long *size)
{
	return _sce_dnas_ar_nokey_init(fd, 0, 0, 0, 0, 0, 0, 0, size);
}

static void expectMember(int type, const char *name, int size)
{
	TEST_ASSERT_EQUAL_INT(0, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	TEST_ASSERT_EQUAL_INT(type, s_fmt.type);
	TEST_ASSERT_EQUAL_INT(0, s_fmt.attr);
	TEST_ASSERT_EQUAL_STRING(name, s_name);
	TEST_ASSERT_EQUAL_INT(size, (int)s_fmt.size);
}

void setUp(void)
{
	fakeReset();
}

void tearDown(void)
{
	scepfsUmount("pfs0:");
	TEST_ASSERT_EQUAL_INT(0, fakeUnexpected);
}

static void test_plain_members_and_bodies(void)
{
	char body[512];
	long long size = 0;
	int fd = openArchive(arHeader(0x1234) + plainMembers() + string(0x10000, '\0'));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	TEST_ASSERT_EQUAL_INT(0x1234, (int)size);
	expectMember(2, "../dnas.dat", 0);
	TEST_ASSERT_EQUAL_INT(0, _sce_dnas_ar_nokey_read_body(body, 0, 0));
	expectMember(2, "", 276);
	TEST_ASSERT_EQUAL_INT(276, _sce_dnas_ar_nokey_read_body(body, 276, 0));
	TEST_ASSERT_EQUAL_INT('d', body[0]);
	TEST_ASSERT_EQUAL_INT('d', body[275]);
	expectMember(1, "res", 0);
	expectMember(2, "res/info.sys", 13);
	memset(body, 0, sizeof(body));
	TEST_ASSERT_EQUAL_INT(13, _sce_dnas_ar_nokey_read_body(body, 13, 0));
	TEST_ASSERT_EQUAL_STRING("title = Test\n", body);
	TEST_ASSERT_EQUAL_INT(-10000, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

static void test_plain_skip_body(void)
{
	long long size;
	int fd = openArchive(arHeader(0) + member(2, 0, "big", string(3000, 'x')) + member(1, 0, "after", ""));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	expectMember(2, "big", 3000);
	TEST_ASSERT_EQUAL_INT(0, _sce_dnas_ar_nokey_skip_body(3000));
	expectMember(1, "after", 0);
	TEST_ASSERT_EQUAL_INT(-10000, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

/* Zero padding after the last member ends the archive, whether or not it
   fills a whole member header. */
static void test_plain_zero_padding_ends(void)
{
	static const int pads[] = { 10, 24, 100 };
	long long size;

	for (unsigned i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
		int fd = openArchive(arHeader(0) + member(1, 0, "res", "") + string(pads[i], '\0'));

		TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
		expectMember(1, "res", 0);
		TEST_ASSERT_EQUAL_INT(-10000, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
		scepfsClose(fd);
		fakeReset();
	}
}

static void test_plain_truncated_header_fails(void)
{
	long long size;
	int fd = openArchive(arHeader(0) + member(1, 0, "res", "") + le32(2) + le32(0));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	expectMember(1, "res", 0);
	TEST_ASSERT_EQUAL_INT(-2, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

static void test_plain_truncated_name_fails(void)
{
	long long size;
	string m = member(2, 0, "res/info.sys", "");
	int fd = openArchive(arHeader(0) + m.substr(0, m.length() - 4));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	TEST_ASSERT_EQUAL_INT(-2, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

/* A personalized member needs the DNAS2 session, which a plaintext
   archive does not have. */
static void test_plain_personalized_member_fails(void)
{
	long long size;
	int fd = openArchive(arHeader(0) + member(1, 0, "res", "") + member(2, 1, "BOOT.ELF", "x"));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	expectMember(1, "res", 0);
	TEST_ASSERT_EQUAL_INT(-2, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

/* The callers hold a name in 1024 bytes, its terminator included. */
static void test_plain_name_length_limit(void)
{
	long long size;
	string longest(1023, 'n');
	int fd =
	    openArchive(arHeader(0) + member(2, 0, longest.c_str(), "") + member(2, 0, (longest + "n").c_str(), ""));

	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	expectMember(2, longest.c_str(), 0);
	TEST_ASSERT_EQUAL_INT(-2, _sce_dnas_ar_nokey_read_format(s_name, &s_fmt));
	scepfsClose(fd);
}

/* A file that ends inside the profile page is refused, not opened. */
static void test_short_file_fails(void)
{
	long long size;
	int fd = openArchive(arHeader(0) + "ab");

	TEST_ASSERT_EQUAL_INT(-1, initArchive(fd, &size));
	scepfsClose(fd);
}

static void test_read_error_fails(void)
{
	long long size;
	int fd;

	fakeAddPartition(PART, 128).failFile = "pkg/archive";
	fakeFind(PART)->files["pkg/archive"] = arHeader(0) + plainMembers();
	TEST_ASSERT_EQUAL_INT(0, scepfsMount("pfs0:", "hdd0:" PART, 0, 0, 0));
	fd = scepfsOpen(FILE_PATH, 0);
	TEST_ASSERT_TRUE(fd >= 0);
	TEST_ASSERT_EQUAL_INT(-2, initArchive(fd, &size));
	scepfsClose(fd);
}

/* Opens `stream` after a plaintext archive was open, and expects the DNAS2
   reader to take it and refuse it. */
static void expectDnasRefuses(const string &stream)
{
	long long size;
	int fd;

	fakeReset();
	fd = openArchive(arHeader(0) + plainMembers());
	TEST_ASSERT_EQUAL_INT(0, initArchive(fd, &size));
	scepfsClose(fd);
	fakeReset();
	fd = openArchive(arHeader(0) + stream);
	TEST_ASSERT_EQUAL_INT(-1, initArchive(fd, &size));
	scepfsClose(fd);
}

/* The streams below go to the DNAS2 reader, which rejects them (their 32 KB
   profile page is not valid), instead of being read as plaintext. */
static void test_encrypted_stream_uses_dnas(void)
{
	expectDnasRefuses(le32(0x20005) + string(0x8000, '\0'));
}

static void test_unknown_first_member_uses_dnas(void)
{
	expectDnasRefuses(member(7, 0, "res", "") + string(0x8000, '\0'));
	expectDnasRefuses(member(2, 1, "BOOT.ELF", "") + string(0x8000, '\0'));
	expectDnasRefuses(member(2, 0, "", "") + string(0x8000, '\0'));
	expectDnasRefuses(string(0x8000, '\0'));
}

extern "C" int __wrap_main(int, char **)
{
	startTests();
	UNITY_BEGIN();
	RUN_TEST(test_plain_members_and_bodies);
	RUN_TEST(test_plain_skip_body);
	RUN_TEST(test_plain_zero_padding_ends);
	RUN_TEST(test_plain_truncated_header_fails);
	RUN_TEST(test_plain_truncated_name_fails);
	RUN_TEST(test_plain_personalized_member_fails);
	RUN_TEST(test_plain_name_length_limit);
	RUN_TEST(test_short_file_fails);
	RUN_TEST(test_read_error_fails);
	RUN_TEST(test_encrypted_stream_uses_dnas);
	RUN_TEST(test_unknown_first_member_uses_dnas);
	return UNITY_END();
}
