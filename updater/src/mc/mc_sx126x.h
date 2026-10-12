#pragma once

/*
 * SX126x arithmetic that has nothing to do with SPI: bandwidth codes, the
 * frequency word, low-data-rate optimisation, time on air, parameter blocks and
 * packet-status decoding. Pure functions so the host tests can pin them to the
 * datasheet; mc_radio.c (Zephyr, SPI) is the only caller.
 *
 * Why a raw driver at all: Zephyr's LoRa API takes enum lora_signal_bandwidth,
 * which stops at 125/250/500 kHz. MeshCore's US preset is 62.5 kHz, and there
 * is no way to say that through the API.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bandwidth in Hz -> SetModulationParams code, or -1 if the chip has none. */
int mc_sx_bw_code(uint32_t bw_hz);

/* RF frequency word for SetRfFrequency: hz * 2^25 / 32 MHz. */
uint32_t mc_sx_freq_word(uint32_t hz);

/* Low data-rate optimisation: required when the symbol time exceeds 16.38 ms. */
bool mc_sx_ldro(uint8_t sf, uint32_t bw_hz);

/* Time on air in milliseconds, rounded up. cr is the denominator 5..8
 * (4/5..4/8). Explicit header, CRC on. */
uint32_t mc_sx_airtime_ms(uint8_t sf, uint32_t bw_hz, uint8_t cr,
			  uint16_t preamble, uint8_t payload_len);

/* SetModulationParams block (4 bytes). False on an unsupported setting. */
bool mc_sx_modparams(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t out[4]);

/* SetPacketParams block (6 bytes), explicit header, CRC on, standard IQ. */
void mc_sx_pktparams(uint16_t preamble, uint8_t payload_len, uint8_t out[6]);

/* One-byte LoRa sync word (0x12 private, 0x34 public) -> the two register
 * bytes written at 0x0740 / 0x0741. */
void mc_sx_syncword_regs(uint8_t sync, uint8_t out[2]);

/* CalibrateImage band bytes for a carrier frequency (datasheet 9.2.1). */
void mc_sx_image_cal(uint32_t hz, uint8_t out[2]);

/* GetPacketStatus bytes -> dBm * 1 (RSSI) and dB * 4 (SNR). */
int mc_sx_rssi_dbm(uint8_t raw_rssi);
int mc_sx_snr_q2(uint8_t raw_snr);

#ifdef __cplusplus
}
#endif
