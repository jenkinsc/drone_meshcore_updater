#include "mc_lab.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "../ble_scanner.h"
#include "mc_cmd.h"
#include "mc_crypto.h"
#include "mc_radio.h"
#include "mc_target.h"

LOG_MODULE_DECLARE(mc);

#ifndef MC_GIT_HASH
#define MC_GIT_HASH "unknown"
#endif
#ifndef MC_PROFILE_NAME
#define MC_PROFILE_NAME "unknown"
#endif

static struct mc_lab_env E;
static struct k_spinlock L;

/* BLE-thread state, under L. */
static struct {
	uint32_t adv_total, dfu_adv_total, match_total;
	uint8_t  any_le[6]; int8_t any_rssi; bool any_have;
	uint8_t  dfu_le[6]; int8_t dfu_rssi; char dfu_name[16]; bool dfu_have;
	char     dfu_rel;            /* 'e' same as armed, '+' A+1, 'o' other, '-' nothing armed */
	uint32_t dfu_log_at;
	uint8_t  match_le[6]; int8_t match_rssi; bool match_have;
} S;

void mc_lab_bind(const struct mc_lab_env *env) { E = *env; }

static void hexs(const uint8_t *in, size_t n, char *out)
{
	static const char d[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static void le_str(const uint8_t le[6], char *o)
{
	snprintf(o, 18, "%02X:%02X:%02X:%02X:%02X:%02X", le[5], le[4], le[3], le[2], le[1], le[0]);
}

static const char *idstat_str(void)
{
	switch (E.id_stat) {
	case MC_LAB_ID_LOADED: return "valid, loaded";
	case MC_LAB_ID_GENERATED: return "EMPTY page: new identity generated";
	case MC_LAB_ID_REGENERATED: return "CORRUPT page: identity REGENERATED";
	default: return "none";
	}
}

/* snr in quarter-dB to "-3.25" without floating point. */
static void snr_str(int q2, char *o, size_t n)
{
	unsigned m = (unsigned)(q2 < 0 ? -q2 : q2);

	snprintf(o, n, "%s%u.%02u", q2 < 0 ? "-" : "", (m / 4) % 1000u, (m % 4) * 25u);
}

static const char *cause_str(uint32_t c)
{
	if (c & RESET_POR) { return "power-on"; }
	if (c & RESET_PIN) { return "pin/double-tap"; }
	if (c & RESET_SOFTWARE) { return "software"; }
	if (c & RESET_WATCHDOG) { return "WATCHDOG"; }
	if (c & RESET_BROWNOUT) { return "BROWNOUT"; }
	if (c & RESET_CPU_LOCKUP) { return "LOCKUP"; }
	if (c & RESET_DEBUG) { return "debug"; }
	return c ? "other" : "none-latched";
}

/* Bundle inventory: *.zip regular files in the bundle directory. */
static int inventory(bool log)
{
	struct fs_dir_t d;
	struct fs_dirent e;
	int n = 0;

	fs_dir_t_init(&d);
	int rc = fs_opendir(&d, E.bundle_dir);

	if (rc < 0) {
		if (log) {
			LOG_WRN("bundles: %s unavailable rc=%d", E.bundle_dir, rc);
		}
		return rc;
	}
	while (fs_readdir(&d, &e) == 0 && e.name[0] != '\0') {
		if (e.type == FS_DIR_ENTRY_FILE && strstr(e.name, ".zip")) {
			if (log && n < 6) {
				LOG_INF("bundle: %s %u B", e.name, (unsigned)e.size);
			}
			n++;
		}
	}
	(void)fs_closedir(&d);
	return n;
}

void mc_lab_init_failed(const char *reason, int rc)
{
	LOG_ERR("AIRDFU INIT FAILED: %s (rc=%d)", reason, rc);
}

void mc_lab_boot_report(int radio_rc)
{
	const struct mc_radio_stats *rs = mc_radio_stats_get();
	char h[65], line[96];
	uint8_t uid[8] = { 0 };
	const char *fail = NULL;
	int busy, dio1, rst;

	LOG_INF("==== AIRDFU LAB BOOT ====");
	LOG_INF("git %s profile %s dry_run=%s", MC_GIT_HASH, MC_PROFILE_NAME,
		IS_ENABLED(CONFIG_MC_DFU_DRY_RUN) ? "YES (nothing will be flashed)"
						  : "NO -- REAL DFU ENABLED");
	LOG_INF("reset cause 0x%x (%s)", (unsigned)E.reset_cause, cause_str(E.reset_cause));
	if (hwinfo_get_device_id(uid, sizeof(uid)) > 0) {
		hexs(uid, sizeof(uid), h);
		LOG_INF("hw id %s", h);
	} else {
		LOG_WRN("hw id unavailable");
	}
	hexs(E.id->pub, MC_PUB_KEY_SIZE, h);
	LOG_INF("meshcore pub %s", h);
	if (E.op_key_ok) {
		uint8_t fp[32];

		mc_sha256(E.op_pub, 32, NULL, 0, fp);
		hexs(fp, 8, h);
		LOG_INF("operator key fpr sha256[0..8]=%s (set)", h);
	} else {
		LOG_WRN("operator key NOT SET: every command will be ignored");
	}
	LOG_INF("identity partition 0x%05x-0x%05x (%u B)", (unsigned)E.id_start,
		(unsigned)E.id_end - 1u, (unsigned)(E.id_end - E.id_start));
	LOG_INF("identity record: %s; slot %u/%u seq %u", idstat_str(), (unsigned)E.store->next_slot,
		(unsigned)MC_STORE_MAX_RECS, (unsigned)E.store->seq);
	LOG_INF("sx1262 spi/gpio ready: %s", mc_radio_hw_ready() ? "yes" : "NO");
	mc_radio_pins(&busy, &dio1, &rst);
	LOG_INF("sx1262 pins BUSY=%d DIO1=%d RESET=%d (1=asserted)", busy, dio1, rst);
	LOG_INF("sx1262 init: %s rc=%d", rs->up ? "ok" : "FAILED", radio_rc);
	LOG_INF("radio %u Hz BW%u SF%u CR4/%u %d dBm sync 0x%02x", (unsigned)CONFIG_MC_FREQ_HZ,
		(unsigned)CONFIG_MC_BW_HZ, (unsigned)CONFIG_MC_SF, (unsigned)CONFIG_MC_CR,
		CONFIG_MC_TX_POWER_DBM, CONFIG_MC_SYNC_WORD);
	LOG_INF("ble ready: %s", bt_is_ready() ? "yes" : "NO");
	int nb = inventory(true);

	LOG_INF("bundles: %d", nb < 0 ? 0 : nb);
	snprintf(line, sizeof(line), "exact addr+type from ARM; A+1 %s, dfu uuid %s, dfu name '%s'",
		 IS_ENABLED(CONFIG_MC_TARGET_ALLOW_PLUS1) ? "allowed" : "off",
		 IS_ENABLED(CONFIG_MC_TARGET_REQUIRE_DFU_UUID) ? "required" : "not required",
		 CONFIG_MC_TARGET_DFU_NAME);
	LOG_INF("target: %s", line);
	LOG_INF("rssi: avg>=%d floor=%d hyst=%d hold=%ums min_samples=%u max_gap=%ums",
		CONFIG_MC_AVG_RSSI_MIN, CONFIG_MC_RSSI_FLOOR, CONFIG_MC_GATE_HYST_DB,
		(unsigned)CONFIG_MC_GATE_HOLD_MS, (unsigned)CONFIG_MC_GATE_MIN_SAMPLES,
		(unsigned)CONFIG_MC_GATE_MAX_GAP_MS);

	if (E.id_stat == MC_LAB_ID_NONE) {
		fail = "identity";
	} else if (!mc_radio_hw_ready()) {
		fail = "sx1262 spi/gpio not ready";
	} else if (radio_rc != 0 || !rs->up) {
		fail = "sx1262 init";
	} else if (!bt_is_ready()) {
		fail = "ble not ready";
	}
	if (fail) {
		LOG_ERR("AIRDFU INIT FAILED: %s", fail);
	} else {
		LOG_INF("AIRDFU LAB READY");
	}
}

void mc_lab_adv(const bt_addr_le_t *a, int8_t rssi, bool dfu_uuid, const char *name,
		const uint8_t *watch_le)
{
	char rel = '-';
	bool log_it = false;
	char s1[18], s2[18];
	k_spinlock_key_t k = k_spin_lock(&L);

	S.adv_total++;
	memcpy(S.any_le, a->a.val, 6); S.any_rssi = rssi; S.any_have = true;
	if (watch_le && memcmp(watch_le, a->a.val, 6) == 0) {
		S.match_total++;
		memcpy(S.match_le, a->a.val, 6); S.match_rssi = rssi; S.match_have = true;
	}
	if (dfu_uuid) {
		uint8_t p1[6];
		uint32_t now = k_uptime_get_32();

		S.dfu_adv_total++;
		if (watch_le) {
			mc_addr_plus_one(watch_le, p1);
			rel = memcmp(a->a.val, watch_le, 6) == 0 ? 'e'
			      : memcmp(a->a.val, p1, 6) == 0 ? '+' : 'o';
		}
		bool changed = !S.dfu_have || memcmp(S.dfu_le, a->a.val, 6) != 0 || S.dfu_rel != rel;

		memcpy(S.dfu_le, a->a.val, 6); S.dfu_rssi = rssi; S.dfu_have = true; S.dfu_rel = rel;
		snprintf(S.dfu_name, sizeof(S.dfu_name), "%s", name ? name : "");
		if (changed || now - S.dfu_log_at > 5000u) {
			S.dfu_log_at = now;
			log_it = true;
		}
	}
	k_spin_unlock(&L, k);
	if (log_it) {
		le_str(a->a.val, s1);
		if (watch_le) {
			le_str(watch_le, s2);
		} else {
			snprintf(s2, sizeof(s2), "(none armed)");
		}
		LOG_INF("DFU-UUID advert %s rssi %d name '%s' vs armed %s: %s", s1, rssi,
			name ? name : "", s2,
			rel == 'e' ? "SAME address" : rel == '+' ? "armed+1" :
			rel == 'o' ? "DIFFERENT (not A, not A+1)" : "no target armed");
	}
}

static size_t put(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static size_t put(char *out, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(out, cap, fmt, ap);

	va_end(ap);
	return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

size_t mc_lab_diag(void *ctx, int cmd_id, char *out, size_t cap)
{
	const struct mc_radio_stats *rs = mc_radio_stats_get();
	char a[18], b[18], snr[12];

	ARG_UNUSED(ctx);
	switch (cmd_id) {
	case MC_CMD_DIAG: {
		return put(out, cap, "diag up=%us rst=0x%x lora=%s ble=%s%s id=%s st=%s q=%u lost=%ld",
			   (unsigned)(k_uptime_get_32() / 1000u), (unsigned)E.reset_cause,
			   rs->up ? "up" : "DOWN", bt_is_ready() ? "up" : "DOWN",
			   ble_scanner_survey_active() ? "/scan" : "/idle",
			   E.id_stat == MC_LAB_ID_LOADED ? "ok" : E.id_stat == MC_LAB_ID_NONE ? "NONE" : "new",
			   mc_state_str(E.ctl->state),
			   (unsigned)k_msgq_num_used_get(E.queue), E.lost ? *E.lost : 0L);
	}
	case MC_CMD_RADIO:
		snr_str(rs->last_snr_q2, snr, sizeof(snr));
		return put(out, cap,
			   "radio %uHz BW%u SF%u CR4/%u %ddBm rx=%u tx=%u last=%s dBm/%s dB crc=%u hdr=%u busy=%u spi=%u txf=%u txt=%u rxf=%u irq=%u",
			   (unsigned)CONFIG_MC_FREQ_HZ, (unsigned)CONFIG_MC_BW_HZ, (unsigned)CONFIG_MC_SF,
			   (unsigned)CONFIG_MC_CR, CONFIG_MC_TX_POWER_DBM, (unsigned)rs->rx_ok,
			   (unsigned)rs->tx_ok, rs->have_rx ? "" : "none", rs->have_rx ? snr : "-",
			   (unsigned)rs->rx_crc_err, (unsigned)rs->rx_hdr_err, (unsigned)rs->busy_timeouts,
			   (unsigned)rs->spi_faults, (unsigned)rs->tx_fail, (unsigned)rs->tx_timeout,
			   (unsigned)rs->rx_restore_fail, (unsigned)rs->dio1_irqs);
	case MC_CMD_BLE: {
		k_spinlock_key_t k = k_spin_lock(&L);
		uint32_t adv = S.adv_total, dfu = S.dfu_adv_total;
		bool hm = S.match_have, hd = S.dfu_have;
		int mr = S.match_rssi, dr = S.dfu_rssi;
		uint8_t ml[6], dl[6];
		char rel = S.dfu_rel;

		memcpy(ml, S.match_le, 6); memcpy(dl, S.dfu_le, 6);
		k_spin_unlock(&L, k);
		le_str(ml, a); le_str(dl, b);
		size_t n = put(out, cap, "ble scan=%s adv=%u dfuadv=%u | match=%s n=%u cur=%d avg=%d min=%d",
			       ble_scanner_survey_active() ? "on" : "idle", (unsigned)adv, (unsigned)dfu,
			       hm ? a : "none", (unsigned)E.ctl->gate.acq_samples,
			       hm ? mr : 0, (int)mc_gate_avg(&E.ctl->gate), (int)mc_gate_min(&E.ctl->gate));
		if (hd && n + 30 < cap) {
			n += put(out + n, cap - n, " | dfu=%s %d rel=%s", b, dr,
				 rel == 'e' ? "same" : rel == '+' ? "A+1" : rel == 'o' ? "other" : "?");
		}
		return n;
	}
	case MC_CMD_STORAGE: {
		struct mc_store t = { 0 };
		struct mc_persist p;
		int rc = mc_store_load(&t, E.flash, &p);
		bool same = rc == MC_STORE_OK && memcmp(t.pub, E.id->pub, MC_PUB_KEY_SIZE) == 0;

		memset(t.prv, 0, sizeof(t.prv));
		int nb = inventory(false);

		return put(out, cap, "storage id=%s(rc=%d) slot %u/%u seq %u bundles=%d sel=%s fw=%s",
			   same ? "valid,key matches" : "INVALID", rc, (unsigned)t.next_slot,
			   (unsigned)MC_STORE_MAX_RECS, (unsigned)t.seq, nb < 0 ? 0 : nb,
			   E.ctl->bundle[0] ? E.ctl->bundle : "none", E.ctl->fw[0] ? E.ctl->fw : "-");
	}
	default:
		return put(out, cap, "?");
	}
}
