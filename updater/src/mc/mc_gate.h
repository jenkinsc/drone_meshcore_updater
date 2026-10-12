#pragma once

/*
 * START gate: is the armed BLE target close enough, for long enough?
 *
 * Pure logic, no Zephyr: the host tests in updater/mc-tests compile this file
 * directly. The caller feeds it one sample per received advertisement from the
 * armed target and asks mc_gate_ready() when a START arrives.
 *
 * ---- The rule -----------------------------------------------------------
 *
 * START is allowed only while the target has been *continuously* heard at or
 * above `min_rssi` for at least `hold_ms`. In detail:
 *
 *   - every advertisement from the target is a sample;
 *   - a sample below `min_rssi` restarts the clock (it does not merely fail the
 *     moment — a link that dips and recovers is not stable);
 *   - silence longer than `max_gap_ms` restarts the clock too, because an
 *     advertiser that went quiet has told us nothing about the link in that
 *     window and the drone may have flown out and back;
 *   - at least `min_samples` samples must back the window, so one lucky strong
 *     advertisement followed by 5 s of nothing cannot pass;
 *   - readiness is evaluated at the moment of START, so a window that ended
 *     (last sample too old) is not ready even if it was long enough once.
 *
 * Strict by design. The threshold is the knob for margin; the gate itself has
 * no tolerance for outliers, so "stable" means what it says.
 *
 * Times are uint32_t milliseconds and wrap-safe (differences only).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_GATE_DEFAULT_HOLD_MS    5000u
#define MC_GATE_DEFAULT_MAX_GAP_MS 3000u
#define MC_GATE_DEFAULT_MIN_SAMPLES 3u

struct mc_gate {
	/* configuration */
	int8_t   min_rssi;
	uint32_t hold_ms;
	uint32_t max_gap_ms;
	uint8_t  min_samples;

	/* state */
	bool     seen_any;       /* any sample since reset */
	bool     window_open;    /* a qualifying window is running */
	uint32_t window_start;   /* time of the first sample in the window */
	uint32_t last_sample;    /* time of the most recent sample (any rssi) */
	int8_t   last_rssi;
	uint16_t window_samples;
	int8_t   window_min;     /* weakest sample in the current window */
};

enum mc_gate_verdict {
	MC_GATE_READY = 0,
	MC_GATE_NOT_SEEN,      /* nothing heard from the target since arming */
	MC_GATE_STALE,         /* heard, but not within max_gap_ms of now */
	MC_GATE_TOO_WEAK,      /* latest sample below min_rssi */
	MC_GATE_SETTLING,      /* strong enough, but not for hold_ms yet */
	MC_GATE_FEW_SAMPLES,   /* held long enough on too few advertisements */
};

void mc_gate_init(struct mc_gate *g, int8_t min_rssi, uint32_t hold_ms,
		  uint32_t max_gap_ms, uint8_t min_samples);

/* Forget everything heard. Called when the target changes or on arm. */
void mc_gate_reset(struct mc_gate *g);

/* One advertisement from the armed target. */
void mc_gate_sample(struct mc_gate *g, uint32_t now_ms, int8_t rssi);

/* Evaluate at `now_ms`. Does not modify the window. */
enum mc_gate_verdict mc_gate_check(const struct mc_gate *g, uint32_t now_ms);

static inline bool mc_gate_ready(const struct mc_gate *g, uint32_t now_ms)
{
	return mc_gate_check(g, now_ms) == MC_GATE_READY;
}

/* Milliseconds the current window has been qualifying as of `now_ms`, 0 when
 * none is open. For status output only; not used by the decision. */
uint32_t mc_gate_held_ms(const struct mc_gate *g, uint32_t now_ms);

const char *mc_gate_verdict_str(enum mc_gate_verdict v);

#ifdef __cplusplus
}
#endif
