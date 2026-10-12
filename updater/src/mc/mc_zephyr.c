#include "mc_zephyr.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include "../battery.h"
#include "../ble_scanner.h"
#include "../dfu_runner.h"
#include "../dfu_status.h"

#include "mc_cmd.h"
#include "mc_crypto.h"
#include "mc_ctl.h"
#include "mc_evlog.h"
#include "mc_id.h"
#include "mc_node.h"
#include "mc_permit.h"
#include "mc_radio.h"
#include "mc_store.h"

LOG_MODULE_REGISTER(mc, LOG_LEVEL_INF);

#define BUNDLE_DIR "/lfs1"

enum { EV_RADIO = 1, EV_ADV, EV_DFU, EV_TICK };

struct mc_evt {
	uint8_t type;
	int8_t  rssi;
};

K_MSGQ_DEFINE(s_q, sizeof(struct mc_evt), 32, 4);
static atomic_t s_lost;                 /* advert samples dropped: queue was full */

static K_THREAD_STACK_DEFINE(s_stack, CONFIG_MC_THREAD_STACK);
static struct k_thread s_thread;
static struct k_timer s_tick;

static struct mc_id      s_id;
static struct mc_ctl     s_ctl;
static struct mc_node    s_node;
static struct mc_store   s_store;
static struct mc_evlog   s_evlog;
static struct mc_radio_rx s_rx;         /* static: too big for the thread stack */

/* ---- state shared with the BLE and DFU threads --------------------------- */

static struct k_spinlock s_lock;
static bool    s_watch_on;
static uint8_t s_watch_le[6];
static char    s_watch_type;

static struct dfu_status_event s_dfu_latest;
static bool    s_dfu_dirty;

static void post(uint8_t type, int8_t rssi)
{
	struct mc_evt e = { .type = type, .rssi = rssi };

	(void)k_msgq_put(&s_q, &e, K_NO_WAIT);
}

static void adv_observer(const bt_addr_le_t *a, int8_t rssi)
{
	uint8_t le[6];
	char t;
	bool on;
	k_spinlock_key_t k = k_spin_lock(&s_lock);

	on = s_watch_on;
	memcpy(le, s_watch_le, 6);
	t = s_watch_type;
	k_spin_unlock(&s_lock, k);

	/* Exact identity only: six octets and the address type. */
	if (!on || !mc_addr_equal(le, t, a->a.val, a->type)) {
		return;
	}
	struct mc_evt e = { .type = EV_ADV, .rssi = rssi };

	if (k_msgq_put(&s_q, &e, K_NO_WAIT) != 0) {
		atomic_set(&s_lost, 1);
	}
}

/* DFU status can change thousands of times in a run; keep only the latest and
 * let the mc thread pick it up. A terminal state is never lost this way, which
 * a bounded queue of events could not promise. */
static void dfu_observer(const struct dfu_status_event *ev)
{
	k_spinlock_key_t k = k_spin_lock(&s_lock);
	bool changed = memcmp(&s_dfu_latest, ev, sizeof(*ev)) != 0;

	if (changed) {
		s_dfu_latest = *ev;
		s_dfu_dirty = true;
	}
	k_spin_unlock(&s_lock, k);
	if (changed) {
		post(EV_DFU, 0);
	}
}

static void radio_irq(void)
{
	post(EV_RADIO, 0);
}

/* ---- controller ops ------------------------------------------------------ */

static uint32_t op_now(void *c) { ARG_UNUSED(c); return k_uptime_get_32(); }

static void addr_str(const bt_addr_le_t *a, char *out)
{
	snprintf(out, MC_ADDR_STR_LEN + 1, "%02X:%02X:%02X:%02X:%02X:%02X",
		 a->a.val[5], a->a.val[4], a->a.val[3], a->a.val[2], a->a.val[1], a->a.val[0]);
}

