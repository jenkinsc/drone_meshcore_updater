/*
 * Table-free a*B for the Ed25519 base point B, for builds that cannot spare
 * the 30 KB precomputed table in ge.c (see MC_ED25519_SMALL there).
 *
 * Plain double-and-add, one doubling and one mixed addition per bit, with the
 * addition result selected by a constant-time conditional move, so the control
 * flow does not depend on the scalar. About 255 * 26 field multiplications;
 * a few tens of milliseconds on a Cortex-M4. Checked against the table version
 * on random scalars and against MeshCore's own keys and signatures
 * (updater/mc-tests).
 *
 * The addition formulas are the complete ones for this curve, so adding the
 * identity or doubling a point needs no special case.
 *
 * Precondition, as for ge_scalarmult_base: a[31] <= 127.
 */
#include "ge.h"

/* The base point in precomputed form: Bi[0] in precomp_data.h. */
static const ge_precomp B = {
    { 25967493, -14356035, 29566456, 3660896, -12694345, 4014787, 27544626, -11754271, -6079156, 2047605 },
    { -12545711, 934262, -2722910, 3049990, -727428, 9406986, 12720692, 5043384, 19500929, -15469378 },
    { -8738181, 4489570, 9688441, -14785194, 10184609, -12363380, 29287919, 11864899, -24514362, -4438546 },
};

static void p3_cmov(ge_p3 *t, const ge_p3 *u, unsigned int b)
{
    fe_cmov(t->X, u->X, b);
    fe_cmov(t->Y, u->Y, b);
    fe_cmov(t->Z, u->Z, b);
    fe_cmov(t->T, u->T, b);
}

void ge_scalarmult_base_small(ge_p3 *h, const unsigned char *a)
{
    ge_p3 acc, sum;
    ge_p1p1 r;
    ge_cached c;
    int i;

    ge_p3_0(&acc);
    for (i = 255; i >= 0; --i) {
        unsigned int bit = (a[i >> 3] >> (i & 7)) & 1u;

        /* acc = 2*acc as acc + acc: the addition is complete, and it avoids
         * linking the 1 KB squaring-and-doubling routine used by ge_p3_dbl. */
        ge_p3_to_cached(&c, &acc);
        ge_add(&r, &acc, &c);
        ge_p1p1_to_p3(&acc, &r);
        ge_madd(&r, &acc, &B);
        ge_p1p1_to_p3(&sum, &r);
        p3_cmov(&acc, &sum, bit);
    }
    *h = acc;
}
