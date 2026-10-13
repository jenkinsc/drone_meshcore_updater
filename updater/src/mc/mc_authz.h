#pragma once

/*
 * The DFU start authorisation: one strict state transition, no timer.
 *
 *            grant()                      consume()                  end()
 *   IDLE ───────────────► START_AUTHORIZED ───────────► DFU_ACTIVE ───────► IDLE
 *     ▲                        │ end() (start refused)                        │
 *     └────────────────────────┴──────────────────────────────────────────────┘
 *
 * This replaces the earlier "permit" that lapsed after two seconds. A time
 * window is a window for something else to walk through; a state is not.
 *
 *   - grant() succeeds only from IDLE, and records WHO is allowed to consume
 *     it (the calling thread) and WHAT it is for (the exact bundle path and
 *     the exact pin string). It is called by the MeshCore controller as the
 *     last step of an accepted START, immediately before it calls the DFU
 *     runner on the same thread.
 *   - consume() is called by dfu_runner_start(). It succeeds only if the state
 *     is START_AUTHORIZED, the caller is the thread named in the grant, and the
 *     path and pin it was handed are byte-identical to the granted ones — and
 *     then moves to DFU_ACTIVE with a single atomic compare-and-swap. A second
 *     consume() — from any thread, with any arguments — finds DFU_ACTIVE and
 *     fails. There is nothing left to reuse.
 *   - A caller that is not the granted thread (the SMP TRIGGER_DFU handler, the
 *     auto_flash block in main) never changes the state on failure: it cannot
 *     steal the authorisation and it cannot cancel it.
 *   - end() returns to IDLE. The controller calls it when the run finishes,
 *     fails, is aborted, or when the runner refused to start.
 *
 * Portable on purpose: `who` is an opaque identity (a thread pointer on the
 * device, a number in the host tests) so the whole rule runs under test.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_AUTHZ_PATH_MAX 96
#define MC_AUTHZ_PIN_MAX  40

enum mc_authz_state {
	MC_AZ_IDLE = 0,
	MC_AZ_GRANTING,           /* transient: fields being written */
	MC_AZ_START_AUTHORIZED,
	MC_AZ_DFU_ACTIVE,
};

struct mc_authz {
	uint32_t  state;          /* enum mc_authz_state; accessed atomically */
	uintptr_t who;
	char      path[MC_AUTHZ_PATH_MAX];
	char      pin[MC_AUTHZ_PIN_MAX];
};

void mc_authz_init(struct mc_authz *a);

/* IDLE -> START_AUTHORIZED. False if not IDLE or the strings do not fit. */
bool mc_authz_grant(struct mc_authz *a, uintptr_t who, const char *path,
		    const char *pin);

/* START_AUTHORIZED -> DFU_ACTIVE, atomically, iff `who`, `path`, `pin` match. */
bool mc_authz_consume(struct mc_authz *a, uintptr_t who, const char *path,
		      const char *pin);

/* Any state -> IDLE. */
void mc_authz_end(struct mc_authz *a);

enum mc_authz_state mc_authz_state(const struct mc_authz *a);

#ifdef __cplusplus
}
#endif
