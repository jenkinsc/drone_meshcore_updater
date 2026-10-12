#include "mc_wire.h"

#include <string.h>

#include "ed25519/ed_25519.h"

#define PATH_LEN_COUNT(p)  ((p) & 63)
#define PATH_LEN_HASHSZ(p) (((p) >> 6) + 1)

int mc_wire_parse(const uint8_t *raw, size_t len, struct mc_pkt *pkt)
{
	size_t i = 0;

	memset(pkt, 0, sizeof(*pkt));
	if (len < 2 || len > MC_MAX_RAW) {
		return -1;
	}

	pkt->header = raw[i++];
	pkt->route  = pkt->header & 0x03;
	pkt->ptype  = (pkt->header >> 2) & 0x0F;
	pkt->pver   = (pkt->header >> 6) & 0x03;

	if (pkt->route == MC_ROUTE_TRANSPORT_FLOOD ||
	    pkt->route == MC_ROUTE_TRANSPORT_DIRECT) {
		if (len < i + 4 + 1) {
			return -1;
		}
		i += 4;                      /* transport codes: not used here */
	}

	pkt->path_len_raw = raw[i++];
	if (PATH_LEN_HASHSZ(pkt->path_len_raw) > 3) {
		return -1;                   /* hash-size code 0b11 is reserved */
	}
	pkt->path_bytes = (uint8_t)(PATH_LEN_COUNT(pkt->path_len_raw) *
				    PATH_LEN_HASHSZ(pkt->path_len_raw));
	if (pkt->path_bytes > MC_MAX_PATH_BYTES) {
		return -1;
	}
	if (i + pkt->path_bytes >= len) {    /* payload must be non-empty */
		return -1;
	}
	pkt->path = &raw[i];
	i += pkt->path_bytes;

	pkt->payload_len = (uint16_t)(len - i);
	if (pkt->payload_len > MC_MAX_PACKET_PAYLOAD) {
		return -1;
	}
	pkt->payload = &raw[i];
	return 0;
}

static void mac_of(const struct mc_crypto *cr, const uint8_t secret[MC_SECRET_SIZE],
		   const uint8_t *cipher, size_t n, uint8_t out[MC_MAC_SIZE])
{
	uint8_t full[32];

	cr->hmac_sha256(secret, cipher, n, full);
	memcpy(out, full, MC_MAC_SIZE);
}

/* Constant-time compare: the MAC is the authentication, so do not leak where
 * the first mismatch was. */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;

	for (size_t i = 0; i < n; i++) {
		d |= (uint8_t)(a[i] ^ b[i]);
	}
	return d == 0;
}

int mc_wire_open(const struct mc_crypto *cr, const uint8_t secret[MC_SECRET_SIZE],
		 const uint8_t *payload, size_t payload_len,
		 uint8_t *plain, size_t plain_cap)
{
	/* dest, src, mac(2), then at least one cipher block. */
	if (payload_len < 2 + MC_MAC_SIZE + 16) {
		return -1;
	}
	const uint8_t *mac = payload + 2;
	const uint8_t *cipher = payload + 2 + MC_MAC_SIZE;
	size_t clen = payload_len - 2 - MC_MAC_SIZE;

	if (clen % 16 != 0 || clen > plain_cap) {
		return -1;
	}

	uint8_t want[MC_MAC_SIZE];
	mac_of(cr, secret, cipher, clen, want);
	if (!ct_equal(want, mac, MC_MAC_SIZE)) {
		return -1;
	}

	cr->aes128_ecb_decrypt(secret, cipher, plain, clen);   /* key = secret[0..15] */
	return (int)clen;
}

int mc_wire_parse_txt(const uint8_t *plain, size_t plain_len, struct mc_txt *out)
{
	memset(out, 0, sizeof(*out));
	if (plain_len < 6) {
		return -1;
	}
	out->ts = (uint32_t)plain[0] | ((uint32_t)plain[1] << 8) |
		  ((uint32_t)plain[2] << 16) | ((uint32_t)plain[3] << 24);
	out->txt_type = (uint8_t)(plain[4] >> 2);
	out->attempt  = (uint8_t)(plain[4] & 3);

	size_t n = 0;
	while (5 + n < plain_len && plain[5 + n] != 0) {
		n++;
	}
	if (n > MC_TEXT_MAX) {
		return -1;
	}
	memcpy(out->text, &plain[5], n);
	out->text[n] = '\0';
	out->text_len = n;
	return 0;
}

void mc_wire_ack_bytes(const struct mc_crypto *cr, const uint8_t *plain,
		       size_t plain_len, size_t text_len,
		       const uint8_t sender_pub[MC_PUB_KEY_SIZE], uint8_t salt,
		       uint8_t out[6])
{
	uint8_t h[32];

	cr->sha256(plain, 5 + text_len, sender_pub, MC_PUB_KEY_SIZE, h);
	memcpy(out, h, 4);
	/* MeshCore appends the byte after the text's terminator ("extended attempt
	 * byte", BaseChatMesh.cpp) so retries differ on the wire. */
	out[4] = (5 + text_len + 1 < plain_len) ? plain[5 + text_len + 1] : 0;
	out[5] = salt;
}

