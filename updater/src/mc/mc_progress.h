#pragma once

/*
 * Turns the DFU runner's status stream into the few messages worth sending
 * over a LoRa link.
 *
 * dfu_status publishes every state change and a progress tick several times a
 * second. Over MeshCore that would be hundreds of airtime-expensive messages,
 * most of them stale by the time they were delivered. What an operator on a
 * hilltop wants is the shape of the run: it started, it is a quarter, half,
 * three quarters of the way, the transfer finished, and how it ended.
 *
 * Messages, each emitted at most once per run:
 *
 *   MC_MSG_MILESTONE  0, 25, 50, 75, 100 (percent of the image streamed)
 *   MC_MSG_RETRY      a new attempt began (attempt number carried)
 *   MC_MSG_DONE       terminal success
 *   MC_MSG_FAILED     terminal failure (dfu_status_result carried)
 *
 * "0%" means the transfer began, i.e. the run reached UPLOADING; it is not sent
 * at run start, because for most of a run's early life there is no transfer to
 * be at 0% of. "100%" is sent when the whole image has been streamed, which is
 * either percent == 100 or the run moving past UPLOADING (progress ticks are
 * coalesced upstream, so the final tick can be skipped; reaching VALIDATING
 * proves it happened).
 *
 * If progress jumps over several milestones in one update, each skipped
 * milestone is still reported, in order, so a listener never sees 0 then 75.
 *
 * Milestones are monotonic across a run: a retry restarts the byte counter but
 * does not re-announce 25% the operator has already been told about. The retry
 * message says that it happened.
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
	MC_MSG_RETRY,
	MC_MSG_DONE,
	MC_MSG_FAILED,
};

struct mc_progress_msg {
	enum mc_msg_kind kind;
	uint8_t  milestone;   /* MILESTONE: 0/25/50/75/100 */
	uint8_t  attempt;     /* RETRY: new attempt, 1-based */
	uint8_t  retries;     /* RETRY: attempt budget */
	enum dfu_status_result result;   /* FAILED (and DONE: always OK) */
};

/* At most five milestones plus one retry plus one terminal in a single update. */
#define MC_PROGRESS_MAX_MSGS 8

struct mc_progress {
	bool    active;
	uint8_t next_milestone;   /* index into {0,25,50,75,100}; 5 = all sent */
	uint8_t last_attempt;
	bool    terminal_sent;
};

/* Begin a run. Call when START is accepted. */
void mc_progress_begin(struct mc_progress *p);

/* Stop tracking without a terminal message (operator abort, or the runner was
 * reset). Further updates are ignored until the next begin. */
void mc_progress_end(struct mc_progress *p);

/* Feed one dfu_status event; returns how many messages were written to `out`
 * (up to MC_PROGRESS_MAX_MSGS). */
size_t mc_progress_update(struct mc_progress *p,
			  const struct dfu_status_event *ev,
			  struct mc_progress_msg *out);

#ifdef __cplusplus
}
#endif
