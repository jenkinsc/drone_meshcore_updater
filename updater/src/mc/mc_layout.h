#pragma once

/*
 * Flash-layout arithmetic shared by the compile-time checks in mc_zephyr.c and
 * the host tests, so "these two ranges must not overlap" is one definition that
 * both the build and the test suite evaluate.
 *
 * The generated map for the XIAO nRF52840 build (devicetree, not folklore;
 * `./build-meshcore.sh` prints it again via tools/mc_layout_report.py):
 *
 *   0x000000-0x026FFF  SoftDevice region (MBR + S140; read-only, unused by us)
 *   0x027000-0x030FFF  mcuboot            (40 KB)
 *   0x031000-0x091FFF  slot0 / image-0    (388 KB)   MCUboot trailer at its end
 *   0x092000-0x0F2FFF  slot1 / image-1    (388 KB)   MCUboot trailer at its end
 *   0x0F3000-0x0F3FFF  mc-identity        (4 KB)     <- this layer's only flash
 *   0x0F4000-0x0FFFFF  uf2 bootloader     (48 KB; read-only; holds the
 *                                          bootloader settings + MBR params)
 *
 * The external 2 MB QSPI part (littlefs: bundles, config, logs) is a different
 * device and shares no addresses with any of the above.
 */

#include <stdint.h>

#define MC_RANGES_OVERLAP(a_off, a_len, b_off, b_len) \
	(((uint64_t)(a_off) < (uint64_t)(b_off) + (b_len)) && \
	 ((uint64_t)(b_off) < (uint64_t)(a_off) + (a_len)))

#define MC_RANGE_INSIDE(off, len, whole_len) \
	(((uint64_t)(off) + (len)) <= (uint64_t)(whole_len))
