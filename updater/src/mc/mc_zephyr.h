#pragma once

/*
 * MeshCore command interface: the Zephyr glue.
 *
 * Everything with a decision in it is in the portable modules next to this
 * file and is host-tested; this file only connects them to the board: a
 * thread, a queue, the SX1262, the identity page, the BLE scanner and the DFU
 * runner.
 *
 * Threading: one thread ("mc") owns the radio, the controller, the node and the
 * store. Other threads only post events (advert samples from the BLE scan
 * callback, DFU status changes, the DIO1 interrupt) and never touch that state.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the MeshCore interface. Non-blocking: identity generation (first boot)
 * and radio bring-up happen on the mc thread. Safe to call once, after the
 * Bluetooth stack is up. Returns 0, or a negative errno if the thread could not
 * start. A radio or storage fault is logged and leaves the interface inert; it
 * never affects the BLE side. */
int mc_init(void);

/* The DFU runner's gate: consume the start authorisation for this exact path
 * and pin, from the thread it was granted to. False = refuse the start. */
bool mc_authz_dfu_begin(const char *path, const char *pin);

/* Called from main.c with the reset cause it read (and cleared) at boot. */
void mc_note_reset_cause(uint32_t cause);

/* True if `path` (a file-system path from any non-MeshCore entry point: SMP
 * fs group, the fsx writer) would modify the bundle that is armed. */
bool mc_fs_mutation_blocked(const char *path);

#ifdef __cplusplus
}
#endif
