#pragma once

/*
 * Target identity: which BLE advertisements count as "the armed device".
 *
 * Pure logic, no Zephyr. Two places use it with the same answer: the START gate
 * (is the thing I am measuring the thing I was told to measure?) and the DFU
 * runner's pinned scan (is the thing I am about to connect to that thing?).
 *
 * ---- The Nordic address behaviour this encodes -----------------------------
 *
 * A Nordic Legacy DFU peer that jumps from its application into the bootloader
 * (buttonless DFU) comes back advertising from the application's address
 * **plus one**: the last octet incremented, with carry across all six octets
 * (the OTAFIX / SDK bootloader derives it from the device address; the upstream
 * scanner implements the same rule in mac_match_or_plus_one()). The address
 * *type* does not change across the jump.
 *
 * So for an armed address A and type T, exactly two advertised identities are
 * the armed device:
 *
 *   EXACT      address == A, type == T.
 *              This is the application (or a device armed while already in DFU
 *              mode). Nothing else is required: it is the address the operator
 *              typed.
 *
 *   DFU_PLUS1  address == A + 1, type == T, AND
 *                - the advertisement carries the Nordic Legacy DFU service UUID
 *                  (00001530-1212-EFDE-1523-785FEABCD123) when
 *                  `require_dfu_uuid` is set, AND
 *                - the advertised name, *if this packet carries one*, equals
 *                  `dfu_name` when one is configured (e.g. "4631_DFU").
 *              Only offered when the armed device was observed in application
 *              mode: if the device being watched already advertises the DFU
 *              service, A *is* the DFU address and A + 1 is somebody else.
 *
 * Anything else — a different address, a different type, A + 1 without the DFU
 * UUID, A + 1 with the wrong name — is rejected. The name is a second check,
 * never the identity: a device that merely calls itself "4631_DFU" at the wrong
 * address is refused, and so is one at A + 1 that does not speak the DFU
 * service. (Advertising data and scan-response data arrive as separate packets,
 * which is why an absent name does not reject while a wrong one does; the DFU
 * client's own GATT discovery of the exact service UUID after connecting is the
 * final check.)
 *
 * The one address overflow case (A = FF:FF:FF:FF:FF:FF wraps to 00:...:00) is
 * handled exactly as upstream does, by carry.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mc_target_cfg {
	bool        require_dfu_uuid;   /* for the A+1 identity */
	const char *dfu_name;           /* NULL or "" = do not check */
	bool        allow_plus1;        /* false: only the exact address */
};

enum mc_target_verdict {
	MC_TGT_EXACT = 0,
	MC_TGT_DFU_PLUS1,
	MC_TGT_REJECT_ADDR,       /* neither A nor A+1 */
	MC_TGT_REJECT_TYPE,       /* right octets, wrong address type */
	MC_TGT_REJECT_UUID,       /* A+1 without the DFU service UUID */
	MC_TGT_REJECT_NAME,       /* A+1 advertising a different name */
	MC_TGT_REJECT_PLUS1_OFF,  /* A+1, but the armed device is already in DFU mode */
};

/* `armed_le` is in Bluetooth order (least-significant octet first). */
void mc_addr_plus_one(const uint8_t in_le[6], uint8_t out_le[6]);

/* `armed_type` is 'r' or 'p'; `seen_type` is 0 public / 1 random. */
enum mc_target_verdict mc_target_check(const struct mc_target_cfg *cfg,
				       const uint8_t armed_le[6], char armed_type,
				       const uint8_t seen_le[6], uint8_t seen_type,
				       bool has_dfu_uuid, const char *name);

static inline bool mc_target_accepts(enum mc_target_verdict v)
{
	return v == MC_TGT_EXACT || v == MC_TGT_DFU_PLUS1;
}

const char *mc_target_verdict_str(enum mc_target_verdict v);

#ifdef __cplusplus
}
#endif