/* Seal `data` into out[0..]: dst, src, mac, cipher. Returns total length or 0. */
static size_t seal(const struct mc_crypto *cr, const uint8_t secret[MC_SECRET_SIZE],
		   uint8_t dst_hash, uint8_t src_hash, const uint8_t *data,
		   size_t data_len, uint8_t *out, size_t cap)
{
	size_t clen = (data_len + 15) & ~(size_t)15;
	size_t total = 2 + MC_MAC_SIZE + clen;

	if (total > MC_MAX_PACKET_PAYLOAD || total > cap) {
		return 0;
	}

	uint8_t padded[MC_MAX_PACKET_PAYLOAD];
	memset(padded, 0, clen);
	memcpy(padded, data, data_len);

	out[0] = dst_hash;
	out[1] = src_hash;
	cr->aes128_ecb_encrypt(secret, padded, out + 2 + MC_MAC_SIZE, clen);
	mac_of(cr, secret, out + 2 + MC_MAC_SIZE, clen, out + 2);
	return total;
}

size_t mc_wire_build_txt(const struct mc_crypto *cr,
			 const uint8_t secret[MC_SECRET_SIZE], uint8_t dst_hash,
			 uint8_t src_hash, uint32_t ts, uint8_t txt_type,
			 const char *text, uint8_t *out, size_t cap)
{
	uint8_t data[MC_MAX_PACKET_PAYLOAD];
	size_t tl = strlen(text);

	if (5 + tl + 1 > sizeof(data) - 16 || cap < 2) {
		return 0;
	}
	data[0] = (uint8_t)ts;
	data[1] = (uint8_t)(ts >> 8);
	data[2] = (uint8_t)(ts >> 16);
	data[3] = (uint8_t)(ts >> 24);
	data[4] = (uint8_t)(txt_type << 2);           /* attempt 0 */
	memcpy(&data[5], text, tl);

	/* header: payload type TXT_MSG, route FLOOD, version 1; path_len 0 */
	out[0] = (uint8_t)((MC_PT_TXT_MSG << 2) | MC_ROUTE_FLOOD);
	out[1] = 0;
	size_t n = seal(cr, secret, dst_hash, src_hash, data, 5 + tl, out + 2, cap - 2);
	return n ? n + 2 : 0;
}

size_t mc_wire_build_path_ack(const struct mc_crypto *cr,
			      const uint8_t secret[MC_SECRET_SIZE],
			      uint8_t dst_hash, uint8_t src_hash,
			      const struct mc_pkt *request, const uint8_t ack[6],
			      uint8_t *out, size_t cap)
{
	uint8_t data[1 + MC_MAX_PATH_BYTES + 1 + 6];
	size_t n = 0;

	if (cap < 2) {
		return 0;
	}
	/* path_len byte, the hops the request travelled, extra type, extra. */
	data[n++] = request->path_len_raw;
	memcpy(&data[n], request->path, request->path_bytes);
	n += request->path_bytes;
	data[n++] = MC_PT_ACK;
	memcpy(&data[n], ack, 6);
	n += 6;

	out[0] = (uint8_t)((MC_PT_PATH << 2) | MC_ROUTE_FLOOD);
	out[1] = 0;
	size_t s = seal(cr, secret, dst_hash, src_hash, data, n, out + 2, cap - 2);
	return s ? s + 2 : 0;
}

size_t mc_wire_build_advert(const uint8_t pub[MC_PUB_KEY_SIZE],
			    const uint8_t prv[MC_PRV_KEY_SIZE], uint32_t ts,
			    const char *name, uint8_t *out, size_t cap)
{
	enum { ADV_TYPE_CHAT = 1, ADV_NAME_MASK = 0x80, APP_MAX = 32 };
	uint8_t app[APP_MAX];
	size_t al = 0;

	app[al++] = ADV_TYPE_CHAT | ADV_NAME_MASK;
	for (const char *p = name; *p && al < APP_MAX; p++) {
		app[al++] = (uint8_t)*p;
	}

	size_t need = 2 + MC_PUB_KEY_SIZE + 4 + MC_SIG_SIZE + al;
	if (need > cap) {
		return 0;
	}

	uint8_t msg[MC_PUB_KEY_SIZE + 4 + APP_MAX];
	memcpy(msg, pub, MC_PUB_KEY_SIZE);
	msg[32] = (uint8_t)ts;
	msg[33] = (uint8_t)(ts >> 8);
	msg[34] = (uint8_t)(ts >> 16);
	msg[35] = (uint8_t)(ts >> 24);
	memcpy(&msg[36], app, al);

	out[0] = (uint8_t)((MC_PT_ADVERT << 2) | MC_ROUTE_FLOOD);
	out[1] = 0;
	memcpy(&out[2], pub, MC_PUB_KEY_SIZE);
	memcpy(&out[2 + 32], &msg[32], 4);
	ed25519_sign(&out[2 + 36], msg, 36 + al, pub, prv);
	memcpy(&out[2 + 36 + MC_SIG_SIZE], app, al);
	return need;
}
