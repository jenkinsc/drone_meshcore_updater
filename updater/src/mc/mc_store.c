#include "mc_store.h"

#include <string.h>

static const uint8_t MAGIC[8] = { 'M', 'C', 'I', 'D', '0', '0', '0', '1' };

#define H_MAGIC 0
#define H_PRV   8
#define H_PUB   (H_PRV + MC_PRV_KEY_SIZE)
#define H_CRC   (H_PUB + MC_PUB_KEY_SIZE)
#define H_LEN   (H_CRC + 4)

uint32_t mc_crc32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t c = 0xFFFFFFFFu;

	while (len--) {
		c ^= *p++;
		for (int i = 0; i < 8; i++) {
			c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1u));
		}
	}
	return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int is_erased(const uint8_t *p, size_t n)
{
	while (n--) {
		if (*p++ != 0xFF) {
			return 0;
		}
	}
	return 1;
}

int mc_store_load(struct mc_store *s, const struct mc_flash *fl, struct mc_persist *p)
{
	uint8_t hdr[MC_STORE_HDR_SIZE];

	memset(s, 0, sizeof(*s));
	s->fl = fl;
	memset(p, 0, sizeof(*p));

	if (fl->read(fl->ctx, 0, hdr, sizeof(hdr)) != 0) {
		return MC_STORE_IOERR;
	}
	if (memcmp(hdr + H_MAGIC, MAGIC, sizeof(MAGIC)) != 0 ||
	    mc_crc32(hdr, H_CRC) != get32(hdr + H_CRC)) {
		return MC_STORE_EMPTY;
	}
	memcpy(s->prv, hdr + H_PRV, MC_PRV_KEY_SIZE);
	memcpy(s->pub, hdr + H_PUB, MC_PUB_KEY_SIZE);

	bool have = false;

	for (uint32_t i = 0; i < MC_STORE_MAX_RECS; i++) {
		uint8_t r[MC_STORE_REC_SIZE];

		if (fl->read(fl->ctx, MC_STORE_HDR_SIZE + i * MC_STORE_REC_SIZE, r, sizeof(r)) != 0) {
			return MC_STORE_IOERR;
		}
		if (is_erased(r, sizeof(r))) {
			continue;
		}
		s->next_slot = i + 1;       /* anything non-erased, even torn, is used */
		if (mc_crc32(r, 12) != get32(r + 12)) {
			continue;
		}
		uint32_t seq = get32(r + 8);

		if (!have || (int32_t)(seq - s->seq) > 0) {
			have = true;
			s->seq = seq;
			p->reserved = get32(r);
			p->adv_ts = get32(r + 4);
		}
	}
	return MC_STORE_OK;
}

static int write_header(struct mc_store *s)
{
	uint8_t hdr[H_LEN];

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr + H_MAGIC, MAGIC, sizeof(MAGIC));
	memcpy(hdr + H_PRV, s->prv, MC_PRV_KEY_SIZE);
	memcpy(hdr + H_PUB, s->pub, MC_PUB_KEY_SIZE);
	put32(hdr + H_CRC, mc_crc32(hdr, H_CRC));
	return s->fl->write(s->fl->ctx, 0, hdr, sizeof(hdr));
}

int mc_store_provision(struct mc_store *s, const uint8_t prv[MC_PRV_KEY_SIZE],
		       const uint8_t pub[MC_PUB_KEY_SIZE])
{
	memcpy(s->prv, prv, MC_PRV_KEY_SIZE);
	memcpy(s->pub, pub, MC_PUB_KEY_SIZE);
	s->next_slot = 0;
	s->seq = 0;
	if (s->fl->erase(s->fl->ctx) != 0) {
		return MC_STORE_IOERR;
	}
	return write_header(s) == 0 ? MC_STORE_OK : MC_STORE_IOERR;
}

int mc_store_append(struct mc_store *s, const struct mc_persist *p)
{
	if (s->next_slot >= MC_STORE_MAX_RECS) {
		/* Full. The identity is in RAM; erase and put it straight back. */
		if (s->fl->erase(s->fl->ctx) != 0 || write_header(s) != 0) {
			return MC_STORE_IOERR;
		}
		s->next_slot = 0;
	}
	uint8_t r[MC_STORE_REC_SIZE];

	put32(r, p->reserved);
	put32(r + 4, p->adv_ts);
	put32(r + 8, s->seq + 1);
	put32(r + 12, mc_crc32(r, 12));
	if (s->fl->write(s->fl->ctx, MC_STORE_HDR_SIZE + s->next_slot * MC_STORE_REC_SIZE,
			 r, sizeof(r)) != 0) {
		return MC_STORE_IOERR;
	}
	s->next_slot++;
	s->seq++;
	return MC_STORE_OK;
}