static size_t op_scan_rows(void *c, struct mc_scan_row *rows, size_t max, size_t off,
			   size_t *total)
{
	static struct ble_scanner_seen seen[BLE_SCANNER_SURVEY_MAX];
	uint8_t order[BLE_SCANNER_SURVEY_MAX];
	size_t held = 0;

	ARG_UNUSED(c);
	/* Starts the survey if nothing is running; a poll if it is. The first
	 * `scan` after idle therefore finds an empty table. */
	(void)ble_scanner_survey_start();
	size_t n = ble_scanner_survey_get(seen, BLE_SCANNER_SURVEY_MAX, 0, &held);

	for (size_t i = 0; i < n; i++) {
		size_t j = i;

		while (j > 0 && seen[order[j - 1]].rssi < seen[i].rssi) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = (uint8_t)i;
	}
	*total = n;
	size_t w = 0;

	for (size_t i = off; i < n && w < max; i++, w++) {
		const struct ble_scanner_seen *e = &seen[order[i]];

		addr_str(&e->addr, rows[w].addr);
		rows[w].type = (e->addr.type == BT_ADDR_LE_PUBLIC ||
				e->addr.type == BT_ADDR_LE_PUBLIC_ID) ? 'p' : 'r';
		rows[w].rssi = e->rssi;
		strncpy(rows[w].name, e->name, sizeof(rows[w].name) - 1);
		rows[w].name[sizeof(rows[w].name) - 1] = '\0';
	}
	return w;
}

static void op_watch(void *c, const char *addr, char type)
{
	ARG_UNUSED(c);
	k_spinlock_key_t k = k_spin_lock(&s_lock);

	if (addr == NULL) {
		s_watch_on = false;
	} else {
		s_watch_on = mc_addr_to_le(addr, s_watch_le);
		s_watch_type = type;
	}
	k_spin_unlock(&s_lock, k);
	if (addr != NULL) {
		(void)ble_scanner_survey_start();
	}
}

static int op_bundle_check(void *c, const char *bundle)
{
	char path[sizeof(BUNDLE_DIR) + 1 + MC_BUNDLE_MAX + 1];
	struct fs_dirent st;

	ARG_UNUSED(c);
	if (!mc_bundle_name_ok(bundle)) {
		return -EINVAL;
	}
	snprintf(path, sizeof(path), BUNDLE_DIR "/%s", bundle);
	int rc = fs_stat(path, &st);

	if (rc < 0) {
		return rc;
	}
	return (st.type == FS_DIR_ENTRY_FILE && st.size > 0) ? 0 : -ENOENT;
}

static int op_dfu_start(void *c, const char *bundle, const char *addr, char type)
{
	char path[sizeof(BUNDLE_DIR) + 1 + MC_BUNDLE_MAX + 1];
	char pin[MC_ADDR_STR_LEN + 12];

	ARG_UNUSED(c);
	snprintf(path, sizeof(path), BUNDLE_DIR "/%s", bundle);
	snprintf(pin, sizeof(pin), "%s (%s)", addr, type == 'p' ? "public" : "random");

	/* The only place in the firmware that grants a start permit. It is
	 * granted and consumed within this call. */
	mc_permit_grant();
	int rc = dfu_runner_start(path, pin, NULL);

	mc_permit_revoke();
	return rc;
}

static int op_dfu_abort(void *c) { ARG_UNUSED(c); return dfu_runner_stop(); }
static bool op_dfu_busy(void *c) { ARG_UNUSED(c); return dfu_runner_busy(); }

static int op_battery(void *c, uint16_t *mv, uint8_t *pct, int *charging)
{
	struct battery_status b;

	ARG_UNUSED(c);
	if (battery_read(&b) != 0) {
		return -ENODEV;
	}
	*mv = b.millivolts;
	*pct = b.percent;
	*charging = b.charging_known ? (b.charging ? 1 : 0) : -1;
	return 0;
}

static const char *op_version(void *c)
{
	ARG_UNUSED(c);
	return "drone_meshcore_updater " APP_VERSION_EXTENDED_STRING " mc1";
}

static size_t op_log_page(void *c, unsigned page, char *out, size_t cap)
{
	ARG_UNUSED(c);
	return mc_evlog_page(&s_evlog, page, out, cap);
}

static void op_report(void *c, const char *msg)
{
	ARG_UNUSED(c);
	mc_node_send_text(&s_node, msg);
}

static void op_note(void *c, const char *msg)
{
	ARG_UNUSED(c);
	mc_evlog_add(&s_evlog, k_uptime_get_32() / 1000u, msg);
	LOG_INF("%s", msg);
}

