#include "mc_sx126x.h"

int mc_sx_bw_code(uint32_t bw_hz)
{
	switch (bw_hz) {
	case 7810:   return 0x00;
	case 10420:  return 0x08;
	case 15630:  return 0x01;
	case 20830:  return 0x09;
	case 31250:  return 0x02;
	case 41670:  return 0x0A;
	case 62500:  return 0x03;
	case 125000: return 0x04;
	case 250000: return 0x05;
	case 500000: return 0x06;
	default:     return -1;
	}
}

uint32_t mc_sx_freq_word(uint32_t hz)
{
	return (uint32_t)(((uint64_t)hz << 25) / 32000000u);
}

bool mc_sx_ldro(uint8_t sf, uint32_t bw_hz)
{
	/* symbol time = 2^sf / bw > 16.38 ms  <=>  2^sf * 1000 > 16.38 * bw */
	uint64_t lhs = ((uint64_t)1u << sf) * 1000000u;       /* us * Hz */
	uint64_t rhs = 16380ull * bw_hz;

	return lhs > rhs;
}

uint32_t mc_sx_airtime_ms(uint8_t sf, uint32_t bw_hz, uint8_t cr,
			  uint16_t preamble, uint8_t payload_len)
{
	if (sf < 5 || sf > 12 || bw_hz == 0 || cr < 5 || cr > 8) {
		return 0;
	}
	/* Datasheet 6.1.4, explicit header, CRC on:
	 * n = 8 + max(ceil((8*PL - 4*SF + 28 + 16) / (4*(SF - 2*DE))) * CR, 0) */
	int de = mc_sx_ldro(sf, bw_hz) ? 1 : 0;
	int num = 8 * (int)payload_len - 4 * (int)sf + 28 + 16;
	int den = 4 * ((int)sf - 2 * de);
	int blocks = num > 0 ? (num + den - 1) / den : 0;
	uint32_t payload_syms = 8u + (uint32_t)blocks * cr;

	/* Preamble is (n + 4.25) symbols: work in quarter-symbols. */
	uint64_t quarter_syms = (uint64_t)(preamble + payload_syms) * 4u + 17u;
	/* symbol time in us = 2^sf * 1e6 / bw ; total us = quarter_syms/4 * that */
	uint64_t us = quarter_syms * (((uint64_t)1u << sf) * 1000000u / bw_hz) / 4u;

	/* The division above truncates the symbol time, which at 62.5 kHz / SF7 is
	 * exact (2048 us) but not in general; the error is below a microsecond per
	 * symbol and the result is rounded up to a millisecond anyway. */
	return (uint32_t)((us + 999u) / 1000u);
}

bool mc_sx_modparams(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t out[4])
{
	int bw = mc_sx_bw_code(bw_hz);

	if (bw < 0 || sf < 5 || sf > 12 || cr < 5 || cr > 8) {
		return false;
	}
	out[0] = sf;
	out[1] = (uint8_t)bw;
	out[2] = (uint8_t)(cr - 4);
	out[3] = mc_sx_ldro(sf, bw_hz) ? 1 : 0;
	return true;
}

void mc_sx_pktparams(uint16_t preamble, uint8_t payload_len, uint8_t out[6])
{
	out[0] = (uint8_t)(preamble >> 8);
	out[1] = (uint8_t)preamble;
	out[2] = 0;           /* explicit header */
	out[3] = payload_len;
	out[4] = 1;           /* CRC on */
	out[5] = 0;           /* standard IQ */
}

void mc_sx_syncword_regs(uint8_t sync, uint8_t out[2])
{
	out[0] = (uint8_t)((sync & 0xF0) | 0x04);
	out[1] = (uint8_t)(((sync & 0x0F) << 4) | 0x04);
}

int mc_sx_rssi_dbm(uint8_t raw_rssi)
{
	return -(int)raw_rssi / 2;
}

int mc_sx_snr_q2(uint8_t raw_snr)
{
	return (int8_t)raw_snr;     /* the register is already dB * 4 */
}

void mc_sx_image_cal(uint32_t hz, uint8_t out[2])
{
	if (hz >= 902000000u) {
		out[0] = 0xE1; out[1] = 0xE9;
	} else if (hz >= 863000000u) {
		out[0] = 0xD7; out[1] = 0xDB;
	} else if (hz >= 779000000u) {
		out[0] = 0xC1; out[1] = 0xC5;
	} else if (hz >= 470000000u) {
		out[0] = 0x75; out[1] = 0x81;
	} else {
		out[0] = 0x6B; out[1] = 0x6F;
	}
}
