#pragma once

/*
 * The MeshCore node: raw LoRa frames in, authorised commands out, replies and
 * reports back as frames.
 *
 * This is where a packet becomes a decision. Everything before it is framing
 * and crypto; everything after it is the controller. It lives apart from the
 * Zephyr glue so the whole path — "is this packet from the operator, is it
 * fresh, what did it ask for, what do we say back" — runs under the host tests
 * with real MeshCore-produced packets.
 *
 * ---- Who gets to command the updater ------------------------------------------
 *
 * Only the configured operator public key. A frame is accepted as a command iff
 *
 *   1. it is a v1 TXT_MSG addressed to this node (dest hash = our key's first
 *      byte), and its source hash is the operator's;
 *   2. its MAC verifies under the secret derived from the operator's public key
 *      (this, not the hash byte, is the authentication);
 *   3. its timestamp is not older than the newest accepted and it is not a
 *      repeat (mc_replay.h).
 *
 * Everything else is dropped without a reply: an unknown sender gets no
 * "access denied" to probe against. Messages from anyone else cannot even be
 * decrypted — this endpoint holds one peer's secret and no contact list — so
 * the read-only commands are not reachable by strangers either. (The
 * privileged/read-only split is still enforced in the controller; it simply has
 * nobody but the operator to apply it to.)
 *
 * Outputs go through a tx callback that queues the frame; the glue decides when
 * to key the radio.
 */

#include <stddef.h>
#include <stdint.h>

#include "mc_ctl.h"
#include "mc_id.h"
#include "mc_replay.h"
#include "mc_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What must survive a reboot. */
struct mc_persist {
	uint32_t replay_ts;   /* newest accepted operator timestamp */
	uint32_t adv_ts;      /* newest timestamp we put on an advert */
};

struct mc_node_ops {
	void (*tx)(void *ctx, const uint8_t *frame, size_t len);
	/* Called when persisted state changed. May be slow (flash); the node calls
	 * it only on privileged commands and adverts, never per packet. */
	void (*persist)(void *ctx, const struct mc_persist *p);
	void (*note)(void *ctx, const char *msg);
	uint32_t (*now_ms)(void *ctx);
	uint8_t (*random_byte)(void *ctx);
};

struct mc_node {
	const struct mc_crypto *cr;
	struct mc_id *id;
	struct mc_ctl *ctl;
	const struct mc_node_ops *ops;
	void *ctx;

	struct mc_replay replay;
	struct mc_persist saved;

	/* Clock for outgoing timestamps: the best real time we know, advanced by
	 * uptime. There is no RTC; the operator's own timestamps are the only
	 * source of wall-clock time. */
	uint32_t clock_ts;
	uint32_t clock_at_ms;

	uint32_t rx_frames;
	uint32_t rx_bad;       /* malformed, wrong MAC, wrong peer */
	uint32_t rx_replay;
};

void mc_node_init(struct mc_node *n, const struct mc_crypto *cr, struct mc_id *id,
		  struct mc_ctl *ctl, const struct mc_node_ops *ops, void *ctx,
		  const struct mc_persist *persisted);

/* Offer one received frame. */
void mc_node_rx(struct mc_node *n, const uint8_t *raw, size_t len);

/* Send an unsolicited text to the operator (milestones, results). */
void mc_node_send_text(struct mc_node *n, const char *text);

/* Build and send a signed advert so the operator's app has a contact to
 * message. */
void mc_node_advertise(struct mc_node *n, const char *name);

#ifdef __cplusplus
}
#endif
