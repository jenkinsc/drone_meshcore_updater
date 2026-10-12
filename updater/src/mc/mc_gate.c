#include "mc_gate.h"

void mc_gate_init(struct mc_gate *g, int8_t min_rssi, uint32_t hold_ms,
		  uint32_t max_gap_ms, uint8_t min_samples)
{
	g->min_rssi    = min_rssi;
	g->hold_ms     = hold_ms;
	g->max_gap_ms  = max_gap_ms;
	g->min_samples = min_samples;
	mc_gate_reset(g);
}

void mc_gate_reset(struct mc_gate *g)
{
	g->seen_any       = false;
	g->window_open    = false;
	g->window_start   = 0;
	g->last_sample    = 0;
	g->last_rssi      = -127;
	g->window_samples = 0;
	g->window_min     = 0;
}

void mc_gate_sample(struct mc_gate *g, uint32_t now_ms, int8_t rssi)
{
	bool gap = g->seen_any && (uint32_t)(now_ms - g->last_sample) > g->max_gap_ms;

	g->seen_any    = true;
	g->last_sample = now_ms;
	g->last_rssi   = rssi;

	if (rssi < g->min_rssi) {
		/* A weak sample ends the window outright. */
		g->window_open    = false;
		g->window_samples = 0;
		return;
	}

	if (!g->window_open || gap) {
		g->window_open    = true;
		g->window_start   = now_ms;
		g->window_samples = 1;
		g->window_min     = rssi;
		return;
	}

	if (g->window_samples < UINT16_MAX) {
		g->window_samples++;
	}
	if (rssi < g->window_min) {
		g->window_min = rssi;
	}
}

enum mc_gate_verdict mc_gate_check(const struct mc_gate *g, uint32_t now_ms)
{
	if (!g->seen_any) {
		return MC_GATE_NOT_SEEN;
	}
	if ((uint32_t)(now_ms - g->last_sample) > g->max_gap_ms) {
		return MC_GATE_STALE;
	}
	if (!g->window_open) {
		return MC_GATE_TOO_WEAK;
	}
	if ((uint32_t)(now_ms - g->window_start) < g->hold_ms) {
		return MC_GATE_SETTLING;
	}
	if (g->window_samples < g->min_samples) {
		return MC_GATE_FEW_SAMPLES;
	}
	return MC_GATE_READY;
}

uint32_t mc_gate_held_ms(const struct mc_gate *g, uint32_t now_ms)
{
	if (!g->window_open) {
		return 0;
	}
	return (uint32_t)(now_ms - g->window_start);
}

const char *mc_gate_verdict_str(enum mc_gate_verdict v)
{
	switch (v) {
	case MC_GATE_READY:       return "ready";
	case MC_GATE_NOT_SEEN:    return "target not seen";
	case MC_GATE_STALE:       return "target went quiet";
	case MC_GATE_TOO_WEAK:    return "rssi below threshold";
	case MC_GATE_SETTLING:    return "rssi not stable long enough";
	case MC_GATE_FEW_SAMPLES: return "too few adverts in window";
	}
	return "?";
}
