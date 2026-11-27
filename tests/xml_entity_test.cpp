/*
 * Tests of the entities of the confirm and back buttons (&btn_enter;,
 * &btn_back;, &btn_enter_name;, src/xml/xml_entitydeclmanager.cpp) in the pages
 * as the loader of the pages parses them (XmlParse::ParseString).
 */

#include <string.h>
#include <string>
#include "unity.h"
#include "app/systemoptions.h"
#include "xml/xmlparse_ns.h"
#include "xml/xml_nodelist.h"
#include "xml/xml_nnmap.h"
#include "support/testutil.h"

#define CIRCLE "\xe2\x97\x8b" /* U+25CB, as the pages write it */
#define CROSS "\xc3\x97"      /* U+00D7 */

static const char PAGE[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<PAGE>\n"
                           "\t<TEXT id=\"finish\" value=' Press the &btn_enter; button to finish. ' />\n"
                           "\t<LINE>Press the &btn_enter; button. Press the &btn_back; button to return.</LINE>\n"
                           "\t<LINE>@@IMG src=\"../images/SW_KBD_B_&btn_enter_name;.png\"</LINE>\n"
                           "\t<LINE>&btn_enter;ボタンを押します。</LINE>\n"
                           "\t<LINE>Save &amp; exit</LINE>\n"
                           "</PAGE>\n";

static Document s_doc;

static string bytes(XMLString s)
{
	return string((const char *)s.GetBytes(), s.ByteLength());
}

/* Parses PAGE with the confirm button `cross` (0 circle). */
static void parsePage(int cross)
{
	SysOption.setPadEnter(cross);
	s_doc = XmlParse::ParseString(PAGE);
	TEST_ASSERT_TRUE_MESSAGE(s_doc != 0, "the page does not parse");
}

static string textValue(void)
{
	Node text = s_doc.getElementsByTagName(XMLString("TEXT", false)).item(0);
	Node value = text.get_attributes().getNamedItem(XMLString("value", false));

	TEST_ASSERT_TRUE(value != 0);
	return bytes(value.get_nodeValue());
}

/* The text of the index-th LINE: one node, as the scripts read it. */
static string line(int index)
{
	Node l = s_doc.getElementsByTagName(XMLString("LINE", false)).item(index);
	Node text = l.get_firstChild();

	TEST_ASSERT_TRUE(text != 0);
	TEST_ASSERT_TRUE_MESSAGE(text.get_nextSibling() == 0, "the text is split");
	return bytes(text.get_nodeValue());
}

void setUp(void)
{}

void tearDown(void)
{
	s_doc = Document(0);
	SysOption.setPadEnter(0);
}

static void test_circle_confirms(void)
{
	parsePage(0);
	TEST_ASSERT_EQUAL_STRING(" Press the " CIRCLE " button to finish. ", textValue().c_str());
	TEST_ASSERT_EQUAL_STRING("Press the " CIRCLE " button. Press the " CROSS " button to return.", line(0).c_str());
	TEST_ASSERT_EQUAL_STRING("@@IMG src=\"../images/SW_KBD_B_circle.png\"", line(1).c_str());
	TEST_ASSERT_EQUAL_STRING(CIRCLE "ボタンを押します。", line(2).c_str());
}

static void test_cross_confirms(void)
{
	parsePage(1);
	TEST_ASSERT_EQUAL_STRING(" Press the " CROSS " button to finish. ", textValue().c_str());
	TEST_ASSERT_EQUAL_STRING("Press the " CROSS " button. Press the " CIRCLE " button to return.", line(0).c_str());
	TEST_ASSERT_EQUAL_STRING("@@IMG src=\"../images/SW_KBD_B_cross.png\"", line(1).c_str());
	TEST_ASSERT_EQUAL_STRING(CROSS "ボタンを押します。", line(2).c_str());
}

/* Each page takes the setting of the moment it is parsed. */
static void test_page_parsed_after_a_change(void)
{
	parsePage(1);
	parsePage(0);
	TEST_ASSERT_EQUAL_STRING("@@IMG src=\"../images/SW_KBD_B_circle.png\"", line(1).c_str());
}

static void test_predefined_entities_unchanged(void)
{
	parsePage(1);
	TEST_ASSERT_EQUAL_STRING("Save & exit", line(3).c_str());
}

/* crt1 calls main, which is bn's: the link wraps it (ld --wrap main). */
extern "C" int __wrap_main(int, char **)
{
	startTests();
	XmlParse::Init();
	UNITY_BEGIN();
	RUN_TEST(test_circle_confirms);
	RUN_TEST(test_cross_confirms);
	RUN_TEST(test_page_parsed_after_a_change);
	RUN_TEST(test_predefined_entities_unchanged);
	return UNITY_END();
}
