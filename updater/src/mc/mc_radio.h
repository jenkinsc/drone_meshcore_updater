#pragma once

/*
 * Raw SX1262 driver for the Seeed Wio-SX1262 on the XIAO nRF52840.
 *
 * Zephyr: SPI + GPIO only. All arithmetic is in mc_sx126x.c (host-tested).
 * Everything below is called from the single mc thread; there is no locking
 * because there is no second caller.
 *
 * NOT TESTED ON HARDWARE. Written from the SX1261/2 datasheet (rev 2.1) and
 * cross-checked against how RadioLib drives the same chip for MeshCore.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mc_radio_rx {
	uint8_t data[255];
	size_t  len;
	int     rssi_dbm;
	int     snr_q2;      /* dB * 4 */
};

/* `irq_notify` runs in interrupt context when DIO1 fires; it must only post to
 * a queue. */
int mc_radio_init(void (*irq_notify)(void));

/* Service a DIO1 event: reads the IRQ flags and, if a good packet arrived,
 * fills `rx` and returns 1. Returns 0 if there was nothing to deliver. */
int mc_radio_service(struct mc_radio_rx *rx);

/* Send one frame and go back to receive. Blocking: roughly the time on air.
 * Waits for a quiet channel first (bounded), then sends regardless. */
int mc_radio_tx(const uint8_t *frame, size_t len);

/* Bring-up counters. Updated only on the mc thread (the DIO1 count aside, a
 * word written from the ISR); read by the lab `radio` command and boot log. */
struct mc_radio_stats {
	uint32_t rx_ok, rx_crc_err, rx_hdr_err, rx_other;
	uint32_t tx_ok, tx_fail, tx_timeout;
	uint32_t busy_timeouts, spi_faults, rx_restore_fail, dio1_irqs;
	int      last_rssi_dbm, last_snr_q2;
	bool     have_rx;
	bool     up;                 /* mc_radio_init() completed */
};
const struct mc_radio_stats *mc_radio_stats_get(void);

/* Raw logical pin states (1 = asserted) for the boot log: BUSY, DIO1, RESET. */
void mc_radio_pins(int *busy, int *dio1, int *reset);

/* SPI controller and GPIO ports report ready (false = devicetree/pinctrl). */
bool mc_radio_hw_ready(void);

#ifdef __cplusplus
}
#endif
