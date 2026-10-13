#include "mc_progress.h"

static const uint8_t MILESTONES[] = { 25, 50, 75 };
#define N_MILESTONES (sizeof(MILESTONES) / sizeof(MILESTONES[0]))

void mc_progress_begin(struct mc_progress *p)
{
	p->active         = true;
	p->next_milestone = 0;
	p->verifying_sent = false;
	p->terminal_sent  = false;
}

void mc_progress_end(struct mc_progress *p)
{
	p->active = false;
}

/* The transfer is complete once the run is past UPLOADING. */
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

	int pct = -1;

	if (upload_finished(ev)) {
		pct = 100;
	} else if (ev->state == DFU_STATUS_UPLOADING) {
		pct = ev->percent;
	}

	/* Highest newly crossed milestone only. */
	int top = -1;

	while (pct >= 0 && p->next_milestone < N_MILESTONES &&
	       pct >= MILESTONES[p->next_milestone]) {
		top = p->next_milestone;
		p->next_milestone++;
	}
	if (top >= 0) {
		out[n].kind = MC_MSG_MILESTONE;
		out[n].milestone = MILESTONES[top];
		out[n].result = DFU_STATUS_RESULT_NONE;
		n++;
	}

	if (ev->state == DFU_STATUS_VERIFYING && !p->verifying_sent) {
		p->verifying_sent = true;
		out[n].kind = MC_MSG_VERIFYING;
		out[n].milestone = 0;
		out[n].result = DFU_STATUS_RESULT_NONE;
		n++;
	}

	if (ev->state == DFU_STATUS_DONE || ev->state == DFU_STATUS_FAILED) {
		out[n].kind = (ev->state == DFU_STATUS_DONE) ? MC_MSG_DONE : MC_MSG_FAILED;
		out[n].milestone = 0;
		out[n].result = (enum dfu_status_result)ev->result;
		n++;
		p->terminal_sent = true;
	}
	return n;
}
