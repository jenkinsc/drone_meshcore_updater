#pragma once

/*
 * START gate: is the armed BLE target close enough, for long enough?
 *
 * Pure logic, no Zephyr: the host tests in updater/mc-tests compile this file
 * directly. The caller feeds it one sample per received advertisement from the
 * armed target and asks mc_gate_check() when it needs a verdict.
 *
 * ---- The rule ------------------------------------------------------------
 *
 * A hovering drone's RSSI is noisy: individual advertisements swing by 10 dB
 * or more with multipath and antenna orientation. So the gate judges the
 * *average*, bounded below by a hard floor, over a minimum duration:
 *
 *   hard floor   any single sample below `floor` throws the acquisition away
 *                and starts it again. Nothing averages a genuinely bad sample
 *                back into acceptability.
 *   gap          silence longer than `max_gap_ms` also restarts it: an
 *                advertiser that went quiet has told us nothing about the link
 *                in that window.
 *   duration     the acquisition must have run for `hold_ms` (default 5 s)
 *                from its first accepted sample.
 *   samples      at least `min_samples` advertisements must back it.
 *   average      the rolling mean of the last MC_GATE_RING samples must be at
 *                or above `avg_min`.
 *
 * ---- Hysteresis -----------------------------------------------------------
 *
 * Once the gate is READY it stays READY until the rolling mean falls below
 * `avg_min - hyst_db`, a sample falls below the floor, or the target goes quiet.
 * A single slightly weak packet therefore does not flap the verdict. To become
 * ready again after losing it, the full rule applies from scratch (the floor
 * and gap rules restart the acquisition; a mere mean dip just has to recover
 * above `avg_min`).
 *
 * START is always evaluated at the instant it arrives, never from a stored
 * verdict: a window that ended (last sample too old) is not ready.
 *
 * Times are uint32_t milliseconds and wrap-safe (differences only).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_GATE_DEFAULT_HOLD_MS     5000u
#define MC_GATE_DEFAULT_MAX_GAP_MS  3000u
#define MC_GATE_DEFAULT_MIN_SAMPLES 8u
#define MC_GATE_DEFAULT_AVG_MIN     (-65)
#define MC_GATE_DEFAULT_FLOOR       (-75)
#define MC_GATE_DEFAULT_HYST_DB     3u

/* Rolling-mean window, in samples. */
#define MC_GATE_RING 16

struct mc_gate_cfg {
	int8_t   avg_min;       /* dBm: minimum rolling average */
	int8_t   floor;         /* dBm: hard floor; any sample below resets */
	uint8_t  hyst_db;       /* dB below avg_min tolerated once ready */
	uint32_t hold_ms;       /* acquisition duration */
	uint32_t max_gap_ms;
	uint8_t  min_samples;
};

struct mc_gate {
	struct mc_gate_cfg cfg;

	bool     seen_any;       /* any sample since reset */
	bool     acquiring;      /* an acquisition is running */
	bool     latched;        /* READY has been reached and not yet lost */
	uint32_t acq_start;      /* time of the first accepted sample */
	uint32_t last_sample;    /* time of the most recent sample (any rssi) */
	int8_t   last_rssi;
	uint16_t acq_samples;    /* accepted samples in this acquisition */

	int8_t   ring[MC_GATE_RING];
	uint8_t  ring_n;         /* valid entries */
	uint8_t  ring_pos;
};

enum mc_gate_verdict {
	MC_GATE_READY = 0,
	MC_GATE_NOT_SEEN,      /* nothing heard from the target since arming */
	MC_GATE_STALE,         /* heard, but not within max_gap_ms of now */
	MC_GATE_BELOW_FLOOR,   /* latest sample under the hard floor */
	MC_GATE_ACQUIRING,     /* not yet hold_ms into a clean acquisition */
	MC_GATE_FEW_SAMPLES,   /* held long enough on too few advertisements */
	MC_GATE_WEAK_AVG,      /* rolling average below the threshold */
};

void mc_gate_init(struct mc_gate *g, const struct mc_gate_cfg *cfg);
void mc_gate_reset(struct mc_gate *g);

/* One advertisement from the target. */
void mc_gate_sample(struct mc_gate *g, uint32_t now_ms, int8_t rssi);

/* Verdict at `now_ms`. Also updates the latch, so call it on the same cadence
 * as samples and on every START. */
enum mc_gate_verdict mc_gate_check(struct mc_gate *g, uint32_t now_ms);

static inline bool mc_gate_ready(struct mc_gate *g, uint32_t now_ms)
{
	return mc_gate_check(g, now_ms) == MC_GATE_READY;
}

/* Rolling statistics over the ring (0 / 0 when empty). */
int8_t  mc_gate_avg(const struct mc_gate *g);
int8_t  mc_gate_min(const struct mc_gate *g);

/* Milliseconds into the current acquisition (0 when none). */
uint32_t mc_gate_held_ms(const struct mc_gate *g, uint32_t now_ms);

const char *mc_gate_verdict_str(enum mc_gate_verdict v);

#ifdef __cplusplus
}
#endif
