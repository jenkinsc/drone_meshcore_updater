#pragma once

/*
 * Turns the DFU runner's status stream into the few messages worth sending
 * over a LoRa link.
 *
 * dfu_status publishes every state change and a progress tick several times a
 * second. Over MeshCore that would be hundreds of airtime-expensive messages,
 * most of them stale by the time they were delivered, and every transmission
 * is time the node is not listening. What an operator wants is the shape of
 * the run, so during a DFU the only messages are:
 *
 *   START      the reply to the START command itself (not produced here)
 *   25%  50%  75%   of the image streamed
 *   VERIFYING  the transfer finished and the runner is checking that the
 *              peer took it (only if the transport has a verify step)
 *   SUCCESS or FAILURE   terminal, with the reason
 *
 * Each is emitted at most once per run. There is no 0% (START says the run
 * began), no 100% (VERIFYING or SUCCESS says the transfer finished) and no
 * retry message (retries go to the event log; `status` shows the attempt).
 *
 * If one update jumps over several milestones only the highest is emitted:
 * a listener told "50%" does not also need the stale "25%". Milestones are
 * monotonic across a run: a retry restarts the byte counter but does not
 * re-announce a quarter the operator already heard about.
 *
 * Pure logic; compiled by the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../dfu_status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mc_msg_kind {
	MC_MSG_MILESTONE = 1,
	MC_MSG_VERIFYING,
	MC_MSG_DONE,
	MC_MSG_FAILED,
};

struct mc_progress_msg {
	enum mc_msg_kind kind;
	uint8_t  milestone;   /* MILESTONE: 25 / 50 / 75 */
	enum dfu_status_result result;   /* FAILED (and DONE: always OK) */
};

/* One update can yield at most a milestone, VERIFYING and a terminal. */
#define MC_PROGRESS_MAX_MSGS 3

struct mc_progress {
	bool    active;
	uint8_t next_milestone;   /* index into {25,50,75}; 3 = all sent */
	bool    verifying_sent;
	bool    terminal_sent;
};

/* Begin a run. Call when START is accepted. */
void mc_progress_begin(struct mc_progress *p);

/* Stop tracking without a terminal message (operator abort, or the runner was
 * reset from elsewhere). */
void mc_progress_end(struct mc_progress *p);

/* Feed one dfu_status event; returns how many messages were written to `out`
 * (at most MC_PROGRESS_MAX_MSGS). */
size_t mc_progress_update(struct mc_progress *p,
			  const struct dfu_status_event *ev,
			  struct mc_progress_msg *out);

#ifdef __cplusplus
}
#endif
