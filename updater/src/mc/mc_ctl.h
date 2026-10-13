#pragma once

/*
 * The MeshCore command controller: the state machine behind arm / start /
 * abort, with every safety rule in one place.
 *
 * No Zephyr, no radio, no flash. Everything the controller needs from the
 * outside world comes through struct mc_ops, so the same code runs on the
 * device (mc_zephyr.c supplies the ops) and under the host tests (which supply
 * fakes and a controllable clock). That is what makes "START is refused unless
 * the gate has been satisfied for 5 seconds" something a test can assert rather
 * than something a reviewer has to believe.
 *
 * ---- States --------------------------------------------------------------
 *
 *   IDLE              nothing selected. The power-on state, always: nothing
 *                     here survives a reboot, so a device that resets mid-flight
 *                     comes back inert rather than re-armed.
 *   WAITING           ARM accepted; the armed address has not been heard yet.
 *   TARGET_DETECTED   an advertisement from the exact armed address was heard.
 *   RSSI_QUALIFIED    the RSSI gate is satisfied; the bundle is being re-hashed.
 *   ARMED             qualified and the bundle matches its ARM-time digest.
 *                     The only state in which START is accepted.
 *   START_AUTHORIZED  START passed every check. Exists only inside the START
 *                     call, between the decision and the DFU runner consuming
 *                     the authorisation (mc_authz.h). Never observable from
 *                     outside, never time-based.
 *   DFU_ACTIVE        the runner is running.
 *
 *   IDLE -> WAITING -> TARGET_DETECTED -> RSSI_QUALIFIED -> ARMED
 *        -> START_AUTHORIZED -> DFU_ACTIVE -> IDLE
 *
 * Backwards edges: the gate losing the target (silence, a sample under the
 * floor, the mean collapsing) drops ARMED / RSSI_QUALIFIED to TARGET_DETECTED;
 * ARM expiry, DISARM, ABORT, a changed bundle, and any DFU outcome go to IDLE.
 *
 * ---- Rules enforced here -------------------------------------------------
 *
 *   - arm/disarm/start/abort need `authed`; the transport decides that from the
 *     message's sender key, never from anything in the text.
 *   - ARM and START additionally need a fresh single-use *token* (below).
 *   - ARM needs a well-formed exact address and a readable bundle, and records
 *     the bundle's SHA-256. START re-hashes it and refuses on any difference.
 *     The bundle is write-locked from ARM until the controller returns to IDLE.
 *   - START needs: ARMED, the gate ready *now*, the digest still matching, and
 *     the DFU runner idle.
 *   - ARM lapses after `arm_timeout_s` without a START.
 *   - ABORT of an active run is honoured only while the target's application
 *     is still intact (before the DFU START packet). After that the target has
 *     erased its application and cancelling would leave it no better off, so
 *     the answer is a refusal that says why.
 *   - Nothing here ever starts a DFU by itself. There is no timer, no
 *     auto-start flag, and no path from tick() or an advertisement sample to
 *     ops->dfu_start().
 *
 * ---- Replay protection: single-use tokens --------------------------------
 *
 * Standard MeshCore clients cannot send a monotonic counter, and a persisted
 * "newest timestamp" turns one wrong operator clock into a lockout. So ARM and
 * START carry a token the node itself issued: six hex digits from the hardware
 * RNG, valid once, for `token_ttl_s`, and never persisted (a reboot discards
 * it). A captured command can be replayed but its token is already spent; a
 * guessed one needs luck against 2^24 *and* the MAC. See MESHCORE.md.
 *
 * DISARM and ABORT take no token: they only ever remove capability, replaying
 * either is harmless, and an emergency stop must not wait for a round trip.
 *
 * ---- Reports ---------------------------------------------------------------
 *
 * Unsolicited messages go through a small queue with a minimum gap between
 * transmissions (`report_gap_ms`); a newer milestone replaces a still-queued
 * older one, terminal messages are never dropped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mc_cmd.h"
#include "mc_gate.h"
#include "mc_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_REPLY_MAX 150   /* characters per reply, NUL excluded */
#define MC_DIGEST_LEN 32
#define MC_FW_ID_LEN  8    /* hex characters of the digest shown to the operator */
#define MC_REPORT_Q   4

struct mc_scan_row {
	char    addr[MC_ADDR_STR_LEN + 1];
	char    type;                  /* 'r' random, 'p' public */
	int8_t  rssi;
	char    name[24];
};

struct mc_ops {
	uint32_t (*now_ms)(void *ctx);
	uint32_t (*rand32)(void *ctx);

	/* Up to `max` rows of the BLE survey from row `off`, strongest first.
	 * `*total` receives the number held. Returns rows written. */
	size_t (*scan_rows)(void *ctx, struct mc_scan_row *rows, size_t max,
			    size_t off, size_t *total);

	/* Start / stop watching one exact address (NULL addr = stop). While
	 * watching, the platform calls mc_ctl_on_adv() for each advertisement. */
	void (*watch)(void *ctx, const char *addr, char type);

	/* SHA-256 of the whole bundle file. 0 on success; negative if it is
	 * missing, empty or unreadable. */
	int (*bundle_digest)(void *ctx, const char *bundle, uint8_t out[MC_DIGEST_LEN]);

