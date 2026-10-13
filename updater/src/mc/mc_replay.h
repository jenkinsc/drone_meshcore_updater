#pragma once

/*
 * Duplicate suppression for commands.
 *
 * MeshCore floods every packet through repeaters, so the same command can
 * arrive more than once within seconds. A short ring of recent message digests
 * makes the node act once and merely re-acknowledge the repeat.
 *
 * This is *not* replay protection. Replay of a recorded command is stopped by
 * the single-use token on arm/start (mc_ctl.h) and by the commands that need no
 * token being harmless to repeat. There is deliberately no timestamp ordering
 * and nothing persisted: ordering by the sender's clock meant one wrong clock
 * could lock the operator out until the node was reflashed. A reboot clears the
 * ring, which costs nothing, because the token does not survive one either.
 *
 * Pure logic; compiled by the host tests.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_REPLAY_RING 8

struct mc_replay {
	uint32_t digest[MC_REPLAY_RING];
	uint8_t  used;
	uint8_t  next;
};

enum mc_replay_verdict {
	MC_REPLAY_OK = 0,
	MC_REPLAY_DUPLICATE,   /* same message seen recently */
};

void mc_replay_init(struct mc_replay *r);

enum mc_replay_verdict mc_replay_accept(struct mc_replay *r, uint32_t digest);

uint32_t mc_replay_digest(const void *data, unsigned len);

#ifdef __cplusplus
}
#endif
