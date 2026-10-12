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

#ifdef __cplusplus
extern "C" {
#endif

/* Start the MeshCore interface. Non-blocking: identity generation (first boot)
 * and radio bring-up happen on the mc thread. Safe to call once, after the
 * Bluetooth stack is up. Returns 0, or a negative errno if the thread could not
 * start. A radio or storage fault is logged and leaves the interface inert; it
 * never affects the BLE side. */
int mc_init(void);

#ifdef __cplusplus
}
#endif
