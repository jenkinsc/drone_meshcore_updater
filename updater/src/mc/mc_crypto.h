#pragma once

/*
 * Self-contained AES-128, SHA-256 and HMAC-SHA256 for the MeshCore envelope.
 *
 * Why not the platform's crypto: the Zephyr/NCS crypto stack changes underneath
 * this project between SDK releases (TinyCrypt is on its way out, PSA needs
 * per-board Kconfig). The envelope needs three small primitives, so they are
 * carried here instead. They are plain C99, tested on the host against OpenSSL
 * on thousands of random inputs (updater/mc-tests), and the host tests also run
 * the MeshCore interoperability vectors through *this* implementation, so what
 * is verified is the code that ships.
 *
 * Not constant-time (table lookups, data-dependent branches in the field
 * arithmetic). The only secrets are session keys derived from a long-term key,
 * and a timing attack needs fine-grained timing of many operations on the
 * device, which a LoRa link at ~1 packet a second does not provide. Noted
 * rather than hidden.
 */

#include <stddef.h>
#include <stdint.h>

#include "mc_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

void mc_sha256(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
	       uint8_t out[32]);
void mc_hmac_sha256(const uint8_t key[32], const uint8_t *msg, size_t len,
		    uint8_t out[32]);
void mc_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out,
			   size_t len);
void mc_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out,
			   size_t len);

extern const struct mc_crypto MC_CRYPTO_BUILTIN;

#ifdef __cplusplus
}
#endif
