#include "mc_id.h"

#include <string.h>

#include "ed25519/ed_25519.h"

static bool usable_pub(const uint8_t pub[MC_PUB_KEY_SIZE])
{
	return pub[0] != 0x00 && pub[0] != 0xFF;
}

bool mc_id_from_seed(struct mc_id *id, const uint8_t seed[32])
{
	memset(id, 0, sizeof(*id));
	ed25519_create_keypair(id->pub, id->prv, seed);
	return usable_pub(id->pub);
}

bool mc_id_from_private(struct mc_id *id, const uint8_t prv[MC_PRV_KEY_SIZE])
{
	memset(id, 0, sizeof(*id));
	memcpy(id->prv, prv, MC_PRV_KEY_SIZE);
	ed25519_derive_pub(id->pub, id->prv);
	return usable_pub(id->pub);
}

void mc_id_set_operator(struct mc_id *id, const uint8_t op_pub[MC_PUB_KEY_SIZE])
{
	memcpy(id->op_pub, op_pub, MC_PUB_KEY_SIZE);
	ed25519_key_exchange(id->secret, id->op_pub, id->prv);
	id->have_operator = true;
}

static int nib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool mc_hex32(const char *hex, uint8_t out[32])
{
	if (strlen(hex) != 64) {
		return false;
	}
	for (int i = 0; i < 32; i++) {
		int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}
