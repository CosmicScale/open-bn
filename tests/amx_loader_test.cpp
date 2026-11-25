/*
 * The modules of the pages (thXmlLoader, src/script/thxmlloader.cpp): a module
 * a page names as ".amx" is a plain AMX program, loaded as it is only from
 * /opt0/bn/script/ and only for a local page; any other module is decrypted as
 * before.
 *
 * The tests need the encrypted module parts/focus.bnx of the Definitive Patch:
 * the loader decrypts it, and the program it gives is the plain module of the
 * other tests. They write their pages in the current directory and their
 * modules in /opt0/bn/script/ under names starting with ".amx_loader_test",
 * removed at the end.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "unity.h"
#include "script/thxmlloader.h"
#include "xml/xmlparse_ns.h"
#include "support/testutil.h"

#define SCRIPT_DIR "/opt0/bn/script/"
#define PLAIN SCRIPT_DIR ".amx_loader_test.amx"
#define PLAIN_BNX SCRIPT_DIR ".amx_loader_test.bnx"
#define BAD SCRIPT_DIR ".amx_loader_test_bad.amx"
#define LINK SCRIPT_DIR ".amx_loader_test_link.amx"
#define PAGE "amx_loader_test.xml"
#define OUTSIDE "amx_loader_test_outside.amx"

/* Results of thXmlLoader: the page and its modules loaded, a module refused
   as a plain program, a module that does not decrypt, a missing module. */
#define LOADED 200
#define REFUSED (int)0xfffea067
#define NOT_DECRYPTED (int)0xfffea06d
#define MISSING 404

static string s_program; /* parts/focus, decrypted */

static string cwd()
{
	char buf[512];

	return getcwd(buf, sizeof(buf)) != 0 ? buf : "";
}

static void writeFile(const char *path, const string &data)
{
	FILE *fp = fopen(path, "wb");

	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT(data.size(), fwrite(data.data(), 1, data.size(), fp));
	fclose(fp);
}

