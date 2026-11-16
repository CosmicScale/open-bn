/*
 * Chloe library: ChloeCrisp.
 */

#include "chloe.h"

/* Records the size of the ChloeKnife stage, then installs the device B table. */
int ChloeCrisp(ChloeObj *p)
{
	p->blobDistance = ChloeKnifeSize;
	return ChloeYogurt(p);
}
