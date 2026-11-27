/*
 * Tests of the confirm button in the system configuration (src/app/syscnf.cpp):
 * the [pad] section of system.ini, on a __sysconf partition in memory, the
 * natives of the settings page and GetSystemParam(SYS_ENTER_BUTTON).
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include "unity.h"
#include "app/bnapi.h"
#include "app/syscnf.h"
#include "app/systemoptions.h"
#include "fs/bbmount.h"
#include "natives/smallext.h"
#include "support/fakepfs.h"
#include "support/testutil.h"

#define SYSTEM_INI "conf/system.ini"
#define SYS_ENTER_BUTTON 720

/* system.ini as bn writes it, up to the [pad] section, with values that are
   not the defaults. */
static const char INI_HEAD[] = "\n"
                               "\n"
                               "[keyboard]\n"
                               "\ttype = 1;\t# 0:106 1:101\n"
                               "\trepeatw = 2;\t# 0:short 1:middle 2:long\n"
                               "\trepeats = 0;\t# 0:fast 1:middle 2:slow\n"
                               "\n"
                               "[mouse]\n"
                               "\tspeed = 2;\t# 0:fast 1:middle 2:slow\n"
                               "\tdblclk = 0;\t# 0:fast 1:middle 2:slow\n"
                               "\tlr = 1;\t\t# 0:left hand 1:right hand\n"
                               "\n"
                               "[atok]\n"
                               "\tmode = 1;\t# 0:roma 1:kana\n"
                               "\tbind = 1;\t# 0:setting1 1:setting2\n"
                               "\n"
                               "[laststatus]\n"
                               "\tsoftkb_onoff = 1;\n"
                               "\tsoftkb_qwert = 0;\n"
                               "\n";

/* The whole system.ini as bn writes it, with `enter` in [pad]. */
static string systemIni(int enter)
{
	char pad[64];

	sprintf(pad, "[pad]\n\tenter = %d;\t# 0:circle 1:cross\n\n\n", enter);
	return string(INI_HEAD) + pad;
}

static void putIni(const string &text)
{
	fakeFind("__sysconf")->files[SYSTEM_INI] = text;
}

static string getIni(void)
{
	return fakeFind("__sysconf")->files[SYSTEM_INI];
}

/* Reads the device and system.ini as bn does at startup. */
static void readConfig(void)
{
	TEST_ASSERT_TRUE_MESSAGE(sysconf_read(0, 0) >= 0, "sysconf_read failed");
}

/* Calls the native `name` of `table` with one argument. */
static cell callNative(AMX_NATIVE_INFO *table, const char *name, cell arg)
{
	cell params[2];

	params[0] = sizeof(cell);
	params[1] = arg;
	for (; table->name != 0; table++)
		if (strcmp(table->name, name) == 0)
			return table->func(0, params);
	TEST_FAIL_MESSAGE("no such native");
	return 0;
}

static int getEnter(void)
{
	return callNative(smallext_syscnf, "scnfGetEnterBtn", 0);
}

static int setEnter(int cross)
{
	return callNative(smallext_syscnf, "scnfSetEnterBtn", cross);
}

void setUp(void)
{
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, BBpfsAllUnMounter(), "a mount is still in use");
	fakeReset();
	fakeAddPartition("__sysconf", 0x40000).writable = 1;
}

void tearDown(void)
{
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, fakeUnexpected, "unexpected PFS call");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, fakeOpenFds(), "a file is still open");
}

/* The first test: the configuration as bn starts. A system.ini without
   [pad], as the systems before the setting wrote it, leaves circle. */
static void test_circle_without_pad_section(void)
{
	TEST_ASSERT_EQUAL_INT(0, getEnter());
	putIni(INI_HEAD);
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, getEnter());
}

static void test_cross_read_from_system_ini(void)
{
	putIni(systemIni(1));
	readConfig();
	TEST_ASSERT_EQUAL_INT(1, getEnter());
	syscnf_setSysOpt(true);
	TEST_ASSERT_EQUAL_INT(1, SysOption.getPadEnter());
	TEST_ASSERT_EQUAL_INT(1, BNAPI::GetSystemParam(SYS_ENTER_BUTTON));
	putIni(systemIni(0));
	readConfig();
	syscnf_setSysOpt(true);
	TEST_ASSERT_EQUAL_INT(0, SysOption.getPadEnter());
	TEST_ASSERT_EQUAL_INT(0, BNAPI::GetSystemParam(SYS_ENTER_BUTTON));
}

static void test_other_values_are_ignored(void)
{
	TEST_ASSERT_EQUAL_INT(0, setEnter(0));
	putIni(systemIni(2));
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, getEnter());
	putIni(systemIni(-1));
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, getEnter());
}

/* enter counts only in [pad]. */
static void test_enter_outside_pad_is_ignored(void)
{
	TEST_ASSERT_EQUAL_INT(0, setEnter(0));
	putIni(string(INI_HEAD) + "[laststatus]\n\tenter = 1;\n");
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, getEnter());
}

static void test_cross_written_to_system_ini(void)
{
	putIni(systemIni(0));
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, setEnter(1));
	TEST_ASSERT_EQUAL_INT(0, systemini_write(0, 0));
	TEST_ASSERT_EQUAL_STRING(systemIni(1).c_str(), getIni().c_str());
	TEST_ASSERT_EQUAL_INT(0, setEnter(0));
	readConfig();
	TEST_ASSERT_EQUAL_INT(1, getEnter());
}

/* A system.ini without [pad] gets it, with circle, at the next write. */
static void test_pad_section_added_on_write(void)
{
	TEST_ASSERT_EQUAL_INT(0, setEnter(0));
	putIni(INI_HEAD);
	readConfig();
	TEST_ASSERT_EQUAL_INT(0, systemini_write(0, 0));
	TEST_ASSERT_EQUAL_STRING(systemIni(0).c_str(), getIni().c_str());
}

static void test_set_refuses_other_values(void)
{
	TEST_ASSERT_EQUAL_INT(0, setEnter(1));
	TEST_ASSERT_EQUAL_INT(-1, setEnter(2));
	TEST_ASSERT_EQUAL_INT(-1, setEnter(-1));
	TEST_ASSERT_EQUAL_INT(1, getEnter());
}

/* crt1 calls main, which is bn's: the link wraps it (ld --wrap main). */
extern "C" int __wrap_main(int, char **)
{
	startTests();
	UNITY_BEGIN();
	RUN_TEST(test_circle_without_pad_section);
	RUN_TEST(test_cross_read_from_system_ini);
	RUN_TEST(test_other_values_are_ignored);
	RUN_TEST(test_enter_outside_pad_is_ignored);
	RUN_TEST(test_cross_written_to_system_ini);
	RUN_TEST(test_pad_section_added_on_write);
	RUN_TEST(test_set_refuses_other_values);
	return UNITY_END();
}
