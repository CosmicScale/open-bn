/*
 * Pad-help labels of text input (gettext.cpp).
 */

#ifndef GETTEXT_H
#define GETTEXT_H

/* Pad-help label n of text input: 0 "Delete", 1 "Back", 2 "Enter", 3 "Convert".
   The first call reads them from /opt0/bn/script/system/fg.xml. */
const char *GetText(int n);

/* Reads the labels from the TEXT elements of the XML file at path, each cut
   to 31 bytes on a UTF-8 character boundary; a label whose element is
   missing keeps the one it had. */
void LoadTexts(const char *path);

#endif
