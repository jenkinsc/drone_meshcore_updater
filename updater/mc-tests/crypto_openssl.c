/* mc_crypto backed by OpenSSL, for the host tests only. The device uses
 * tinycrypt (mc_crypto_tc.c). */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>

#include "../src/mc/mc_wire.h"

static void sha256(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t out[32])
{
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	unsigned n = 32;
	EVP_DigestInit_ex(c, EVP_sha256(), NULL);
	EVP_DigestUpdate(c, a, al);
	if (bl) EVP_DigestUpdate(c, b, bl);
	EVP_DigestFinal_ex(c, out, &n);
	EVP_MD_CTX_free(c);
}

static void hmac(const uint8_t key[32], const uint8_t *m, size_t l, uint8_t out[32])
{
	unsigned n = 32;
	HMAC(EVP_sha256(), key, 32, m, l, out, &n);
}

static void aes(int enc, const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t len)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int n = 0, f = 0;
	EVP_CipherInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL, enc);
	EVP_CIPHER_CTX_set_padding(c, 0);
	EVP_CipherUpdate(c, out, &n, in, (int)len);
	EVP_CipherFinal_ex(c, out + n, &f);
	EVP_CIPHER_CTX_free(c);
}
static void aes_e(const uint8_t k[16], const uint8_t *i, uint8_t *o, size_t l) { aes(1, k, i, o, l); }
static void aes_d(const uint8_t k[16], const uint8_t *i, uint8_t *o, size_t l) { aes(0, k, i, o, l); }

const struct mc_crypto TEST_CRYPTO = { sha256, hmac, aes_e, aes_d };
