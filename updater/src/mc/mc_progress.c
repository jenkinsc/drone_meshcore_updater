#include "mc_progress.h"

static const uint8_t MILESTONES[] = { 0, 25, 50, 75, 100 };
#define N_MILESTONES (sizeof(MILESTONES) / sizeof(MILESTONES[0]))

void mc_progress_begin(struct mc_progress *p)
{
	p->active         = true;
	p->next_milestone = 0;
	p->last_attempt   = 0;
	p->terminal_sent  = false;
}

void mc_progress_end(struct mc_progress *p)
{
	p->active = false;
}

/* True once the whole image has been streamed to the target. */
static bool upload_finished(const struct dfu_status_event *ev)
{
	switch (ev->state) {
	case DFU_STATUS_VALIDATING:
	case DFU_STATUS_DISCONNECTING:
	case DFU_STATUS_VERIFYING:
	case DFU_STATUS_DONE:
		return true;
	case DFU_STATUS_UPLOADING:
		return ev->percent >= 100;
	default:
		return false;
	}
}

size_t mc_progress_update(struct mc_progress *p,
			  const struct dfu_status_event *ev,
			  struct mc_progress_msg *out)
{
	size_t n = 0;

	if (!p->active || p->terminal_sent) {
		return 0;
	}

	/* A new attempt. The runner numbers them from 1; the first is not a
	 * "retry" worth a message. */
	if (ev->attempt > p->last_attempt) {
		if (p->last_attempt != 0 && n < MC_PROGRESS_MAX_MSGS) {
			out[n].kind      = MC_MSG_RETRY;
			out[n].attempt   = ev->attempt;
			out[n].retries   = ev->retries;
			out[n].milestone = 0;
			out[n].result    = DFU_STATUS_RESULT_NONE;
			n++;
		}
		p->last_attempt = ev->attempt;
	}

	/* How far the transfer has got, as a percentage, or -1 before it began. */
	int pct = -1;
	if (upload_finished(ev)) {
		pct = 100;
	} else if (ev->state == DFU_STATUS_UPLOADING) {
		pct = ev->percent;
	}

	while (pct >= 0 && p->next_milestone < N_MILESTONES &&
	       pct >= MILESTONES[p->next_milestone] && n < MC_PROGRESS_MAX_MSGS) {
		out[n].kind      = MC_MSG_MILESTONE;
		out[n].milestone = MILESTONES[p->next_milestone];
		out[n].attempt   = ev->attempt;
		out[n].retries   = ev->retries;
		out[n].result    = DFU_STATUS_RESULT_NONE;
		n++;
		p->next_milestone++;
	}

	if (ev->state == DFU_STATUS_DONE || ev->state == DFU_STATUS_FAILED) {
		if (n < MC_PROGRESS_MAX_MSGS) {
			out[n].kind      = (ev->state == DFU_STATUS_DONE) ? MC_MSG_DONE
								     : MC_MSG_FAILED;
			out[n].milestone = 0;
			out[n].attempt   = ev->attempt;
			out[n].retries   = ev->retries;
			out[n].result    = (enum dfu_status_result)ev->result;
			n++;
			p->terminal_sent = true;
		}
	}

	return n;
}
