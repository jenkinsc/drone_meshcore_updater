#pragma once

/*
 * MeshCore command grammar. Pure parsing, no Zephyr, no I/O.
 *
 * Commands arrive as the text of a MeshCore direct message. One line, words
 * separated by spaces, command word case-insensitive:
 *
 *   status
 *   scan [page]                       BLE advertisers heard, strongest first
 *   rssi                              live gate state for the armed target
 *   arm <addr> <bundle> [random|public] [token]
 *   disarm
 *   start [token]
 *   abort
 *   version
 *   battery
 *   log [page]                        recent events, newest first
 *
 * `addr` is exactly "AA:BB:CC:DD:EE:FF" (either case). It is the *whole* target
 * identity: there is no name matching, no prefix matching, no wildcard. The
 * address type defaults to random, which is what a Nordic DFU peer uses, and
 * is part of the identity — a public and a random address with the same six
 * octets are different devices.
 *
 * `bundle` is a bare file name in the updater's bundle directory. No '/', no
 * "..": the command must not be able to name anything outside it.
 *
 * Arguments are strict. Extra words are an error, not ignored: a mistyped
 * "start now" that silently meant "start" is how a command layer ends up doing
 * something other than what the operator wrote.
 *
 * ---- Privilege -----------------------------------------------------------
 *
 * arm, disarm, start and abort change what the updater does and are
 * *privileged*. status, scan, rssi, version, battery and log only report.
 * mc_cmd_privileged() is the single table; the controller consults it and the
 * tests pin it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_ADDR_STR_LEN  17          /* "AA:BB:CC:DD:EE:FF" */
#define MC_BUNDLE_MAX    40          /* characters, excluding NUL */
#define MC_TOKEN_LEN     6           /* hex digits */

enum mc_cmd_id {
	MC_CMD_STATUS = 0,
	MC_CMD_SCAN,
	MC_CMD_RSSI,
	MC_CMD_ARM,
	MC_CMD_DISARM,
	MC_CMD_START,
	MC_CMD_ABORT,
	MC_CMD_VERSION,
	MC_CMD_BATTERY,
	MC_CMD_LOG,
	MC_CMD_DIAG,      /* lab-only diagnostics, read-only */
	MC_CMD_RADIO,
	MC_CMD_BLE,
	MC_CMD_STORAGE,
	MC_CMD_COUNT
};

struct mc_cmd {
	enum mc_cmd_id id;
	unsigned page;                          /* scan / log, 1-based */
	char     addr[MC_ADDR_STR_LEN + 1];     /* arm: normalised upper-case */
	char     addr_type;                     /* arm: 'r' random, 'p' public */
	char     bundle[MC_BUNDLE_MAX + 1];     /* arm */
	char     token[MC_TOKEN_LEN + 1];       /* arm / start; "" if absent */
};

enum mc_parse_err {
	MC_PARSE_OK = 0,
	MC_PARSE_EMPTY,
	MC_PARSE_UNKNOWN,       /* not a command word */
	MC_PARSE_USAGE,         /* known command, wrong arguments */
	MC_PARSE_BAD_ADDR,
	MC_PARSE_BAD_BUNDLE,
};

enum mc_parse_err mc_cmd_parse(const char *text, struct mc_cmd *out);

bool mc_cmd_privileged(enum mc_cmd_id id);
/* True for the commands that must carry a fresh single-use token (arm, start). */
bool mc_cmd_needs_token(enum mc_cmd_id id);
bool mc_token_ok(const char *s);
const char *mc_cmd_name(enum mc_cmd_id id);

/* Normalise "aa:bb:..." to upper case. Returns false unless `s` is exactly six
 * colon-separated hex octets. `out` needs MC_ADDR_STR_LEN + 1 bytes. */
bool mc_addr_normalise(const char *s, char *out);

/* Convert a normalised "AA:BB:CC:DD:EE:FF" to six octets in Bluetooth's
 * in-memory order (least-significant octet first, i.e. reversed from how it is
 * printed) — the layout of bt_addr_t.val. False if `s` is not a normalised
 * address. */
bool mc_addr_to_le(const char *s, uint8_t le[6]);

/* Exact identity match: all six octets and the address type ('r'/'p' against
 * the observed type byte, 0 = public, 1 = random). There is no partial or
 * "+1" form here by design — see the gate's contract. */
bool mc_addr_equal(const uint8_t armed_le[6], char armed_type,
		   const uint8_t seen_le[6], uint8_t seen_type);

/* True for a safe bundle file name: [A-Za-z0-9._-], not starting with '.',
 * 1..MC_BUNDLE_MAX characters. */
bool mc_bundle_name_ok(const char *s);

#ifdef __cplusplus
}
#endif
