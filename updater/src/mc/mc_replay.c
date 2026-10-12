#include "mc_replay.h"

void mc_replay_init(struct mc_replay *r, uint32_t persisted_newest_ts)
{
	r->newest_ts = persisted_newest_ts;
	r->used = 0;
	r->next = 0;
	for (unsigned i = 0; i < MC_REPLAY_RING; i++) {
		r->digest[i] = 0;
	}
}

uint32_t mc_replay_digest(const void *data, unsigned len)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261u;

	for (unsigned i = 0; i < len; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

enum mc_replay_verdict mc_replay_accept(struct mc_replay *r, uint32_t ts,
					uint32_t digest)
{
	if (ts < r->newest_ts) {
		return MC_REPLAY_STALE;
	}
	for (unsigned i = 0; i < r->used; i++) {
		if (r->digest[i] == digest) {
			return MC_REPLAY_DUPLICATE;
		}
	}
	r->newest_ts = ts;
	r->digest[r->next] = digest;
	r->next = (uint8_t)((r->next + 1) % MC_REPLAY_RING);
	if (r->used < MC_REPLAY_RING) {
		r->used++;
	}
	return MC_REPLAY_OK;
}
