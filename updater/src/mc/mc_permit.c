#include "mc_permit.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* Uptime in ms at which the permit expires, plus one so that zero can mean
 * "none held" even at the instant of boot. */
static atomic_t s_deadline;

void mc_permit_grant(void)
{
	atomic_set(&s_deadline, (atomic_val_t)(k_uptime_get_32() + MC_PERMIT_TTL_MS + 1u));
}

bool mc_permit_consume(void)
{
	atomic_val_t d = atomic_set(&s_deadline, 0);

	if (d == 0) {
		return false;
	}
	/* Signed difference: correct across the 32-bit uptime wrap. */
	return (int32_t)((uint32_t)d - 1u - k_uptime_get_32()) >= 0;
}

void mc_permit_revoke(void)
{
	atomic_set(&s_deadline, 0);
}
