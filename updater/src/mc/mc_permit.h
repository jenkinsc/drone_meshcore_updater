#pragma once

/*
 * One-shot permission to start a DFU.
 *
 * This is the enforcement half of "do not begin DFU automatically". The
 * updater has three ways into dfu_runner_start(): the auto_flash flag at boot,
 * a TRIGGER_DFU write over SMP from any BLE client, and the MeshCore START
 * command. With CONFIG_MC_CMD_ENFORCE=y the runner refuses all of them unless
 * a permit is held, and only the MeshCore controller — after it has checked the
 * sender's key, the armed target and the RSSI gate — ever grants one.
 *
 * A permit is single use and short-lived (MC_PERMIT_TTL_MS). Single use so one
 * START cannot authorise two runs; short-lived so a permit granted and then not
 * consumed (the start call failed) cannot be picked up later by a different
 * caller. Granting and consuming happen a few instructions apart in the same
 * START handler, so the TTL is generous.
 *
 * Thread-safe: one atomic word.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_PERMIT_TTL_MS 2000

void mc_permit_grant(void);

/* True exactly once per grant, and only within the TTL. */
bool mc_permit_consume(void);

/* Drop an unconsumed permit. */
void mc_permit_revoke(void);

#ifdef __cplusplus
}
#endif
