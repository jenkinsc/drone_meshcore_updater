/* The table-free base-point multiply must equal the table version (ge.c built
 * WITHOUT MC_ED25519_SMALL) on every scalar the firmware can present. */
#include <stdio.h>
#include <string.h>
#include "../src/mc/ed25519/ge.h"

void ge_scalarmult_base_small(ge_p3 *h, const unsigned char *a);
void sc_reduce_small(unsigned char *s);
void sc_muladd_small(unsigned char *s, const unsigned char *a, const unsigned char *b, const unsigned char *c);
void sc_reduce(unsigned char *s);
void sc_muladd(unsigned char *s, const unsigned char *a, const unsigned char *b, const unsigned char *c);

static unsigned rs = 99;
static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

int main(void)
{
	int fail = 0, n = 0;
	for (int i = 0; i < 400; i++) {
		unsigned char a[32], p1[32], p2[32];
		ge_p3 A, B;
		for (int j = 0; j < 32; j++) a[j] = (unsigned char)rnd();
		switch (i) {                         /* edge scalars first */
		case 0: memset(a, 0, 32); break;
		case 1: memset(a, 0, 32); a[0] = 1; break;
		case 2: memset(a, 0xFF, 32); a[31] = 127; break;
		case 3: memset(a, 0, 32); a[31] = 64; break;
		default: a[31] &= 127;
		}
		ge_scalarmult_base(&A, a);
		ge_scalarmult_base_small(&B, a);
		ge_p3_tobytes(p1, &A);
		ge_p3_tobytes(p2, &B);
		n++;
		if (memcmp(p1, p2, 32)) { fail++; printf("  FAIL scalar #%d\n", i); }
	}
	/* scalar arithmetic: compact loops vs the original unrolled sc.c */
	static const unsigned char Lv[32] = {
	    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
	    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10 };
	int sn = 0, sfail = 0;
	for (int i = 0; i < 3000; i++) {
		unsigned char x[64], y[64];
		for (int j = 0; j < 64; j++) x[j] = (unsigned char)rnd();
		switch (i) {
		case 0: memset(x, 0, 64); break;
		case 1: memset(x, 0xFF, 64); break;
		case 2: memset(x, 0, 64); memcpy(x, Lv, 32); break;               /* = l */
		case 3: memset(x, 0, 64); memcpy(x, Lv, 32); x[0]--; break;      /* l - 1 */
		case 4: memset(x, 0, 64); memcpy(x, Lv, 32); x[0]++; break;      /* l + 1 */
		case 5: memset(x, 0, 64); x[31] = 0x10; break;                    /* 2^252 */
		case 6: memset(x, 0, 64); x[63] = 0x80; break;                    /* 2^511 */
		default: if (i % 3 == 0) memset(x + 32, 0, 32); /* also 256-bit inputs */
		}
		memcpy(y, x, 64);
		sc_reduce(x);
		sc_reduce_small(y);
		sn++;
		if (memcmp(x, y, 32)) { sfail++; printf("  FAIL sc_reduce #%d\n", i); }
	}
	for (int i = 0; i < 3000; i++) {
		unsigned char a[32], b[32], c[32], r1[32], r2[32];
		for (int j = 0; j < 32; j++) { a[j] = (unsigned char)rnd(); b[j] = (unsigned char)rnd(); c[j] = (unsigned char)rnd(); }
		if (i % 5 == 0) { memset(a, 0xFF, 32); a[31] &= 0x1F; }            /* sc_muladd takes reduced-ish inputs */
		a[31] &= 0x1F; b[31] &= (i % 2) ? 0x7F : 0x1F; c[31] &= 0x1F;   /* b may be a clamped key */
		sc_muladd(r1, a, b, c);
		sc_muladd_small(r2, a, b, c);
		sn++;
		if (memcmp(r1, r2, 32)) { sfail++; printf("  FAIL sc_muladd #%d\n", i); }
	}
	printf("sc compact vs original: %d cases, %d failed\n", sn, sfail);
	fail += sfail;
	printf("ed25519 table vs table-free: %d scalars, %d failed\n", n, fail);
	return fail ? 1 : 0;
}
