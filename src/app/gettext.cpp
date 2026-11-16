/*
 * Labels of the soft-key buttons.
 */

#include "gettext.h"

const char *GetText(int n)
{
	/* "Delete", "Back", "Enter" in English as in the PSBBN Definitive Patch,
	   padded to 6 bytes as it did; the fourth is "Convert" in Japanese. */
	static const char *const s_text[4] = { "Delete", "Back  ", "Enter ", "変換" };
	if ((unsigned)n >= 4)
		return "?????";
	return s_text[n];
}
