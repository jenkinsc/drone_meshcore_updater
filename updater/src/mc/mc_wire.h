#pragma once

/*
 * MeshCore packet framing and the encrypted-datagram envelope.
 *
 * Just enough of the MeshCore wire format to be a well-behaved *endpoint*: it
 * receives text messages from one known peer, acknowledges them, answers, and
 * announces itself. It does not route, repeat or keep a contact list.
 *
 * Source of truth is MeshCore itself (src/Packet.cpp, src/Mesh.cpp,
 * src/Utils.cpp, docs/packet_format.md, docs/payloads.md). The host tests check
 * this file against vectors produced by MeshCore's own Utils/Identity code, so
 * "matches MeshCore" is a tested claim, not a reading of the docs.
 *
 * ---- Envelope (payload types REQ/RESPONSE/TXT_MSG/PATH, payload version 1) ----
 *
 *   dest_hash(1) src_hash(1) mac(2) ciphertext(16*n)
 *
 *   hash        first byte of the node's Ed25519 public key
 *   secret      32 bytes, X25519 of (own private key, peer's public key after
 *               the Edwards->Montgomery conversion): ed25519_key_exchange()
 *   cipher      AES-128-ECB, key = secret[0..15], plaintext zero-padded
 *   mac         first 2 bytes of HMAC-SHA256(key = all 32 secret bytes, data =
 *               ciphertext)
 *
 * Encrypt-then-MAC, MAC checked before decrypting. Anything that fails the MAC
 * is dropped without a response.
 *
 * ---- Crypto is injected -------------------------------------------------------
 *
 * Hashing and AES come in through struct mc_crypto so the same code runs on the
 * device (tinycrypt) and on a host (OpenSSL) for tests. Ed25519 is the vendored
 * orlp library MeshCore itself uses (ed25519/), called directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_PUB_KEY_SIZE   32
#define MC_PRV_KEY_SIZE   64
#define MC_SECRET_SIZE    32
#define MC_SIG_SIZE       64
#define MC_MAC_SIZE       2
#define MC_MAX_PACKET_PAYLOAD 184
#define MC_MAX_PATH_BYTES 64
#define MC_MAX_RAW        255
#define MC_TEXT_MAX       160    /* longest command text we accept */

/* Route types (header bits 0-1). */
#define MC_ROUTE_TRANSPORT_FLOOD  0
#define MC_ROUTE_FLOOD            1
#define MC_ROUTE_DIRECT           2
#define MC_ROUTE_TRANSPORT_DIRECT 3

/* Payload types (header bits 2-5). */
#define MC_PT_REQ      0x00
#define MC_PT_RESPONSE 0x01
#define MC_PT_TXT_MSG  0x02
#define MC_PT_ACK      0x03
#define MC_PT_ADVERT   0x04
#define MC_PT_PATH     0x08

/* TXT_MSG txt_type (upper six bits of the flags byte). */
#define MC_TXT_PLAIN 0
#define MC_TXT_CLI   1

struct mc_crypto {
	void (*sha256)(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
		       uint8_t out[32]);
	void (*hmac_sha256)(const uint8_t key[32], const uint8_t *msg, size_t len,
			    uint8_t out[32]);
	void (*aes128_ecb_encrypt)(const uint8_t key[16], const uint8_t *in,
				   uint8_t *out, size_t len);
	void (*aes128_ecb_decrypt)(const uint8_t key[16], const uint8_t *in,
				   uint8_t *out, size_t len);
};

struct mc_pkt {
	uint8_t  header;
	uint8_t  route;            /* MC_ROUTE_* */
	uint8_t  ptype;            /* MC_PT_* */
	uint8_t  pver;             /* payload version, 0 = v1 */
	uint8_t  path_len_raw;     /* as on the wire */
	uint8_t  path_bytes;       /* byte length of path[] */
	const uint8_t *path;       /* points into the raw buffer */
	const uint8_t *payload;
	uint16_t payload_len;
};

/* Parse a received frame. 0 on success, -1 on malformed. `pkt` points into
 * `raw`, which must outlive it. Same bounds MeshCore's Dispatcher applies. */
int mc_wire_parse(const uint8_t *raw, size_t len, struct mc_pkt *pkt);

static inline bool mc_wire_is_flood(const struct mc_pkt *p)
{
	return p->route == MC_ROUTE_FLOOD || p->route == MC_ROUTE_TRANSPORT_FLOOD;
}

/* Open a datagram payload (dest, src, mac, ciphertext) after the caller has
 * already matched the hashes. Verifies the MAC, then decrypts into `plain`.
 * Returns the plaintext length (a multiple of 16) or -1 if the MAC is wrong or
 * the sizes are. */
int mc_wire_open(const struct mc_crypto *cr, const uint8_t secret[MC_SECRET_SIZE],
		 const uint8_t *payload, size_t payload_len,
		 uint8_t *plain, size_t plain_cap);

/* A parsed TXT_MSG plaintext. */
struct mc_txt {
	uint32_t ts;
	uint8_t  txt_type;
	uint8_t  attempt;
	size_t   text_len;                 /* excluding NUL */
	char     text[MC_TEXT_MAX + 1];
};

/* plaintext = ts(4 LE) flags(1) text, zero padded. -1 if too short or the
 * text does not fit. */
int mc_wire_parse_txt(const uint8_t *plain, size_t plain_len, struct mc_txt *out);

/* The 6-byte acknowledgement a sender expects for `plain` (as received):
 * sha256(plain[0 .. 5+text_len) || sender_pub)[0..3], then the byte MeshCore
 * carries after the text's NUL, then `salt`. */
void mc_wire_ack_bytes(const struct mc_crypto *cr, const uint8_t *plain,
		       size_t plain_len, size_t text_len,
		       const uint8_t sender_pub[MC_PUB_KEY_SIZE], uint8_t salt,
		       uint8_t out[6]);

/* TXT_MSG to `dst_hash` as a flood. Returns the frame length or 0 if it will
 * not fit in a packet. */
size_t mc_wire_build_txt(const struct mc_crypto *cr,
			 const uint8_t secret[MC_SECRET_SIZE], uint8_t dst_hash,
			 uint8_t src_hash, uint32_t ts, uint8_t txt_type,
			 const char *text, uint8_t *out, size_t cap);

/* The "path return carrying an ACK" MeshCore sends for a flooded message: a
 * PATH packet whose encrypted body is the path the request took plus the ACK.
 * Without it the sender keeps retrying and shows the message as undelivered. */
size_t mc_wire_build_path_ack(const struct mc_crypto *cr,
			      const uint8_t secret[MC_SECRET_SIZE],
			      uint8_t dst_hash, uint8_t src_hash,
			      const struct mc_pkt *request, const uint8_t ack[6],
			      uint8_t *out, size_t cap);

/* A signed ADVERT as a flood. `name` is truncated to fit MeshCore's 32-byte
 * app-data limit. Type is "chat" so a companion app lists the node as a contact
 * it can message. */
size_t mc_wire_build_advert(const uint8_t pub[MC_PUB_KEY_SIZE],
			    const uint8_t prv[MC_PRV_KEY_SIZE], uint32_t ts,
			    const char *name, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
