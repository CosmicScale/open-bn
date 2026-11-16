/*
 * The two encrypted Chloe stages (see chloe.h).
 */

#include "chloe.h"

char ChloeDoushite[] __attribute__((aligned(16))) = {
#include "../assets/chloe/stage1.inc"
};
const int ChloeDoushiteSize = sizeof(ChloeDoushite);

char ChloeKnife[] __attribute__((aligned(16))) = {
#include "../assets/chloe/stage2.inc"
};
const int ChloeKnifeSize = sizeof(ChloeKnife);
