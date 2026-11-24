/*
 * The pad-help labels of text input (GetText, src/app/gettext.cpp) read from
 * the TEXT elements of an fg.xml.
 */

#include <stdio.h>
#include <unistd.h>
#include "unity.h"
#include "app/gettext.h"
#include "xml/xmlparse_ns.h"
#include "support/testutil.h"

#define XML "gettext_test.xml"

#define HEAD                                                                                   \
	"<?xml version=\"1.0\"?>\n"                                                            \
	"<RESOURCE version=\"1.1\" xmlns=\"http://www.scei.co.jp/bn/2002/\" screen_w=\"640\" " \
	"screen_h=\"480\" >\n"                                                                 \
	"\t<SCRIPT id=\"main\" src=\"fg\" type=\"binary/small\"/>\n"
#define TAIL "</RESOURCE>\n"

static void writeXml(const char *body)
{
	FILE *fp = fopen(XML, "wb");

	TEST_ASSERT_NOT_NULL(fp);
	fputs(HEAD, fp);
	fputs(body, fp);
	fputs(TAIL, fp);
	fclose(fp);
}

void setUp(void)
{}

void tearDown(void)
{
	unlink(XML);
}

/* Runs first: the labels are kept from one test to the next. */
static void test_missing_file_keeps_built_in_labels(void)
{
	LoadTexts("gettext_test_missing.xml");
	TEST_ASSERT_EQUAL_STRING("Delete", GetText(0));
	TEST_ASSERT_EQUAL_STRING("Back", GetText(1));
	TEST_ASSERT_EQUAL_STRING("Enter", GetText(2));
	TEST_ASSERT_EQUAL_STRING("Convert", GetText(3));
}

static void test_labels_from_text_elements(void)
{
	writeXml("\t<TEXT id=\"skb_btn_delete\" value=\"削除\" />\n"
	         "\t<TEXT id=\"msgtop_btn_back\" value=\"戻る\" />\n"
	         "\t<TEXT id=\"fg:btn_l\" value=\"\" />\n"
	         "\t<TEXT id=\"msgtop_btn_enter\" value=\"決定\" />\n"
	         "\t<TEXT id=\"skb_btn_convert\" value=\"Convert\" />\n"
	         "\t<TEXT id=\"msgtop_btn_option\" value=\"オプション\" />\n");
	LoadTexts(XML);
	TEST_ASSERT_EQUAL_STRING("削除", GetText(0));
	TEST_ASSERT_EQUAL_STRING("戻る", GetText(1));
	TEST_ASSERT_EQUAL_STRING("決定", GetText(2));
	TEST_ASSERT_EQUAL_STRING("Convert", GetText(3));
}

static void test_missing_element_keeps_label(void)
{
	writeXml("\t<TEXT id=\"msgtop_btn_enter\" value=\"Enter\" />\n");
	LoadTexts(XML);
	TEST_ASSERT_EQUAL_STRING("削除", GetText(0));
	TEST_ASSERT_EQUAL_STRING("戻る", GetText(1));
	TEST_ASSERT_EQUAL_STRING("Enter", GetText(2));
	TEST_ASSERT_EQUAL_STRING("Convert", GetText(3));
}

static void test_broken_file_keeps_labels(void)
{
	writeXml("\t<TEXT id=\"msgtop_btn_back\" value=\"Back\"\n");
	LoadTexts(XML);
	TEST_ASSERT_EQUAL_STRING("戻る", GetText(1));
}

/* At most 31 bytes, and a character cut by the 31st byte is dropped whole. */
static void test_long_labels_cut(void)
{
	writeXml("\t<TEXT id=\"skb_btn_delete\" value=\"0123456789012345678901234567890123456789\" />\n"
	         "\t<TEXT id=\"msgtop_btn_back\" value=\"0123456789012345678901234567890\" />\n"
	         "\t<TEXT id=\"msgtop_btn_enter\" value=\"АБВГДЕЖЗИЙКЛМНОП\" />\n"
	         "\t<TEXT id=\"skb_btn_convert\" value=\"一二三四五六七八九十百\" />\n");
	LoadTexts(XML);
	TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890", GetText(0));
	TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890", GetText(1));
	TEST_ASSERT_EQUAL_STRING("АБВГДЕЖЗИЙКЛМНО", GetText(2));
	TEST_ASSERT_EQUAL_STRING("一二三四五六七八九十", GetText(3));
}

/* The first element with an id counts, as for the system script fg; an
   element without a value is skipped; an empty value is an empty label. */
static void test_first_element_and_empty_value(void)
{
	writeXml("\t<TEXT id=\"skb_btn_delete\" />\n"
	         "\t<TEXT id=\"skb_btn_delete\" value=\"Supprimer\" />\n"
	         "\t<TEXT id=\"skb_btn_delete\" value=\"Effacer\" />\n"
	         "\t<TEXT id=\"skb_btn_convert\" value=\"\" />\n");
	LoadTexts(XML);
	TEST_ASSERT_EQUAL_STRING("Supprimer", GetText(0));
	TEST_ASSERT_EQUAL_STRING("", GetText(3));
}

static void test_out_of_range(void)
{
	TEST_ASSERT_EQUAL_STRING("?????", GetText(4));
	TEST_ASSERT_EQUAL_STRING("?????", GetText(-1));
}

extern "C" int __wrap_main(int, char **)
{
	startTests();
	XmlParse::Init();
	UNITY_BEGIN();
	RUN_TEST(test_missing_file_keeps_built_in_labels);
	RUN_TEST(test_labels_from_text_elements);
	RUN_TEST(test_missing_element_keeps_label);
	RUN_TEST(test_broken_file_keeps_labels);
	RUN_TEST(test_long_labels_cut);
	RUN_TEST(test_first_element_and_empty_value);
	RUN_TEST(test_out_of_range);
	return UNITY_END();
}