static const struct mc_ops OPS = {
	.now_ms = op_now, .scan_rows = op_scan_rows, .watch = op_watch,
	.bundle_check = op_bundle_check, .dfu_start = op_dfu_start,
	.dfu_abort = op_dfu_abort, .dfu_busy = op_dfu_busy, .battery = op_battery,
	.version = op_version, .log_page = op_log_page, .report = op_report,
	.note = op_note,
};

/* ---- node ops ------------------------------------------------------------ */

static void nop_tx(void *c, const uint8_t *f, size_t n)
{
	ARG_UNUSED(c);
	int rc = mc_radio_tx(f, n);

	if (rc) {
		LOG_WRN("tx failed rc=%d (%u bytes)", rc, (unsigned)n);
	}
}

static void nop_persist(void *c, const struct mc_persist *p)
{
	ARG_UNUSED(c);
	if (mc_store_append(&s_store, p) != MC_STORE_OK) {
		LOG_ERR("persist failed — replay protection will restart from the last good record");
	}
}

static uint8_t nop_rand(void *c) { ARG_UNUSED(c); return (uint8_t)sys_rand32_get(); }

static const struct mc_node_ops NODE_OPS = {
	.tx = nop_tx, .persist = nop_persist, .note = op_note, .now_ms = op_now,
	.random_byte = nop_rand,
};

/* ---- identity page ------------------------------------------------------- */

#define ID_DEV  FIXED_PARTITION_DEVICE(mc_identity_partition)
#define ID_OFF  FIXED_PARTITION_OFFSET(mc_identity_partition)
BUILD_ASSERT(FIXED_PARTITION_SIZE(mc_identity_partition) == MC_STORE_PAGE,
	     "mc-identity partition must be exactly one 4 KB page");

static int fl_read(void *c, uint32_t off, void *b, size_t n)
{
	ARG_UNUSED(c);
	return flash_read(ID_DEV, ID_OFF + off, b, n);
}
static int fl_write(void *c, uint32_t off, const void *b, size_t n)
{
	ARG_UNUSED(c);
	return flash_write(ID_DEV, ID_OFF + off, b, n);
}
static int fl_erase(void *c)
{
	ARG_UNUSED(c);
	return flash_erase(ID_DEV, ID_OFF, MC_STORE_PAGE);
}
static const struct mc_flash FLASH = { fl_read, fl_write, fl_erase, NULL };

