/*
 * DNAS protected-region boundaries. In the shipped program a protected
 * function body is stored scrambled and a protection shim descrambles it in
 * place while it runs; this build leaves the regions in plaintext, so the
 * boundaries are empty and the hidden strings are ordinary literals.
 */
#ifndef DNAS_PROT_H
#define DNAS_PROT_H

/* DNAS2 */
#define SCE_DNAS_PROT_BEGIN() ((void)0)
#define SCE_DNAS_PROT_END() ((void)0)

/* DNAS1 */
#define SCE_DNAS1_PROT_BEGIN() ((void)0)
#define SCE_DNAS1_PROT_END() ((void)0)

/* String literal stored inside a region (both families). */
#define SCE_DNAS_STR(s) ((char *)(s))

#endif
