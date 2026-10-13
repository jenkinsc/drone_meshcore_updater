#include "mc_gate.h"

void mc_gate_init(struct mc_gate *g, const struct mc_gate_cfg *cfg)
{
	g->cfg = *cfg;
	mc_gate_reset(g);
}

void mc_gate_reset(struct mc_gate *g)
{
	g->seen_any    = false;
	g->acquiring   = false;
	g->latched     = false;
	g->acq_start   = 0;
	g->last_sample = 0;
	g->last_rssi   = -127;
	g->acq_samples = 0;
	g->ring_n      = 0;
	g->ring_pos    = 0;
}

static void restart(struct mc_gate *g)
{
	g->acquiring   = false;
	g->latched     = false;
	g->acq_samples = 0;
	g->ring_n      = 0;
	g->ring_pos    = 0;
}

void mc_gate_sample(struct mc_gate *g, uint32_t now_ms, int8_t rssi)
{
	bool gap = g->seen_any && (uint32_t)(now_ms - g->last_sample) > g->cfg.max_gap_ms;

	g->seen_any    = true;
	g->last_sample = now_ms;
	g->last_rssi   = rssi;

	if (gap) {
		restart(g);
	}
	if (rssi < g->cfg.floor) {
		/* The hard floor: nothing averages this away. */
		restart(g);
		return;
	}
	if (!g->acquiring) {
		g->acquiring = true;
		g->acq_start = now_ms;
	}
	if (g->acq_samples < UINT16_MAX) {
		g->acq_samples++;
	}
	g->ring[g->ring_pos] = rssi;
	g->ring_pos = (uint8_t)((g->ring_pos + 1) % MC_GATE_RING);
	if (g->ring_n < MC_GATE_RING) {
		g->ring_n++;
	}
}

int8_t mc_gate_avg(const struct mc_gate *g)
{
	int32_t sum = 0;

	if (g->ring_n == 0) {
		return 0;
	}
	for (uint8_t i = 0; i < g->ring_n; i++) {
		sum += g->ring[i];
	}
	/* Round to nearest, symmetric about zero. */
	int32_t n = g->ring_n;

	return (int8_t)((sum - (n / 2)) / n);
}

int8_t mc_gate_min(const struct mc_gate *g)
{
	int8_t m = 0;

	for (uint8_t i = 0; i < g->ring_n; i++) {
		if (i == 0 || g->ring[i] < m) {
			m = g->ring[i];
		}
	}
	return m;
}

enum mc_gate_verdict mc_gate_check(struct mc_gate *g, uint32_t now_ms)
{
	if (!g->seen_any) {
		return MC_GATE_NOT_SEEN;
	}
	if ((uint32_t)(now_ms - g->last_sample) > g->cfg.max_gap_ms) {
		restart(g);
		return MC_GATE_STALE;
	}
	if (g->last_rssi < g->cfg.floor) {
		return MC_GATE_BELOW_FLOOR;
	}
	if (!g->acquiring) {
		return MC_GATE_ACQUIRING;
	}

	int avg = mc_gate_avg(g);

	if (g->latched) {
		/* Hysteresis: stay ready until the mean is clearly gone. */
		if (avg < g->cfg.avg_min - (int)g->cfg.hyst_db) {
			g->latched = false;
			return MC_GATE_WEAK_AVG;
		}
		return MC_GATE_READY;
	}

	if ((uint32_t)(now_ms - g->acq_start) < g->cfg.hold_ms) {
		return MC_GATE_ACQUIRING;
	}
	if (g->acq_samples < g->cfg.min_samples) {
		return MC_GATE_FEW_SAMPLES;
	}
	if (avg < g->cfg.avg_min) {
		return MC_GATE_WEAK_AVG;
	}
	g->latched = true;
	return MC_GATE_READY;
}

uint32_t mc_gate_held_ms(const struct mc_gate *g, uint32_t now_ms)
{
	if (!g->acquiring) {
		return 0;
	}
	return (uint32_t)(now_ms - g->acq_start);
}

const char *mc_gate_verdict_str(enum mc_gate_verdict v)
{
	switch (v) {
	case MC_GATE_READY:       return "ready";
	case MC_GATE_NOT_SEEN:    return "target not seen";
	case MC_GATE_STALE:       return "target went quiet";
	case MC_GATE_BELOW_FLOOR: return "rssi under floor";
	case MC_GATE_ACQUIRING:   return "acquiring";
	case MC_GATE_FEW_SAMPLES: return "too few adverts";
	case MC_GATE_WEAK_AVG:    return "avg too weak";
	}
	return "?";
}