/* The size an AMX header gives, -1 for a buffer that does not start with one. */
static int amxSize(const unsigned char *p, int len)
{
	if (len < 8 || p[4] != 0xe0 || p[5] != 0xf1)
		return -1;
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

/*
 * Loads a page of the current directory whose only module is src, as bn
 * loads a page; returns the result of the loader, and the program of the
 * module in image when it is loaded.
 */
static int loadPage(const char *src, string *image = 0)
{
	FILE *fp = fopen(PAGE, "wb");

	TEST_ASSERT_NOT_NULL(fp);
	fprintf(fp,
	    "<?xml version=\"1.0\"?>\n"
	    "<RESOURCE version=\"1.1\" xmlns=\"http://www.scei.co.jp/bn/2002/\" screen_w=\"640\" "
	    "screen_h=\"480\" >\n"
	    "\t<SCRIPT id=\"main\" src=\"%s\" type=\"binary/small\"/>\n"
	    "</RESOURCE>\n",
	    src);
	fclose(fp);

	thXmlLoader *loader =
	    new thXmlLoader("file:" + cwd() + "/" PAGE, "", "", SceneWindow::MODE_FG, THXML_NO_HISTORY, 0, 0, Buffer());
	loader->threadProc();
	int rc = loader->m_resultCode;
	if (image != 0 && rc == LOADED) {
		const unsigned char *p = (const unsigned char *)(void *)loader->m_imageMap["main"];
		int size = amxSize(p, 8);

		TEST_ASSERT_TRUE(size > 0);
		*image = string((const char *)p, size);
	}
	delete loader;
	return rc;
}

/* The tests after the first need the program it decrypts. */
static void needProgram(void)
{
	if (s_program.size() == 0)
		TEST_IGNORE_MESSAGE("parts/focus was not decrypted");
}

static int accepted(const char *pageUri, const string &img)
{
	return acceptPlainAmx(pageUri, PLAIN, img.data(), img.size());
}

static void removeFiles()
{
	unlink(PLAIN);
	unlink(PLAIN_BNX);
	unlink(BAD);
	unlink(LINK);
	unlink(PAGE);
	unlink(OUTSIDE);
}

void setUp(void)
{}

void tearDown(void)
{
	unlink(BAD);
	unlink(LINK);
	unlink(PLAIN_BNX);
	unlink(PAGE);
	unlink(OUTSIDE);
}

/* Runs first: the decrypted program is the plain module of the tests after it. */
static void test_encrypted_module_is_decrypted(void)
{
	TEST_ASSERT_EQUAL_INT(LOADED, loadPage("file:/opt0/bn/script/parts/focus", &s_program));
	TEST_ASSERT_EQUAL_INT(s_program.size(), amxSize((const unsigned char *)s_program.data(), s_program.size()));
	TEST_ASSERT_EQUAL_INT(6, s_program[6]);
	writeFile(PLAIN, s_program);
}

static void test_plain_module_is_loaded_as_it_is(void)
{
	string image;

	needProgram();
	TEST_ASSERT_EQUAL_INT(LOADED, loadPage("file:" PLAIN, &image));
	TEST_ASSERT_TRUE(image == s_program);
}

/* Sony's pages link each other as "file://opt0/...". */
static void test_plain_module_path_is_resolved(void)
{
	needProgram();
	TEST_ASSERT_EQUAL_INT(LOADED, loadPage("file://opt0/bn/script/.amx_loader_test.amx"));
	TEST_ASSERT_EQUAL_INT(LOADED, loadPage("file:/opt0/bn/script/parts/../.amx_loader_test.amx"));
}

static void test_plain_module_outside_the_scripts_is_refused(void)
{
	needProgram();
	writeFile(OUTSIDE, s_program);
	TEST_ASSERT_EQUAL_INT(REFUSED, loadPage(OUTSIDE));
	TEST_ASSERT_EQUAL_INT(REFUSED, loadPage(("file:/opt0/bn/script/../../.." + cwd() + "/" OUTSIDE).c_str()));
}

static void test_link_out_of_the_scripts_is_refused(void)
{
	needProgram();
	writeFile(OUTSIDE, s_program);
	TEST_ASSERT_EQUAL_INT(0, symlink((cwd() + "/" OUTSIDE).c_str(), LINK));
	TEST_ASSERT_EQUAL_INT(REFUSED, loadPage("file:" LINK));
}

/* The extension decides: a plain program named .bnx is decrypted, and fails. */
static void test_plain_program_named_bnx_is_decrypted(void)
{
	needProgram();
	writeFile(PLAIN_BNX, s_program);
	TEST_ASSERT_EQUAL_INT(NOT_DECRYPTED, loadPage("file:/opt0/bn/script/.amx_loader_test"));
	TEST_ASSERT_EQUAL_INT(NOT_DECRYPTED, loadPage("file:" PLAIN_BNX));
}

static void test_encrypted_module_named_amx_is_refused(void)
{
	string bnx = readFile(SCRIPT_DIR "parts/focus.bnx");

	TEST_ASSERT_TRUE(bnx.size() > 0);
	writeFile(BAD, bnx);
	TEST_ASSERT_FALSE(acceptPlainAmx("/bntmp/test/page.xml", BAD, bnx.data(), bnx.size()));
	TEST_ASSERT_EQUAL_INT(REFUSED, loadPage("file:" BAD));
}

static void test_missing_plain_module(void)
{
	TEST_ASSERT_EQUAL_INT(MISSING, loadPage("file:/opt0/bn/script/.amx_loader_test_missing.amx"));
}

/* Only a page that would decrypt its modules with the local key. */
static void test_page_origin(void)
{
	needProgram();
	TEST_ASSERT_TRUE(accepted("file:/opt0/bn/script/game/game.xml", s_program));
	TEST_ASSERT_TRUE(accepted("file://opt0/bn/script/music/music.xml", s_program));
	TEST_ASSERT_TRUE(accepted("/bntmp/test/page.xml", s_program));
	TEST_ASSERT_TRUE(accepted("file:/mnt/usb/page.xml", s_program));
	TEST_ASSERT_FALSE(accepted("file:/mnt/cdrom/page.xml", s_program));
	TEST_ASSERT_FALSE(accepted("/mnt/cdrom/page.xml", s_program));
	TEST_ASSERT_FALSE(accepted("http://example.com/page.xml", s_program));
	TEST_ASSERT_FALSE(accepted("https://example.com/page.xml", s_program));
}

/* Sets the 32-bit field at offset of an AMX header. */
static void setField(string &img, int offset, int value)
{
	for (int i = 0; i < 4; i++)
		img[offset + i] = (char)(value >> (8 * i));
}

/* A whole program: the signature, file version 6, the size of the file in its
   header, and blocks in order within its memory (cod, dat, hea and stp at 12,
   16, 20 and 24). */
static void test_header(void)
{
	string img;

	needProgram();
	img = s_program;
	img[4] = 0xe1;
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program;
	img[6] = 5;
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program + '\0';
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program.substr(0, s_program.size() - 1);
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program.substr(0, 40);
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program;
	setField(img, 24, s_program.size() - 1); /* stp: less memory than the file */
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program;
	setField(img, 20, 0x7fffffff); /* hea past stp */
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	img = s_program;
	setField(img, 12, 8); /* cod inside the header */
	TEST_ASSERT_FALSE(accepted("/bntmp/test/page.xml", img));
	TEST_ASSERT_TRUE(accepted("/bntmp/test/page.xml", s_program));
}

extern "C" int __wrap_main(int, char **)
{
	startTests();
	XmlParse::Init();
	removeFiles();
	UNITY_BEGIN();
	RUN_TEST(test_encrypted_module_is_decrypted);
	RUN_TEST(test_plain_module_is_loaded_as_it_is);
	RUN_TEST(test_plain_module_path_is_resolved);
	RUN_TEST(test_plain_module_outside_the_scripts_is_refused);
	RUN_TEST(test_link_out_of_the_scripts_is_refused);
	RUN_TEST(test_plain_program_named_bnx_is_decrypted);
	RUN_TEST(test_encrypted_module_named_amx_is_refused);
	RUN_TEST(test_missing_plain_module);
	RUN_TEST(test_page_origin);
	RUN_TEST(test_header);
	int failures = UNITY_END();
	removeFiles();
	return failures;
}
