#include "mc_authz.h"

#include <string.h>

void mc_authz_init(struct mc_authz *a)
{
	memset(a, 0, sizeof(*a));
}

static bool cas(uint32_t *p, uint32_t from, uint32_t to)
{
	return __atomic_compare_exchange_n(p, &from, to, false, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE);
}

bool mc_authz_grant(struct mc_authz *a, uintptr_t who, const char *path,
		    const char *pin)
{
	if (strlen(path) >= MC_AUTHZ_PATH_MAX || strlen(pin) >= MC_AUTHZ_PIN_MAX) {
		return false;
	}
	if (!cas(&a->state, MC_AZ_IDLE, MC_AZ_GRANTING)) {
		return false;
	}
	a->who = who;
	strcpy(a->path, path);
	strcpy(a->pin, pin);
	__atomic_store_n(&a->state, MC_AZ_START_AUTHORIZED, __ATOMIC_RELEASE);
	return true;
}

bool mc_authz_consume(struct mc_authz *a, uintptr_t who, const char *path,
		      const char *pin)
{
	if (__atomic_load_n(&a->state, __ATOMIC_ACQUIRE) != MC_AZ_START_AUTHORIZED) {
		return false;
	}
	/* The fields are stable while the state is START_AUTHORIZED: only the
	 * granting thread writes them, and only through IDLE -> GRANTING. */
	if (a->who != who || path == NULL || pin == NULL ||
	    strcmp(a->path, path) != 0 || strcmp(a->pin, pin) != 0) {
		return false;                    /* leave the grant untouched */
	}
	return cas(&a->state, MC_AZ_START_AUTHORIZED, MC_AZ_DFU_ACTIVE);
}

void mc_authz_end(struct mc_authz *a)
{
	__atomic_store_n(&a->state, MC_AZ_IDLE, __ATOMIC_RELEASE);
}

enum mc_authz_state mc_authz_state(const struct mc_authz *a)
{
	return (enum mc_authz_state)__atomic_load_n(&a->state, __ATOMIC_ACQUIRE);
}