static void hex(const uint8_t *in, size_t n, char *out)
{
	static const char d[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static int load_identity(struct mc_persist *persisted)
{
	if (!device_is_ready(ID_DEV)) {
		LOG_ERR("identity flash not ready");
		return -ENODEV;
	}
	int rc = mc_store_load(&s_store, &FLASH, persisted);
	if (rc == MC_STORE_OK && mc_id_from_private(&s_id, s_store.prv) &&
	    memcmp(s_id.pub, s_store.pub, MC_PUB_KEY_SIZE) == 0) {
		return 0;
	}
	if (rc == MC_STORE_IOERR) {
		LOG_ERR("identity flash unreadable");
		return -EIO;
	}
	LOG_INF("generating MeshCore identity (%s)", rc == MC_STORE_OK ? "stored one is corrupt" : "first boot");

	const struct device *ent = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t seed[32];

	if (!device_is_ready(ent)) {
		return -ENODEV;
	}
	do {
		rc = entropy_get_entropy(ent, seed, sizeof(seed));
		if (rc) {
			return rc;
		}
	} while (!mc_id_from_seed(&s_id, seed));
	memset(seed, 0, sizeof(seed));

	if (mc_store_provision(&s_store, s_id.prv, s_id.pub) != MC_STORE_OK) {
		LOG_ERR("identity write failed");
		return -EIO;
	}
	memset(persisted, 0, sizeof(*persisted));
	return 0;
}

/* ---- thread -------------------------------------------------------------- */

static void take_dfu(void)
{
	struct dfu_status_event ev;
	bool dirty;
	k_spinlock_key_t k = k_spin_lock(&s_lock);

	dirty = s_dfu_dirty;
	ev = s_dfu_latest;
	s_dfu_dirty = false;
	k_spin_unlock(&s_lock, k);
	if (dirty) {
		mc_ctl_on_dfu_event(&s_ctl, &ev);
	}
}

static void tick_fn(struct k_timer *t) { ARG_UNUSED(t); post(EV_TICK, 0); }

static void mc_thread(void *a, void *b, void *c)
{
	struct mc_persist persisted = { 0 };
	uint8_t op_pub[MC_PUB_KEY_SIZE];

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	if (load_identity(&persisted) != 0) {
		LOG_ERR("MeshCore interface disabled (identity)");
		return;
	}
	char h[65];

	hex(s_id.pub, MC_PUB_KEY_SIZE, h);
	LOG_INF("MeshCore node public key: %s", h);

	if (strlen(CONFIG_MC_OPERATOR_PUBKEY) == 0 || !mc_hex32(CONFIG_MC_OPERATOR_PUBKEY, op_pub)) {
		LOG_WRN("CONFIG_MC_OPERATOR_PUBKEY is %s — every command will be ignored",
			strlen(CONFIG_MC_OPERATOR_PUBKEY) ? "not 64 hex characters" : "empty");
	} else {
		mc_id_set_operator(&s_id, op_pub);
	}

	struct mc_cfg cfg = {
		.rssi_min = CONFIG_MC_RSSI_MIN,
		.hold_ms = CONFIG_MC_GATE_HOLD_MS,
		.max_gap_ms = CONFIG_MC_GATE_MAX_GAP_MS,
		.min_samples = CONFIG_MC_GATE_MIN_SAMPLES,
		.arm_timeout_s = CONFIG_MC_ARM_TIMEOUT_S,
	};

	mc_evlog_init(&s_evlog);
	mc_ctl_init(&s_ctl, &OPS, NULL, &cfg);
	mc_node_init(&s_node, &MC_CRYPTO_BUILTIN, &s_id, &s_ctl, &NODE_OPS, NULL, &persisted);

	int rc = mc_radio_init(radio_irq);

	if (rc) {
		LOG_ERR("MeshCore interface disabled (radio rc=%d)", rc);
		return;
	}
	ble_scanner_set_adv_observer(adv_observer);
	dfu_status_set_observer(dfu_observer);
	k_timer_init(&s_tick, tick_fn, NULL);
	k_timer_start(&s_tick, K_SECONDS(1), K_SECONDS(1));

	uint32_t next_adv = k_uptime_get_32() + 15000u + (sys_rand32_get() % 15000u);
	uint32_t ticks = 0;

	LOG_INF("MeshCore interface up: rssi_min=%d hold=%ums", cfg.rssi_min,
		(unsigned)cfg.hold_ms);

	for (;;) {
		struct mc_evt e;

		k_msgq_get(&s_q, &e, K_FOREVER);
		if (atomic_cas(&s_lost, 1, 0)) {
			mc_ctl_samples_lost(&s_ctl);
		}
		take_dfu();

		switch (e.type) {
		case EV_RADIO:
			while (mc_radio_service(&s_rx)) {
				mc_node_rx(&s_node, s_rx.data, s_rx.len);
			}
			break;
		case EV_ADV:
			mc_ctl_on_adv(&s_ctl, e.rssi);
			break;
		case EV_TICK:
			mc_ctl_tick(&s_ctl);
			/* The survey's own idle timer is 6 s; poll it while armed so
			 * the gate keeps getting samples. */
			if (s_ctl.state == MC_ARMED && (++ticks % 3u) == 0) {
				(void)ble_scanner_survey_start();
			}
			if ((int32_t)(k_uptime_get_32() - next_adv) >= 0) {
				mc_node_advertise(&s_node, CONFIG_MC_NODE_NAME);
				next_adv = k_uptime_get_32() + CONFIG_MC_ADVERT_INTERVAL_S * 1000u;
			}
			break;
		default:
			break;
		}
	}
}

int mc_init(void)
{
	k_tid_t t = k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
				    mc_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0,
				    K_NO_WAIT);

	if (t == NULL) {
		return -ENOMEM;
	}
	k_thread_name_set(t, "mc");
	return 0;
}
