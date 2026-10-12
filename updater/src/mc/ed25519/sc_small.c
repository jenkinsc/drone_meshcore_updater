/*
 * Compact scalar arithmetic mod l = 2^252 + 27742317777372353535851937790883648493
 * for builds where flash is scarce (MC_ED25519_SMALL). The original sc.c is
 * ~10 KB of unrolled limb code; this is a few hundred bytes of loops.
 *
 * sc_reduce: shift-and-subtract, 260 passes over 64 bytes, each pass a
 * constant-time conditional subtract, so nothing branches on the value.
 * sc_muladd: schoolbook 32x32 byte product plus c, then sc_reduce.
 * Slower than sc.c (a few milliseconds on a Cortex-M4) and checked against it on
 * random and edge inputs in updater/mc-tests/test_ed_cross.c.
 *
 * The *_small names always exist so the cross-check can run both
 * implementations in one binary; sc_reduce/sc_muladd are defined here only when
 * MC_ED25519_SMALL says sc.c is not being linked.
 */
#include <string.h>

#include "sc.h"

/* l, little endian. */
static const unsigned char L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

/* s[0..63] (little endian) -> s mod l in s[0..31]; s[32..63] = 0. */
void sc_reduce_small(unsigned char *s)
{
    unsigned char m[64], t[64];
    int i, k;

    /* m = l << 259: its top bit (bit 252 of l) lands on bit 511. */
    memset(m, 0, sizeof(m));
    for (i = 0; i < 32; i++) {
        m[32 + i] |= (unsigned char)(L[i] << 3);
        if (33 + i < 64) {
            m[33 + i] |= (unsigned char)(L[i] >> 5);
        }
    }

    for (k = 0; k <= 259; k++) {
        unsigned int borrow = 0, keep;

        for (i = 0; i < 64; i++) {
            unsigned int d = (unsigned int)s[i] - m[i] - borrow;

            t[i] = (unsigned char)d;
            borrow = (d >> 31) & 1u;
        }
        keep = borrow - 1u;                       /* all ones if s >= m */
        for (i = 0; i < 64; i++) {
            s[i] = (unsigned char)((t[i] & keep) | (s[i] & ~keep));
        }
        for (i = 0; i < 63; i++) {                /* m >>= 1 */
            m[i] = (unsigned char)((m[i] >> 1) | (m[i + 1] << 7));
        }
        m[63] >>= 1;
    }
}

/* s = (a*b + c) mod l, all 32-byte little-endian. s may alias a, b or c. */
void sc_muladd_small(unsigned char *s, const unsigned char *a, const unsigned char *b,
                     const unsigned char *c)
{
    unsigned char x[64];
    int i, j;

    memset(x, 0, sizeof(x));
    memcpy(x, c, 32);
    for (i = 0; i < 32; i++) {
        unsigned int carry = 0;

        for (j = 0; j < 32; j++) {
            unsigned int v = x[i + j] + (unsigned int)a[i] * b[j] + carry;

            x[i + j] = (unsigned char)v;
            carry = v >> 8;
        }
        x[i + 32] = (unsigned char)carry;         /* this byte has no earlier contribution */
    }
    sc_reduce_small(x);
    memcpy(s, x, 32);
}

#ifdef MC_ED25519_SMALL
void sc_reduce(unsigned char *s) { sc_reduce_small(s); }

void sc_muladd(unsigned char *s, const unsigned char *a, const unsigned char *b,
               const unsigned char *c)
{
    sc_muladd_small(s, a, b, c);
}
#endif
