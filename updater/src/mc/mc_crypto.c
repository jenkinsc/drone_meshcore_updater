#include "mc_crypto.h"

#include <string.h>

/* ================================ SHA-256 ================================== */

struct sha256 {
	uint32_t h[8];
	uint8_t  buf[64];
	uint32_t blen;
	uint64_t total;
};

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
	static const uint32_t H0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(s->h, H0, sizeof(H0));
	s->blen = 0;
	s->total = 0;
}

static void sha_update(struct sha256 *s, const uint8_t *d, size_t n)
{
	s->total += n;
	while (n) {
		size_t take = 64 - s->blen;
		if (take > n) {
			take = n;
		}
		memcpy(s->buf + s->blen, d, take);
		s->blen += (uint32_t)take;
		d += take;
		n -= take;
		if (s->blen == 64) {
			sha_block(s, s->buf);
			s->blen = 0;
		}
	}
}

static void sha_final(struct sha256 *s, uint8_t out[32])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80;

	sha_update(s, &pad, 1);
	pad = 0;
	while (s->blen != 56) {
		sha_update(s, &pad, 1);
	}
	uint8_t len[8];
	for (int i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha_update(s, len, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i]     = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

void mc_sha256(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
	       uint8_t out[32])
{
	struct sha256 s;

	sha_init(&s);
	sha_update(&s, a, alen);
	if (blen) {
		sha_update(&s, b, blen);
	}
	sha_final(&s, out);
}

void mc_hmac_sha256(const uint8_t key[32], const uint8_t *msg, size_t len,
		    uint8_t out[32])
{
	uint8_t pad[64], inner[32];
	struct sha256 s;

	/* Key is exactly 32 bytes (< block size), so it is used zero-padded. */
	memset(pad, 0x36, sizeof(pad));
	for (int i = 0; i < 32; i++) {
		pad[i] ^= key[i];
	}
	sha_init(&s);
	sha_update(&s, pad, 64);
	sha_update(&s, msg, len);
	sha_final(&s, inner);

	memset(pad, 0x5c, sizeof(pad));
	for (int i = 0; i < 32; i++) {
		pad[i] ^= key[i];
	}
	sha_init(&s);
	sha_update(&s, pad, 64);
	sha_update(&s, inner, 32);
	sha_final(&s, out);
}

/* ================================= AES-128 ================================= */

/* S-boxes are computed once rather than typed in: 512 hand-copied bytes is where
 * a typo would hide, and the field arithmetic below is short. (Algorithm from
 * the standard's construction: multiplicative inverse in GF(2^8), then the
 * affine transform.) */
static uint8_t SBOX[256], INV_SBOX[256];
static int tables_ready;

static uint8_t rotl8(uint8_t x, int n)
{
	return (uint8_t)((x << n) | (x >> (8 - n)));
}

static void build_tables(void)
{
	uint8_t p = 1, q = 1;

	do {
		/* p *= 3 */
		p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));
		/* q /= 3 */
		q ^= (uint8_t)(q << 1);
		q ^= (uint8_t)(q << 2);
		q ^= (uint8_t)(q << 4);
		if (q & 0x80) {
			q ^= 0x09;
		}
		uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
		SBOX[p] = (uint8_t)(x ^ 0x63);
	} while (p != 1);
	SBOX[0] = 0x63;

	for (int i = 0; i < 256; i++) {
		INV_SBOX[SBOX[i]] = (uint8_t)i;
	}
	tables_ready = 1;
}

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;

	while (b) {
		if (b & 1) {
			r ^= a;
		}
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static void expand_key(const uint8_t key[16], uint8_t rk[176])
{
	uint8_t rcon = 1;

	memcpy(rk, key, 16);
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4];
		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t t0 = t[0];
			t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
			t[1] = SBOX[t[2]];
			t[2] = SBOX[t[3]];
			t[3] = SBOX[t0];
			rcon = xtime(rcon);
		}
		for (int j = 0; j < 4; j++) {
			rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
		}
	}
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
	for (int i = 0; i < 16; i++) {
		s[i] ^= rk[i];
	}
}

/* State is column-major as in the standard: s[4*c + r]. */
static void sub_shift(uint8_t s[16])
{
	uint8_t t[16];

	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			t[4 * c + r] = SBOX[s[4 * ((c + r) % 4) + r]];
		}
	}
	memcpy(s, t, 16);
}

static void inv_shift_sub(uint8_t s[16])
{
	uint8_t t[16];

	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			t[4 * ((c + r) % 4) + r] = INV_SBOX[s[4 * c + r]];
		}
	}
	memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16])
{
	for (int c = 0; c < 4; c++) {
		uint8_t *x = &s[4 * c];
		uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
		x[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
		x[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
		x[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
		x[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
	}
}

static void inv_mix_columns(uint8_t s[16])
{
	for (int c = 0; c < 4; c++) {
		uint8_t *x = &s[4 * c];
		uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
		x[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
		x[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
		x[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
		x[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
	}
}

static void encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];

	memcpy(s, in, 16);
	add_round_key(s, rk);
	for (int round = 1; round < 10; round++) {
		sub_shift(s);
		mix_columns(s);
		add_round_key(s, rk + 16 * round);
	}
	sub_shift(s);
	add_round_key(s, rk + 160);
	memcpy(out, s, 16);
}

static void decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];

	memcpy(s, in, 16);
	add_round_key(s, rk + 160);
	for (int round = 9; round >= 1; round--) {
		inv_shift_sub(s);
		add_round_key(s, rk + 16 * round);
		inv_mix_columns(s);
	}
	inv_shift_sub(s);
	add_round_key(s, rk);
	memcpy(out, s, 16);
}

void mc_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out,
			   size_t len)
{
	uint8_t rk[176];

	if (!tables_ready) {
		build_tables();
	}
	expand_key(key, rk);
	for (size_t i = 0; i + 16 <= len; i += 16) {
		encrypt_block(rk, in + i, out + i);
	}
	memset(rk, 0, sizeof(rk));
}

void mc_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out,
			   size_t len)
{
	uint8_t rk[176];

	if (!tables_ready) {
		build_tables();
	}
	expand_key(key, rk);
	for (size_t i = 0; i + 16 <= len; i += 16) {
		decrypt_block(rk, in + i, out + i);
	}
	memset(rk, 0, sizeof(rk));
}

const struct mc_crypto MC_CRYPTO_BUILTIN = {
	mc_sha256, mc_hmac_sha256, mc_aes128_ecb_encrypt, mc_aes128_ecb_decrypt,
};