	/* Refuse (NULL = stop refusing) writes, deletes and renames of this
	 * bundle by every other path. */
	void (*bundle_lock)(void *ctx, const char *bundle);

	/* Begin the DFU. 0 on success, negative errno otherwise. Only ever called
	 * from a START that passed every check, with the controller in
	 * START_AUTHORIZED. `plus1_ok`: the armed device was seen in application
	 * mode, so its post-jump DFU address (A+1) is part of its identity. */
	int (*dfu_start)(void *ctx, const char *bundle, const char *addr, char type,
			 bool plus1_ok);
	/* Lab-only read-only diagnostics (diag / radio / ble / storage). NULL in a
	 * build without them. Returns the reply length. */
	size_t (*diag)(void *ctx, int cmd_id, char *out, size_t cap);

	/* The run is over (or never began): clear any platform-side authorisation. */
	void (*dfu_end)(void *ctx);
	int (*dfu_abort)(void *ctx);
	bool (*dfu_busy)(void *ctx);

	int (*battery)(void *ctx, uint16_t *mv, uint8_t *pct, int *charging);
	const char *(*version)(void *ctx);
	size_t (*log_page)(void *ctx, unsigned page, char *out, size_t cap);

	/* An unsolicited message for the operator (milestone, result, expiry). */
	void (*report)(void *ctx, const char *msg);
	/* One line for the event log. */
	void (*note)(void *ctx, const char *msg);
};

enum mc_state {
	MC_IDLE = 0,
	MC_WAITING,
	MC_TARGET_DETECTED,
	MC_RSSI_QUALIFIED,
	MC_ARMED,
	MC_START_AUTHORIZED,
	MC_DFU_ACTIVE,
};

enum mc_last {
	MC_LAST_NONE = 0,
	MC_LAST_OK,
	MC_LAST_FAILED,
	MC_LAST_ABORTED,
	MC_LAST_EXPIRED,
	MC_LAST_BUNDLE_CHANGED,
	MC_LAST_DRYRUN,
};

/* dfu_start() return value: the platform is built as a dry run. Every check
 * passed, no DFU was started, nothing on the target was touched. */
#define MC_START_DRY_RUN 1

struct mc_cfg {
	struct mc_gate_cfg gate;
	uint32_t arm_timeout_s;   /* 0 = never lapse */
	uint32_t token_ttl_s;
	uint32_t report_gap_ms;
};

struct mc_report_slot {
	uint8_t kind;             /* 0 empty, 1 milestone (replaceable), 2 other */
	char    text[MC_REPLY_MAX + 1];
};

struct mc_ctl {
	const struct mc_ops *ops;
	void *ctx;
	struct mc_cfg cfg;

	enum mc_state state;
	struct mc_gate gate;
	char     addr[MC_ADDR_STR_LEN + 1];
	char     addr_type;
	char     bundle[MC_BUNDLE_MAX + 1];
	uint8_t  digest[MC_DIGEST_LEN];
	char     fw[MC_FW_ID_LEN + 1];
	bool     target_dfu_mode;   /* the armed address advertised the DFU UUID */
	uint32_t armed_at;

	/* single-use token */
	uint32_t token;
	uint32_t token_at;
	bool     token_valid;

	/* last run */
	enum mc_last last;
	enum dfu_status_result last_result;
	struct mc_progress prog;
	struct dfu_status_event ev;      /* latest DFU snapshot, for status */

	/* report queue */
	struct mc_report_slot rq[MC_REPORT_Q];
	uint32_t last_tx_ms;
	bool     any_tx;
};

void mc_ctl_init(struct mc_ctl *c, const struct mc_ops *ops, void *ctx,
		 const struct mc_cfg *cfg);

/* Handle one command line. `authed` is true only when the transport verified
 * the sender is the configured operator key. Writes a reply (NUL-terminated,
 * <= MC_REPLY_MAX characters) and returns its length. Never returns 0. */
size_t mc_ctl_handle(struct mc_ctl *c, bool authed, const char *text,
		     char *reply, size_t cap);

/* One advertisement from the watched address (the platform already matched it
 * exactly). `dfu_uuid`: this packet carried the Nordic Legacy DFU UUID. */
void mc_ctl_on_adv(struct mc_ctl *c, int8_t rssi, bool dfu_uuid);

/* Samples were lost (queue overflow): the gate can no longer vouch for what
 * happened in its window, so it starts again. Conservative by construction. */
void mc_ctl_samples_lost(struct mc_ctl *c);

/* A DFU status change. Queues milestone / result reports. */
void mc_ctl_on_dfu_event(struct mc_ctl *c, const struct dfu_status_event *ev);

/* Periodic housekeeping: arm timeout, gate staleness, report flush. ~1 Hz. */
void mc_ctl_tick(struct mc_ctl *c);

const char *mc_state_str(enum mc_state s);

/* True while an unsolicited transmission other than a queued DFU report should
 * be held back (a DFU run is in progress). */
static inline bool mc_ctl_quiet(const struct mc_ctl *c)
{
	return c->state == MC_START_AUTHORIZED || c->state == MC_DFU_ACTIVE;
}

#ifdef __cplusplus
}
#endif
