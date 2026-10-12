#pragma once

/*
 * Replay protection for commands.
 *
 * MeshCore floods every packet through repeaters, so the same command arrives
 * more than once, and anyone who recorded one can replay it later. Both need
 * the same answer: act once.
 *
 * Two checks, both required:
 *
 *   1. Timestamp. Each message carries the sender's clock in seconds. Accept
 *      only timestamps >= the newest already accepted. (Equal, not strictly
 *      greater: a phone can send two commands inside one second and refusing
 *      the second would make "arm" followed by "start" flaky.)
 *   2. Digest ring. A message whose digest is among the last few accepted is a
 *      duplicate and is dropped, which is what stops the equal-timestamp case
 *      from becoming a replay hole.
 *
 * The newest timestamp is meant to be persisted by the caller (see mc_store)
 * so that a reboot does not reopen the window: after a power cycle, anything
 * older than the last accepted command stays refused. Only the digest ring is
 * RAM-only, which leaves exactly one residual replay — re-sending the very last
 * accepted message after a reboot — and even that has to pass the arm/gate
 * state machine to do anything.
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
	uint32_t newest_ts;
	uint32_t digest[MC_REPLAY_RING];
	uint8_t  used;
	uint8_t  next;
};

enum mc_replay_verdict {
	MC_REPLAY_OK = 0,
	MC_REPLAY_STALE,       /* timestamp older than one already accepted */
	MC_REPLAY_DUPLICATE,   /* same message seen recently */
};

void mc_replay_init(struct mc_replay *r, uint32_t persisted_newest_ts);

/* Check and, if acceptable, record. `digest` is any 32-bit digest of the whole
 * message (timestamp included). Returns the verdict; state changes only on OK. */
enum mc_replay_verdict mc_replay_accept(struct mc_replay *r, uint32_t ts,
					uint32_t digest);

/* FNV-1a over `data`, for callers that need a digest. Not cryptographic; the
 * packets it is applied to have already passed their MAC. */
uint32_t mc_replay_digest(const void *data, unsigned len);

#ifdef __cplusplus
}
#endif
