#pragma once

/*
 * The updater's MeshCore identity and its one trusted peer.
 *
 * The node has an Ed25519 keypair of its own (generated once, kept in the
 * protected flash page — see mc_store.h) and exactly one *operator* public key
 * from configuration. The shared secret between them is computed once.
 *
 * Authorisation is cryptographic, not nominal. A message is attributed to the
 * operator only if its MAC verifies under the secret derived from the
 * operator's public key; the one-byte "source hash" in the packet is just a
 * routing hint and is never trusted on its own. Nobody without the operator's
 * private key can produce a message that passes — and that includes anyone who
 * merely knows the operator's public key.
 *
 * What would break it: the node's *private* key leaking. With it, the secret
 * can be recomputed and operator messages forged. That is why it lives in a
 * flash page no file service can read.
 */

#include <stdbool.h>
#include <stdint.h>

#include "mc_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mc_id {
	uint8_t pub[MC_PUB_KEY_SIZE];
	uint8_t prv[MC_PRV_KEY_SIZE];
	uint8_t op_pub[MC_PUB_KEY_SIZE];
	uint8_t secret[MC_SECRET_SIZE];   /* node <-> operator */
	bool    have_operator;
};

/* Derive the keypair from a 32-byte random seed. Returns false if the result
 * is not usable as a MeshCore identity (public key beginning 0x00 or 0xFF,
 * which MeshCore reserves — the caller draws a fresh seed and retries). */
bool mc_id_from_seed(struct mc_id *id, const uint8_t seed[32]);

/* Load an existing keypair; re-derives the public key from the private key so
 * a corrupt store cannot yield a mismatched pair. */
bool mc_id_from_private(struct mc_id *id, const uint8_t prv[MC_PRV_KEY_SIZE]);

/* Set the operator's public key and compute the shared secret. */
void mc_id_set_operator(struct mc_id *id, const uint8_t op_pub[MC_PUB_KEY_SIZE]);

/* Parse 64 hex characters into 32 bytes. False on any non-hex or wrong length. */
bool mc_hex32(const char *hex, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
