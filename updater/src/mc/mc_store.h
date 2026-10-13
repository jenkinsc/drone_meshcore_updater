#pragma once

/*
 * Persistent identity and counters on one 4 KB flash page.
 *
 *   0x000  header   magic, private key (64), public key (32), CRC-32
 *   0x080  records  16 bytes each, append-only: reserved (was replay_ts), adv_ts, seq, CRC-32
 *
 * Why not littlefs: the DFU bundles and config.txt live on a filesystem that
 * any BLE client can rewrite over SMP (fsx_mgmt has no path restrictions). The
 * node's private key must not be on it.
 *
 * Why append-only records: a rewritten slot would wear one cell and a torn
 * write would lose it; appending 16-byte records spreads the wear over the page
 * and a torn record is just skipped (its CRC fails).
 *
 * How often it is written: only the advert-timestamp high-water mark lives
 * here (mc_node.h), once for the first advert and then at most once per day of
 * uptime. Replay protection is a RAM-only token, so a privileged command costs
 * no flash write. 248 records at one a day is most of a year of continuous
 * uptime per rollover, and a reflash of the application or a reboot writes
 * nothing.
 *
 * The weak point, stated plainly: when the page fills (248 records) it must be
 * erased, which erases the identity too, and it is rewritten from RAM straight
 * after. A power cut inside that window (~100 ms) loses the identity, and the
 * node will generate a new one on the next boot, which the operator must
 * re-add. The glue logs a warning once the page is 80% used.
 *
 * Portable: flash access goes through struct mc_flash so the host tests can
 * run it on a RAM page that enforces NOR semantics (a write can only clear
 * bits) and cut power at every step.
 */

#include <stddef.h>
#include <stdint.h>

#include "mc_node.h"
#include "mc_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_STORE_PAGE      4096u
#define MC_STORE_HDR_SIZE  128u
#define MC_STORE_REC_SIZE  16u
#define MC_STORE_MAX_RECS  ((MC_STORE_PAGE - MC_STORE_HDR_SIZE) / MC_STORE_REC_SIZE)

struct mc_flash {
	int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
	int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);  /* 4-byte aligned */
	int (*erase)(void *ctx);                                              /* whole page */
	void *ctx;
};

struct mc_store {
	const struct mc_flash *fl;
	uint8_t  prv[MC_PRV_KEY_SIZE];
	uint8_t  pub[MC_PUB_KEY_SIZE];
	uint32_t next_slot;     /* first slot after the last non-erased one */
	uint32_t seq;           /* sequence of the newest valid record */
};

enum mc_store_rc {
	MC_STORE_OK = 0,
	MC_STORE_EMPTY = 1,     /* no valid identity: provision one */
	MC_STORE_IOERR = -1,
};

/* Read the page. On MC_STORE_OK `*p` holds the newest record (zeros if none). */
int mc_store_load(struct mc_store *s, const struct mc_flash *fl, struct mc_persist *p);

/* Erase and write a new identity. */
int mc_store_provision(struct mc_store *s, const uint8_t prv[MC_PRV_KEY_SIZE],
		       const uint8_t pub[MC_PUB_KEY_SIZE]);

/* Append a record, rolling the page over (re-writing the identity) if full. */
int mc_store_append(struct mc_store *s, const struct mc_persist *p);

uint32_t mc_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
