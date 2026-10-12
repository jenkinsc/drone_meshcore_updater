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
 *   DISARMED   idle. START is refused. The power-on state, always: nothing
 *              here survives a reboot, so a device that resets mid-flight comes
 *              back inert rather than re-armed.
 *   ARMED      a target and bundle are chosen and the gate is watching. START
 *              is accepted only once the gate says ready.
 *   RUNNING    a DFU was started. Arming is consumed.
 *
 * START moves ARMED -> RUNNING and the arming is gone: a second target needs a
 * second ARM. A run's outcome (DONE / FAILED) is remembered for `status` until
 * the next ARM.
 *
 * ---- Rules enforced here -------------------------------------------------
 *
 *   - arm/disarm/start/abort need `authed`; the transport decides that from the
 *     message's sender key, never from anything in the text.
 *   - ARM needs a well-formed exact address and an existing bundle.
 *   - START needs: ARMED, gate ready *now*, and the DFU runner idle.
 *   - An armed state lapses after `arm_timeout_s` without a START.
 *   - Nothing here ever starts a DFU by itself. There is no timer, no
 *     auto-start flag, and no path from tick() or an advertisement sample to
 *     ops->dfu_start().
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

struct mc_scan_row {
	char    addr[MC_ADDR_STR_LEN + 1];
	char    type;                  /* 'r' random, 'p' public */
	int8_t  rssi;
	char    name[24];
};

struct mc_ops {
	uint32_t (*now_ms)(void *ctx);

	/* Up to `max` rows of the BLE survey from row `off`, strongest first.
	 * `*total` receives the number held. Returns rows written. */
	size_t (*scan_rows)(void *ctx, struct mc_scan_row *rows, size_t max,
			    size_t off, size_t *total);

	/* Start / stop watching one exact address (NULL addr = stop). While
	 * watching, the platform calls mc_ctl_on_adv() for each advertisement. */
	void (*watch)(void *ctx, const char *addr, char type);

	/* 0 if `bundle` exists and is non-empty. */
	int (*bundle_check)(void *ctx, const char *bundle);

	/* Begin the DFU. 0 on success, negative errno otherwise. Only ever
	 * called from a START that passed every check. */
	int (*dfu_start)(void *ctx, const char *bundle, const char *addr, char type);
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
	MC_DISARMED = 0,
	MC_ARMED,
	MC_RUNNING,
};

enum mc_last {
	MC_LAST_NONE = 0,
	MC_LAST_OK,
	MC_LAST_FAILED,
	MC_LAST_ABORTED,
};

struct mc_cfg {
	int8_t   rssi_min;       /* dBm gate threshold */
	uint32_t hold_ms;        /* how long it must hold; MC_GATE_DEFAULT_HOLD_MS */
	uint32_t max_gap_ms;
	uint8_t  min_samples;
	uint32_t arm_timeout_s;  /* 0 = never lapse */
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
	uint32_t armed_at;

	/* last run */
	enum mc_last last;
	enum dfu_status_result last_result;
	struct mc_progress prog;
	struct dfu_status_event ev;      /* latest DFU snapshot, for status */
};

void mc_ctl_init(struct mc_ctl *c, const struct mc_ops *ops, void *ctx,
		 const struct mc_cfg *cfg);

/* Handle one command line. `authed` is true only when the transport verified
 * the sender is the configured operator key. Writes a reply (NUL-terminated,
 * <= MC_REPLY_MAX characters) and returns its length. Never returns 0. */
size_t mc_ctl_handle(struct mc_ctl *c, bool authed, const char *text,
		     char *reply, size_t cap);

/* One advertisement from the watched address (the platform already matched it
 * exactly). */
void mc_ctl_on_adv(struct mc_ctl *c, int8_t rssi);

/* Samples were lost (queue overflow): the gate can no longer vouch for what
 * happened in its window, so it starts again. Conservative by construction. */
void mc_ctl_samples_lost(struct mc_ctl *c);

/* A DFU status change. Emits milestone / result reports through ops->report. */
void mc_ctl_on_dfu_event(struct mc_ctl *c, const struct dfu_status_event *ev);

/* Periodic housekeeping (arm timeout). Cheap; call at ~1 Hz. */
void mc_ctl_tick(struct mc_ctl *c);

const char *mc_state_str(enum mc_state s);

#ifdef __cplusplus
}
#endif
