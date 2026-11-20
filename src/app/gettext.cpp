/*
 * Pad-help labels shown while a text field is being edited (TextIM), from the
 * TEXT elements of the system script fg.xml, so that a language pack
 * translates them without a different bn. Enter and Back show in every text
 * input; Delete and Convert only with the software keyboard on screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xml/xmlparse_ns.h"
#include "xml/xml_nodelist.h"
#include "xml/xml_nnmap.h"
#include "gettext.h"

#define FG_XML "/opt0/bn/script/system/fg.xml"
#define TEXT_COUNT 4

/* "Delete", "Back", "Enter", "Convert": the id of each TEXT element and the
   label used when fg.xml has no such element. Back and Enter are the labels
   fg.xml already has for the buttons of the message boxes. */
static const char *const s_id[TEXT_COUNT] = { "skb_btn_delete", "msgtop_btn_back", "msgtop_btn_enter",
	"skb_btn_convert" };
static const char *s_text[TEXT_COUNT] = { "Delete", "Back", "Enter", "Convert" };
static char *s_read[TEXT_COUNT]; /* the labels read from a file, to free */
static int s_loaded;

/* Longest label in bytes: the system script fg keeps each label in 8 cells
   (32 bytes with the NUL) and copies it there without a bound; a longer one
   runs into the next label, and fg then makes both again over and over. */
#define TEXT_MAX 31

/* A copy of value cut to TEXT_MAX bytes, never in the middle of a UTF-8
   character; 0 when out of memory. */
static char *copy_label(const char *value)
{
	size_t len = strlen(value);
	char *p;

	if (len > TEXT_MAX) {
		len = TEXT_MAX;
		while (len > 0 && ((unsigned char)value[len] & 0xc0) == 0x80)
			len--;
	}
	p = (char *)malloc(len + 1);
	if (p != 0) {
		memcpy(p, value, len);
		p[len] = 0;
	}
	return p;
}

/* The whole file with a NUL at the end, or 0. */
static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *p = 0;
	long size;

	if (fp == 0)
		return 0;
	if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) >= 0) {
		rewind(fp);
		p = (char *)malloc(size + 1);
		if (p != 0 && fread(p, 1, size, fp) != (size_t)size) {
			free(p);
			p = 0;
		}
		if (p != 0)
			p[size] = 0;
	}
	fclose(fp);
	return p;
}

/* Puts in value the attribute name of node; false when node has none. */
static bool attribute(Node node, const char *name, XMLString &value)
{
	Node a = node.get_attributes().getNamedItem(XMLString(name, false));

	if (a == 0 || a.get_nodeType() == 0)
		return false;
	value = a.get_nodeValue();
	return true;
}

void LoadTexts(const char *path)
{
	char *xml = read_file(path);

	s_loaded = 1;
	if (xml == 0)
		return;
	Document doc = XmlParse::ParseString(xml);
	free(xml);
	if (doc == 0)
		return;

	NodeList list = doc.getElementsByTagName(XMLString("TEXT", false));
	int found[TEXT_COUNT] = { 0 };
	unsigned long i;

	for (i = 0; i < list.get_length(); i++) {
		XMLString id, value;
		int n;

		if (!attribute(list.item(i), "id", id) || !attribute(list.item(i), "value", value))
			continue;
		for (n = 0; n < TEXT_COUNT; n++) {
			/* The first element with the id, as fg takes it. */
			if (!found[n] && strcmp((const char *)id.GetBufferPointer(), s_id[n]) == 0) {
				char *copy = copy_label((const char *)value.GetBufferPointer());

				found[n] = 1;
				if (copy != 0) {
					free(s_read[n]);
					s_read[n] = copy;
					s_text[n] = copy;
				}
			}
		}
	}
}

const char *GetText(int n)
{
	if (!s_loaded)
		LoadTexts(FG_XML);
	if ((unsigned)n >= TEXT_COUNT)
		return "?????";
	return s_text[n];
}
