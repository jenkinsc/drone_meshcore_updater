/*
 * Host tests for the MeshCore command layer. Plain C, no framework:
 *
 *     make -C updater/mc-tests
 *
 * Covers the logic that decides whether a firmware update may start. The Zephyr
 * glue (radio, flash, BLE scanner hooks) is not exercised here; see README.md
 * for what that leaves untested.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mc/mc_cmd.h"
#include "../src/mc/mc_ctl.h"
#include "../src/mc/mc_evlog.h"
#include "../src/mc/mc_gate.h"
#include "../src/mc/mc_id.h"
#include "../src/mc/mc_node.h"
#include "../src/mc/mc_progress.h"
#include "../src/mc/mc_replay.h"
#include "../src/mc/mc_wire.h"
#include "../src/mc/mc_crypto.h"
#include "../src/mc/mc_sx126x.h"
#include "../src/mc/mc_store.h"
#include "../src/mc/ed25519/sha512.h"
#include <openssl/evp.h>

extern const struct mc_crypto TEST_CRYPTO;
static const struct mc_crypto *CRY;

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
	printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define SECTION(n) printf("%s\n", n)

/* ---------------------------------------------------------------- vectors -- */

static uint8_t V_node_pub[32], V_op_pub[32], V_secret_node[32], V_secret_op[32];
static uint8_t V_plain[64], V_sealed[128], V_ack4[4], V_adv_msg[64], V_adv_sig[64];
static size_t V_plain_len, V_sealed_len, V_adv_msg_len;

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
	size_t n = strlen(h) / 2;
	assert(n <= cap);
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(h + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
	return n;
}

static void load_vectors(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];

	assert(f && "vectors file missing: run from updater/mc-tests");
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *val = eq + 1;
		val[strcspn(val, "\r\n")] = '\0';
		if (!strcmp(line, "node_pub")) unhex(val, V_node_pub, 32);
		else if (!strcmp(line, "op_pub")) unhex(val, V_op_pub, 32);
		else if (!strcmp(line, "secret_node_side")) unhex(val, V_secret_node, 32);
		else if (!strcmp(line, "secret_op_side")) unhex(val, V_secret_op, 32);
		else if (!strcmp(line, "plain")) V_plain_len = unhex(val, V_plain, sizeof(V_plain));
		else if (!strcmp(line, "sealed")) V_sealed_len = unhex(val, V_sealed, sizeof(V_sealed));
		else if (!strcmp(line, "ack4")) unhex(val, V_ack4, 4);
		else if (!strcmp(line, "advert_msg")) V_adv_msg_len = unhex(val, V_adv_msg, sizeof(V_adv_msg));
		else if (!strcmp(line, "advert_sig")) unhex(val, V_adv_sig, 64);
	}
	fclose(f);
}

static void seed_for(uint8_t base, uint8_t seed[32])   /* reference FixedRNG */
{
	for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(base + i * 7 + 3);
}

/* ------------------------------------------------------------------- gate -- */

static void feed(struct mc_gate *g, uint32_t from, uint32_t to, uint32_t step, int8_t rssi)
{
	for (uint32_t t = from; t <= to; t += step) mc_gate_sample(g, t, rssi);
}

static void test_gate(void)
{
	struct mc_gate g;
	SECTION("gate");

	mc_gate_init(&g, -75, 5000, 3000, 3);
	CHECK(mc_gate_check(&g, 0) == MC_GATE_NOT_SEEN);

	/* strong from t=1000, one advert per 500 ms */
	feed(&g, 1000, 5999, 500, -60);
	CHECK(mc_gate_check(&g, 5999) == MC_GATE_SETTLING);     /* 4999 ms in */
	feed(&g, 6000, 6000, 500, -60);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);        /* exactly 5000 ms */
	CHECK(mc_gate_ready(&g, 6000));

	/* the window ends if the target goes quiet */
	CHECK(mc_gate_check(&g, 6000 + 3001) == MC_GATE_STALE);

	/* one weak sample restarts the clock, however strong the rest */
	mc_gate_init(&g, -75, 5000, 3000, 3);
	feed(&g, 0, 2500, 500, -50);
	mc_gate_sample(&g, 3000, -80);
	CHECK(mc_gate_check(&g, 3000) == MC_GATE_TOO_WEAK);
	feed(&g, 3500, 8000, 500, -50);
	CHECK(mc_gate_check(&g, 8000) == MC_GATE_SETTLING);     /* window began 3500 */
	feed(&g, 8500, 8500, 500, -50);
	CHECK(mc_gate_check(&g, 8500) == MC_GATE_READY);

	/* threshold is inclusive */
	mc_gate_init(&g, -75, 5000, 3000, 3);
	feed(&g, 0, 5000, 500, -75);
	CHECK(mc_gate_ready(&g, 5000));
	mc_gate_init(&g, -75, 5000, 3000, 3);
	feed(&g, 0, 5000, 500, -76);
	CHECK(!mc_gate_ready(&g, 5000));

	/* a silence longer than max_gap restarts the window even if it recovers */
	mc_gate_init(&g, -75, 5000, 3000, 3);
	feed(&g, 0, 2000, 500, -50);
	feed(&g, 6000, 6000, 500, -50);       /* 4 s gap */
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_SETTLING);
	feed(&g, 6500, 11000, 500, -50);
	CHECK(mc_gate_ready(&g, 11000));

	/* held long enough but on too few adverts */
	mc_gate_init(&g, -75, 5000, 6000, 3);
	mc_gate_sample(&g, 0, -50);
	mc_gate_sample(&g, 5000, -50);
	CHECK(mc_gate_check(&g, 5000) == MC_GATE_FEW_SAMPLES);

	/* millisecond clock wrap */
	mc_gate_init(&g, -75, 5000, 3000, 3);
	feed(&g, 0xFFFFFF00u, 0xFFFFFF00u, 1, -50);
	for (uint32_t i = 0; i <= 12; i++) mc_gate_sample(&g, 0xFFFFFF00u + i * 500u, -50);
	CHECK(mc_gate_ready(&g, 0xFFFFFF00u + 12 * 500u));
}

/* ----------------------------------------------------------------- parser -- */

static void test_cmd(void)
{
	struct mc_cmd c;
	SECTION("cmd");

	CHECK(mc_cmd_parse("status", &c) == MC_PARSE_OK && c.id == MC_CMD_STATUS);
	CHECK(mc_cmd_parse("  STATUS \r\n", &c) == MC_PARSE_OK && c.id == MC_CMD_STATUS);
	CHECK(mc_cmd_parse("scan", &c) == MC_PARSE_OK && c.page == 1);
	CHECK(mc_cmd_parse("scan 3", &c) == MC_PARSE_OK && c.page == 3);
	CHECK(mc_cmd_parse("scan 0", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("scan x", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("log 2", &c) == MC_PARSE_OK && c.id == MC_CMD_LOG && c.page == 2);
	CHECK(mc_cmd_parse("rssi", &c) == MC_PARSE_OK);
	CHECK(mc_cmd_parse("version", &c) == MC_PARSE_OK);
	CHECK(mc_cmd_parse("battery", &c) == MC_PARSE_OK);
	CHECK(mc_cmd_parse("", &c) == MC_PARSE_EMPTY);
	CHECK(mc_cmd_parse("reboot", &c) == MC_PARSE_UNKNOWN);

	/* strict arity: extra words are errors, not ignored */
	CHECK(mc_cmd_parse("start now", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("abort all", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("disarm x", &c) == MC_PARSE_USAGE);

	CHECK(mc_cmd_parse("arm ea:7c:11:0b:55:90 fw.zip", &c) == MC_PARSE_OK);
	CHECK(!strcmp(c.addr, "EA:7C:11:0B:55:90") && c.addr_type == 'r' && !strcmp(c.bundle, "fw.zip"));
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 fw.zip public", &c) == MC_PARSE_OK && c.addr_type == 'p');
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 fw.zip random", &c) == MC_PARSE_OK && c.addr_type == 'r');
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 fw.zip weird", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90", &c) == MC_PARSE_USAGE);

	/* the address is the whole identity: nothing partial gets through */
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55 fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:9 fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm EA7C110B5590 fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:9G fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90:00 fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm * fw.zip", &c) == MC_PARSE_BAD_ADDR);
	CHECK(mc_cmd_parse("arm RAK4631 fw.zip", &c) == MC_PARSE_BAD_ADDR);   /* no name matching */

	/* bundle names cannot leave the bundle directory */
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 ../config.txt", &c) == MC_PARSE_BAD_BUNDLE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 /lfs1/fw.zip", &c) == MC_PARSE_BAD_BUNDLE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 a/b.zip", &c) == MC_PARSE_BAD_BUNDLE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 .hidden", &c) == MC_PARSE_BAD_BUNDLE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 a..b.zip", &c) == MC_PARSE_BAD_BUNDLE);

	/* privilege table */
	CHECK(mc_cmd_privileged(MC_CMD_ARM) && mc_cmd_privileged(MC_CMD_DISARM));
	CHECK(mc_cmd_privileged(MC_CMD_START) && mc_cmd_privileged(MC_CMD_ABORT));
	CHECK(!mc_cmd_privileged(MC_CMD_STATUS) && !mc_cmd_privileged(MC_CMD_SCAN));
	CHECK(!mc_cmd_privileged(MC_CMD_RSSI) && !mc_cmd_privileged(MC_CMD_VERSION));
	CHECK(!mc_cmd_privileged(MC_CMD_BATTERY) && !mc_cmd_privileged(MC_CMD_LOG));

	/* exact address match, including type and Bluetooth byte order */
	uint8_t a[6], b[6];
	CHECK(mc_addr_to_le("EA:7C:11:0B:55:90", a) && a[0] == 0x90 && a[5] == 0xEA);
	memcpy(b, a, 6);
	CHECK(mc_addr_equal(a, 'r', b, 1));
	CHECK(!mc_addr_equal(a, 'r', b, 0));                 /* wrong type */
	CHECK(!mc_addr_equal(a, 'p', b, 1));
	CHECK(mc_addr_equal(a, 'p', b, 0));
	b[0] ^= 1;                                           /* the "+1" bootloader neighbour */
	CHECK(!mc_addr_equal(a, 'r', b, 1));
	b[0] = 0x91; b[5] ^= 0x80;
	CHECK(!mc_addr_equal(a, 'r', b, 1));
}

/* --------------------------------------------------------------- progress -- */

static struct dfu_status_event ev(uint8_t st, uint8_t pct, uint8_t res, uint8_t att)
{
	struct dfu_status_event e = { st, pct, res, att, 5 };
	return e;
}

static size_t prog(struct mc_progress *p, struct dfu_status_event e, struct mc_progress_msg *m)
{
	return mc_progress_update(p, &e, m);
}

static void test_progress(void)
{
	struct mc_progress p;
	struct mc_progress_msg m[MC_PROGRESS_MAX_MSGS];
	size_t n;
	SECTION("progress");

	mc_progress_begin(&p);
	CHECK(prog(&p, ev(DFU_STATUS_SCANNING, 0, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_CONNECTING, 0, 0, 1), m) == 0);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 0, 0, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_MILESTONE && m[0].milestone == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 12, 0, 1), m) == 0);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 25, 0, 1), m);
	CHECK(n == 1 && m[0].milestone == 25);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 25, 0, 1), m) == 0);   /* never twice */
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 51, 0, 1), m);
	CHECK(n == 1 && m[0].milestone == 50);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 99, 0, 1), m);
	CHECK(n == 1 && m[0].milestone == 75);
	/* the 100% tick was coalesced away; moving on to VALIDATING proves it */
	n = prog(&p, ev(DFU_STATUS_VALIDATING, 99, 0, 1), m);
	CHECK(n == 1 && m[0].milestone == 100);
	CHECK(prog(&p, ev(DFU_STATUS_DISCONNECTING, 99, 0, 1), m) == 0);
	n = prog(&p, ev(DFU_STATUS_DONE, 99, DFU_STATUS_RESULT_OK, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_DONE);
	CHECK(prog(&p, ev(DFU_STATUS_DONE, 99, DFU_STATUS_RESULT_OK, 1), m) == 0);   /* once */

	/* a jump over milestones still reports each, in order */
	mc_progress_begin(&p);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 80, 0, 1), m);
	CHECK(n == 4);
	CHECK(m[0].milestone == 0 && m[1].milestone == 25 && m[2].milestone == 50 && m[3].milestone == 75);

	/* failure mid-transfer: milestones so far, then one FAILED with the reason */
	mc_progress_begin(&p);
	prog(&p, ev(DFU_STATUS_UPLOADING, 30, 0, 1), m);
	n = prog(&p, ev(DFU_STATUS_FAILED, 30, DFU_STATUS_RESULT_TIMEOUT, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_FAILED && m[0].result == DFU_STATUS_RESULT_TIMEOUT);
	CHECK(prog(&p, ev(DFU_STATUS_FAILED, 30, DFU_STATUS_RESULT_TIMEOUT, 1), m) == 0);

	/* a retry is announced but does not repeat milestones already sent */
	mc_progress_begin(&p);
	prog(&p, ev(DFU_STATUS_UPLOADING, 55, 0, 1), m);                 /* 0,25,50 */
	n = prog(&p, ev(DFU_STATUS_SCANNING, 0, 0, 2), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_RETRY && m[0].attempt == 2);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 30, 0, 2), m) == 0);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 76, 0, 2), m);
	CHECK(n == 1 && m[0].milestone == 75);

	/* nothing after the run is ended */
	mc_progress_begin(&p);
	mc_progress_end(&p);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 50, 0, 1), m) == 0);
}

/* ----------------------------------------------------------- replay/evlog -- */

static void test_replay_evlog(void)
{
	struct mc_replay r;
	SECTION("replay");
	mc_replay_init(&r, 1000);
	CHECK(mc_replay_accept(&r, 999, 1) == MC_REPLAY_STALE);
	CHECK(mc_replay_accept(&r, 1000, 1) == MC_REPLAY_OK);
	CHECK(mc_replay_accept(&r, 1000, 1) == MC_REPLAY_DUPLICATE);
	CHECK(mc_replay_accept(&r, 1000, 2) == MC_REPLAY_OK);      /* same second, new message */
	CHECK(mc_replay_accept(&r, 1001, 3) == MC_REPLAY_OK);
	CHECK(mc_replay_accept(&r, 1000, 4) == MC_REPLAY_STALE);   /* older than newest */
	CHECK(r.newest_ts == 1001);
	CHECK(mc_replay_digest("a", 1) != mc_replay_digest("b", 1));

	SECTION("evlog");
	struct mc_evlog l;
	char out[160];
	mc_evlog_init(&l);
	CHECK(mc_evlog_page(&l, 1, out, sizeof(out)) > 0 && !strcmp(out, "log empty"));
	for (int i = 1; i <= 7; i++) {
		char t[32];
		snprintf(t, sizeof(t), "event %d", i);
		mc_evlog_add(&l, (uint32_t)i * 10, t);
	}
	size_t n = mc_evlog_page(&l, 1, out, sizeof(out));
	CHECK(n == strlen(out) && n < MC_REPLY_MAX);
	CHECK(strstr(out, "log 1/3") && strstr(out, "event 7") && strstr(out, "event 5"));
	CHECK(!strstr(out, "event 4"));
	mc_evlog_page(&l, 3, out, sizeof(out));
	CHECK(strstr(out, "event 1") && !strstr(out, "event 2"));
	mc_evlog_page(&l, 9, out, sizeof(out));
	CHECK(strstr(out, "no page 9"));
	for (int i = 0; i < 100; i++) mc_evlog_add(&l, 1, "x");        /* wraps cleanly */
	CHECK(l.count == MC_EVLOG_ENTRIES);
	mc_evlog_add(&l, 5, "this event text is far longer than the forty-four characters kept");
	mc_evlog_page(&l, 1, out, sizeof(out));
	CHECK(strlen(out) < MC_REPLY_MAX);
}

/* -------------------------------------------------------------- controller -- */

struct fake {
	uint32_t now;
	int start_calls, abort_calls, watch_calls;
	char start_bundle[64], start_addr[24];
	char start_type;
	bool busy;
	int start_rc;
	char watching[24];
	char reports[16][160];
	int nreports;
	char notes[64][160];
	int nnotes;
	bool have_bundle;
};

static uint32_t f_now(void *c) { return ((struct fake *)c)->now; }
static size_t f_scan(void *c, struct mc_scan_row *rows, size_t max, size_t off, size_t *total)
{
	(void)c;
	static const struct mc_scan_row all[5] = {
		{ "EA:7C:11:0B:55:90", 'r', -52, "RAK4631_OTA" },
		{ "C0:11:22:33:44:55", 'r', -61, "Xiao" },
		{ "10:22:33:44:55:66", 'p', -70, "" },
		{ "AA:BB:CC:DD:EE:01", 'r', -81, "phone" },
		{ "AA:BB:CC:DD:EE:02", 'r', -90, "tag" },
	};
	*total = 5;
	size_t n = 0;
	for (size_t i = off; i < 5 && n < max; i++) rows[n++] = all[i];
	return n;
}
static void f_watch(void *c, const char *a, char t)
{
	struct fake *f = c; (void)t;
	f->watch_calls++;
	snprintf(f->watching, sizeof(f->watching), "%s", a ? a : "");
}
static int f_bundle(void *c, const char *b) { (void)b; return ((struct fake *)c)->have_bundle ? 0 : -2; }
static int f_start(void *c, const char *b, const char *a, char t)
{
	struct fake *f = c;
	f->start_calls++;
	snprintf(f->start_bundle, sizeof(f->start_bundle), "%s", b);
	snprintf(f->start_addr, sizeof(f->start_addr), "%s", a);
	f->start_type = t;
	return f->start_rc;
}
static int f_abort(void *c) { ((struct fake *)c)->abort_calls++; return 0; }
static bool f_busy(void *c) { return ((struct fake *)c)->busy; }
static int f_batt(void *c, uint16_t *mv, uint8_t *pct, int *chg) { (void)c; *mv = 3912; *pct = 71; *chg = 0; return 0; }
static const char *f_ver(void *c) { (void)c; return "0.1.0+test"; }
static size_t f_logp(void *c, unsigned p, char *o, size_t cap) { (void)c; return (size_t)snprintf(o, cap, "log page %u", p); }
static void f_report(void *c, const char *m)
{
	struct fake *f = c;
	if (f->nreports < 16) snprintf(f->reports[f->nreports++], 160, "%s", m);
}
static void f_note(void *c, const char *m)
{
	struct fake *f = c;
	if (f->nnotes < 64) snprintf(f->notes[f->nnotes++], 160, "%s", m);
}

static const struct mc_ops FOPS = {
	f_now, f_scan, f_watch, f_bundle, f_start, f_abort, f_busy, f_batt, f_ver, f_logp,
	f_report, f_note,
};

static const struct mc_cfg CFG = {
	.rssi_min = -75, .hold_ms = 5000, .max_gap_ms = 3000, .min_samples = 3,
	.arm_timeout_s = 600,
};

static void ctl_setup(struct mc_ctl *c, struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->have_bundle = true;
	mc_ctl_init(c, &FOPS, f, &CFG);
}

static char R[MC_REPLY_MAX + 1];
static size_t H(struct mc_ctl *c, bool authed, const char *t)
{
	size_t n = mc_ctl_handle(c, authed, t, R, sizeof(R));
	CHECK(n == strlen(R) && n >= 1 && n <= MC_REPLY_MAX);
	return n;
}

static void adv_run(struct mc_ctl *c, struct fake *f, uint32_t ms, uint32_t step, int8_t rssi)
{
	for (uint32_t t = 0; t <= ms; t += step) {
		mc_ctl_on_adv(c, rssi);
		f->now += step;
	}
}

#define ARM "arm EA:7C:11:0B:55:90 fw.zip"

static void test_ctl(void)
{
	struct mc_ctl c;
	struct fake f;
	SECTION("controller");

	/* boots disarmed; START is refused */
	ctl_setup(&c, &f);
	CHECK(c.state == MC_DISARMED);
	H(&c, true, "start");
	CHECK(strstr(R, "refused") && f.start_calls == 0);

	/* privileged commands from an unauthorised sender change nothing */
	H(&c, false, ARM);
	CHECK(strstr(R, "DENIED") && c.state == MC_DISARMED && f.watch_calls == 0);
	H(&c, false, "start");
	CHECK(strstr(R, "DENIED") && f.start_calls == 0);
	H(&c, false, "abort");
	CHECK(strstr(R, "DENIED") && f.abort_calls == 0);
	H(&c, false, "disarm");
	CHECK(strstr(R, "DENIED"));

	/* read-only commands do not need the key */
	H(&c, false, "status");   CHECK(strstr(R, "DISARMED"));
	H(&c, false, "version");  CHECK(strstr(R, "0.1.0+test"));
	H(&c, false, "battery");  CHECK(strstr(R, "3.91V") && strstr(R, "71%"));
	H(&c, false, "log 2");    CHECK(!strcmp(R, "log page 2"));
	H(&c, false, "scan");     CHECK(strstr(R, "scan 1/2") && strstr(R, "EA:7C:11:0B:55:90 r -52 RAK4631_OTA"));
	H(&c, false, "scan 2");   CHECK(strstr(R, "scan 2/2") && strstr(R, "AA:BB:CC:DD:EE:02"));
	H(&c, false, "scan 3");   CHECK(strstr(R, "no page 3"));
	H(&c, false, "rssi");     CHECK(strstr(R, "not armed"));
	H(&c, false, "bogus");    CHECK(strstr(R, "cmds"));

	/* arm needs an existing bundle */
	f.have_bundle = false;
	H(&c, true, ARM);
	CHECK(strstr(R, "not found") && c.state == MC_DISARMED);
	f.have_bundle = true;

	/* arm; start immediately -> refused, DFU never invoked */
	H(&c, true, ARM);
	CHECK(strstr(R, "ARMED") && c.state == MC_ARMED);
	CHECK(!strcmp(f.watching, "EA:7C:11:0B:55:90"));
	H(&c, true, "start");
	CHECK(strstr(R, "not seen") && f.start_calls == 0);

	/* strong but only 4 s of it -> still refused */
	f.now = 10000;
	adv_run(&c, &f, 3999, 500, -60);
	H(&c, true, "start");
	CHECK(strstr(R, "refused") && f.start_calls == 0);
	H(&c, true, "rssi");
	CHECK(strstr(R, "rssi -60") && strstr(R, "(min -75)"));

	/* a weak dip resets it */
	mc_ctl_on_adv(&c, -90);
	adv_run(&c, &f, 3999, 500, -60);
	H(&c, true, "start");
	CHECK(f.start_calls == 0);

	/* sustained 5 s above threshold -> START accepted, exact target passed through */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 20000;
	adv_run(&c, &f, 5000, 500, -60);
	f.now -= 500;                          /* last sample was at +5000 */
	H(&c, true, "start");
	CHECK(f.start_calls == 1);
	CHECK(!strcmp(f.start_addr, "EA:7C:11:0B:55:90") && !strcmp(f.start_bundle, "fw.zip"));
	CHECK(f.start_type == 'r');
	CHECK(strstr(R, "STARTED") && c.state == MC_RUNNING);
	CHECK(!strcmp(f.watching, ""));       /* arming consumed; watch stopped */

	/* a second START does not start a second run */
	H(&c, true, "start");
	CHECK(f.start_calls == 1 && strstr(R, "already running"));
	H(&c, true, ARM);
	CHECK(strstr(R, "refused") && c.state == MC_RUNNING);
	H(&c, true, "disarm");
	CHECK(strstr(R, "use abort") && c.state == MC_RUNNING);

	/* reports: 0/25/50/75/100 and success, once each */
	struct dfu_status_event e;
	f.nreports = 0;
	e = (struct dfu_status_event){ DFU_STATUS_UPLOADING, 0, 0, 1, 5 }; mc_ctl_on_dfu_event(&c, &e);
	e.percent = 26; mc_ctl_on_dfu_event(&c, &e);
	e.percent = 51; mc_ctl_on_dfu_event(&c, &e);
	e.percent = 76; mc_ctl_on_dfu_event(&c, &e);
	e.state = DFU_STATUS_VALIDATING; e.percent = 99; mc_ctl_on_dfu_event(&c, &e);
	e.state = DFU_STATUS_DONE; e.result = DFU_STATUS_RESULT_OK; mc_ctl_on_dfu_event(&c, &e);
	CHECK(f.nreports == 6);
	CHECK(!strcmp(f.reports[0], "DFU 0%") && !strcmp(f.reports[1], "DFU 25%") &&
	      !strcmp(f.reports[2], "DFU 50%") && !strcmp(f.reports[3], "DFU 75%") &&
	      !strcmp(f.reports[4], "DFU 100%"));
	CHECK(strstr(f.reports[5], "DFU SUCCESS EA:7C:11:0B:55:90"));
	CHECK(c.state == MC_DISARMED && c.last == MC_LAST_OK);
	H(&c, true, "status"); CHECK(!strcmp(R, "DISARMED last=ok"));
	H(&c, true, "start");  CHECK(strstr(R, "not armed") && f.start_calls == 1);

	/* failure is reported with its reason */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60); f.now -= 500;
	H(&c, true, "start");
	e = (struct dfu_status_event){ DFU_STATUS_UPLOADING, 30, 0, 1, 5 };
	mc_ctl_on_dfu_event(&c, &e);
	e = (struct dfu_status_event){ DFU_STATUS_FAILED, 30, DFU_STATUS_RESULT_TIMEOUT, 1, 5 };
	f.nreports = 0;
	mc_ctl_on_dfu_event(&c, &e);
	CHECK(f.nreports == 1 && strstr(f.reports[0], "DFU FAILED") && strstr(f.reports[0], "timeout"));
	H(&c, true, "status"); CHECK(strstr(R, "last=failed (timeout)"));

	/* a runner reset from elsewhere ends the run visibly */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60); f.now -= 500;
	H(&c, true, "start");
	e = (struct dfu_status_event){ DFU_STATUS_IDLE, 0, 0, 0, 0 };
	mc_ctl_on_dfu_event(&c, &e);
	CHECK(c.state == MC_DISARMED && strstr(f.reports[f.nreports - 1], "stopped"));

	/* abort while running */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60); f.now -= 500;
	H(&c, true, "start");
	H(&c, true, "abort");
	CHECK(f.abort_calls == 1 && c.state == MC_DISARMED && c.last == MC_LAST_ABORTED);
	e = (struct dfu_status_event){ DFU_STATUS_UPLOADING, 40, 0, 1, 5 };
	f.nreports = 0;
	mc_ctl_on_dfu_event(&c, &e);
	CHECK(f.nreports == 0);                 /* nothing reported after abort */

	/* abort / disarm while only armed */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	H(&c, true, "disarm");
	CHECK(c.state == MC_DISARMED && !strcmp(f.watching, ""));
	H(&c, true, ARM);
	H(&c, true, "abort");
	CHECK(c.state == MC_DISARMED && f.abort_calls == 0);
	H(&c, true, "abort");
	CHECK(strstr(R, "nothing running"));

	/* lost samples make the gate start over, so START is refused */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60); f.now -= 500;
	mc_ctl_samples_lost(&c);
	H(&c, true, "start");
	CHECK(f.start_calls == 0);

	/* START while the runner is busy is refused even with a good gate */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60); f.now -= 500;
	f.busy = true;
	H(&c, true, "start");
	CHECK(f.start_calls == 0 && strstr(R, "busy"));
	f.busy = false;
	f.start_rc = -16;
	H(&c, true, "start");
	CHECK(f.start_calls == 1 && c.state == MC_ARMED && strstr(R, "still ARMED"));

	/* the gate is evaluated at START time: a good window that went stale fails */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60);
	f.now += 4000;                          /* target silent for 4 s */
	H(&c, true, "start");
	CHECK(f.start_calls == 0 && strstr(R, "went quiet"));

	/* NO AUTOMATIC START: a perfect gate and a very long wait never begin DFU */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000;
	for (int i = 0; i < 1000; i++) {
		mc_ctl_on_adv(&c, -40);
		mc_ctl_tick(&c);
		f.now += 500;
		if (i == 500) { c.cfg.arm_timeout_s = 0; }   /* never lapses either */
	}
	CHECK(f.start_calls == 0 && c.state == MC_ARMED);

	/* arming lapses */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 599000; mc_ctl_tick(&c); CHECK(c.state == MC_ARMED);
	f.now = 600000; mc_ctl_tick(&c);
	CHECK(c.state == MC_DISARMED && strstr(f.reports[f.nreports - 1], "expired"));

	/* re-arming a different target replaces the first and restarts the gate */
	ctl_setup(&c, &f);
	H(&c, true, ARM);
	f.now = 1000; adv_run(&c, &f, 5000, 500, -60);
	H(&c, true, "arm C0:11:22:33:44:55 other.zip");
	CHECK(!strcmp(c.addr, "C0:11:22:33:44:55") && !c.gate.seen_any);
	H(&c, true, "start");
	CHECK(f.start_calls == 0);

	/* every reply fits one MeshCore message, even with the longest arguments */
	ctl_setup(&c, &f);
	char longarm[160];
	snprintf(longarm, sizeof(longarm), "arm EA:7C:11:0B:55:90 %s public",
		 "abcdefghijklmnopqrstuvwxyz0123456789.zip");
	H(&c, true, longarm);
	H(&c, true, "start");
	H(&c, true, "status");
	f.now = 1000; adv_run(&c, &f, 5000, 500, -127 + 60);
}

/* ------------------------------------------------------- wire vs MeshCore -- */

static void test_wire_vectors(void)
{
	SECTION("wire (vs MeshCore-generated vectors)");

	/* Same seeds -> same identities as MeshCore's LocalIdentity(RNG*) */
	uint8_t seed[32];
	struct mc_id node, op;
	seed_for(0x10, seed);
	CHECK(mc_id_from_seed(&node, seed));
	CHECK(!memcmp(node.pub, V_node_pub, 32));
	seed_for(0x55, seed);
	CHECK(mc_id_from_seed(&op, seed));
	CHECK(!memcmp(op.pub, V_op_pub, 32));

	/* ECDH agrees with MeshCore from both sides */
	mc_id_set_operator(&node, op.pub);
	CHECK(!memcmp(node.secret, V_secret_node, 32));
	CHECK(!memcmp(V_secret_node, V_secret_op, 32));

	/* Sealing a text with the operator-side secret reproduces MeshCore's bytes */
	uint8_t frame[MC_MAX_RAW];
	size_t n = mc_wire_build_txt(CRY, V_secret_op, node.pub[0], op.pub[0],
				     1760000000u, MC_TXT_PLAIN, "arm EA:7C:11:0B:55:90 fw.zip",
				     frame, sizeof(frame));
	CHECK(n == 2 + 2 + V_sealed_len);
	CHECK(frame[0] == 0x09 && frame[1] == 0x00);              /* TXT_MSG, flood, no path */
	CHECK(frame[2] == node.pub[0] && frame[3] == op.pub[0]);
	CHECK(!memcmp(&frame[4], V_sealed, V_sealed_len));        /* mac + ciphertext */

	/* ...and our receive path opens MeshCore's own packet */
	struct mc_pkt pkt;
	CHECK(mc_wire_parse(frame, n, &pkt) == 0);
	CHECK(pkt.ptype == MC_PT_TXT_MSG && mc_wire_is_flood(&pkt) && pkt.path_bytes == 0);
	uint8_t plain[MC_MAX_PACKET_PAYLOAD];
	int pl = mc_wire_open(CRY, node.secret, pkt.payload, pkt.payload_len,
			      plain, sizeof(plain));
	CHECK(pl == 48);
	CHECK(!memcmp(plain, V_plain, V_plain_len));
	struct mc_txt t;
	CHECK(mc_wire_parse_txt(plain, (size_t)pl, &t) == 0);
	CHECK(t.ts == 1760000000u && t.txt_type == 0 && !strcmp(t.text, "arm EA:7C:11:0B:55:90 fw.zip"));

	/* A flipped bit anywhere in mac or ciphertext is refused */
	for (size_t i = 4; i < n; i++) {
		uint8_t bad[MC_MAX_RAW];
		memcpy(bad, frame, n);
		bad[i] ^= 0x01;
		struct mc_pkt bp;
		CHECK(mc_wire_parse(bad, n, &bp) == 0);
		CHECK(mc_wire_open(CRY, node.secret, bp.payload, bp.payload_len,
				   plain, sizeof(plain)) < 0);
	}
	/* A secret from any other key does not open it */
	uint8_t other[32];
	memcpy(other, node.secret, 32);
	other[31] ^= 1;
	CHECK(mc_wire_open(CRY, other, pkt.payload, pkt.payload_len, plain, sizeof(plain)) < 0);

	/* ACK bytes match MeshCore's sha256(plain[0..5+len) || sender_pub)[0..3] */
	uint8_t ack[6];
	mc_wire_ack_bytes(CRY, V_plain, V_plain_len + 0, strlen("arm EA:7C:11:0B:55:90 fw.zip"),
			  op.pub, 0xAB, ack);
	CHECK(!memcmp(ack, V_ack4, 4) && ack[5] == 0xAB);

	/* Signed advert: signature identical to MeshCore's (Ed25519 is deterministic) */
	uint8_t adv[MC_MAX_RAW];
	size_t an = mc_wire_build_advert(node.pub, node.prv, 1760000100u, "Dr", adv, sizeof(adv));
	CHECK(an == 2 + 32 + 4 + 64 + 3);
	CHECK(adv[0] == 0x11);                                    /* ADVERT, flood */
	CHECK(!memcmp(&adv[2], node.pub, 32));
	CHECK(!memcmp(&adv[2 + 32], &V_adv_msg[32], 4));          /* timestamp */
	CHECK(!memcmp(&adv[2 + 36], V_adv_sig, 64));              /* signature */
	CHECK(adv[2 + 36 + 64] == 0x81 && adv[2 + 36 + 64 + 1] == 'D');   /* chat | has-name */
	CHECK(V_adv_msg_len == 39);

	/* Parser bounds */
	CHECK(mc_wire_parse(frame, 1, &pkt) < 0);
	uint8_t bad[8] = { 0x09, 0xC1, 1, 2, 3, 4, 5, 6 };        /* hash-size code 3 */
	CHECK(mc_wire_parse(bad, sizeof(bad), &pkt) < 0);
	uint8_t nopay[3] = { 0x09, 0x01, 0xAA };                  /* path eats the payload */
	CHECK(mc_wire_parse(nopay, sizeof(nopay), &pkt) < 0);
	uint8_t withpath[6] = { 0x09, 0x02, 0x11, 0x22, 0x33, 0x44 };
	CHECK(mc_wire_parse(withpath, sizeof(withpath), &pkt) == 0 && pkt.path_bytes == 2 &&
	      pkt.payload_len == 2 && pkt.payload[0] == 0x33);

	CHECK(mc_hex32("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef", seed));
	CHECK(!mc_hex32("0123", seed));
	CHECK(!mc_hex32("zz23456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef", seed));
}

/* ------------------------------------------------- node: packets -> acts -- */

struct sink {
	uint32_t now;
	uint8_t frames[16][MC_MAX_RAW];
	size_t lens[16];
	int n;
	int persists;
	struct mc_persist last;
};
static void s_tx(void *c, const uint8_t *f, size_t l)
{
	struct sink *s = c;
	if (s->n < 16) { memcpy(s->frames[s->n], f, l); s->lens[s->n++] = l; }
}
static void s_persist(void *c, const struct mc_persist *p) { struct sink *s = c; s->persists++; s->last = *p; }
static void s_note(void *c, const char *m) { (void)c; (void)m; }
static uint32_t s_now(void *c) { return ((struct sink *)c)->now; }
static uint8_t s_rand(void *c) { (void)c; return 0x5A; }

static const struct mc_node_ops NOPS = { s_tx, s_persist, s_note, s_now, s_rand };

struct rig {
	struct mc_id node, op;
	struct mc_ctl ctl;
	struct fake f;
	struct mc_node nd;
	struct sink s;
};

static void rig_setup(struct rig *r, uint32_t persisted_ts)
{
	uint8_t seed[32];
	memset(r, 0, sizeof(*r));
	seed_for(0x10, seed); mc_id_from_seed(&r->node, seed);
	seed_for(0x55, seed); mc_id_from_seed(&r->op, seed);
	mc_id_set_operator(&r->node, r->op.pub);
	ctl_setup(&r->ctl, &r->f);
	struct mc_persist p = { persisted_ts, 0 };
	mc_node_init(&r->nd, CRY, &r->node, &r->ctl, &NOPS, &r->s, &p);
}

/* A frame the operator's MeshCore app would send, built with the real secret. */
static size_t op_frame(const struct rig *r, uint32_t ts, uint8_t type, const char *text, uint8_t *out)
{
	return mc_wire_build_txt(CRY, r->node.secret, r->node.pub[0], r->op.pub[0],
				 ts, type, text, out, MC_MAX_RAW);
}

/* Decrypt one of the node's frames back into text, as the operator would. */
static bool decode_reply(const struct rig *r, const uint8_t *fr, size_t len, struct mc_txt *t)
{
	struct mc_pkt p;
	uint8_t plain[MC_MAX_PACKET_PAYLOAD];
	if (mc_wire_parse(fr, len, &p) || p.ptype != MC_PT_TXT_MSG) return false;
	if (p.payload[0] != r->op.pub[0] || p.payload[1] != r->node.pub[0]) return false;
	int n = mc_wire_open(CRY, r->node.secret, p.payload, p.payload_len, plain, sizeof(plain));
	return n > 0 && mc_wire_parse_txt(plain, (size_t)n, t) == 0;
}

static void test_node(void)
{
	struct rig r;
	uint8_t fr[MC_MAX_RAW];
	struct mc_txt t;
	size_t n;
	SECTION("node (packet -> decision)");

	/* operator "arm": ACK + reply, state changes, timestamp persisted */
	rig_setup(&r, 1000);
	n = op_frame(&r, 2000, MC_TXT_PLAIN, ARM, fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.ctl.state == MC_ARMED);
	CHECK(r.s.n == 2);                                   /* ACK, then reply */
	struct mc_pkt p0;
	CHECK(mc_wire_parse(r.s.frames[0], r.s.lens[0], &p0) == 0 && p0.ptype == MC_PT_PATH);
	CHECK(decode_reply(&r, r.s.frames[1], r.s.lens[1], &t) && strstr(t.text, "ARMED"));
	CHECK(r.s.persists == 1 && r.s.last.replay_ts == 2000);

	/* the same packet again (a repeater's copy, or a retry): acked, not re-run */
	r.s.n = 0;
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 1);                                   /* ACK only, no reply */
	CHECK(r.f.watch_calls == 1);

	/* a recorded older command is dropped without a trace on the air */
	r.s.n = 0;
	n = op_frame(&r, 1500, MC_TXT_PLAIN, "disarm", fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 0 && r.ctl.state == MC_ARMED);

	/* a frame sealed with the wrong key never reaches the controller */
	struct mc_id intruder;
	uint8_t seed[32];
	seed_for(0x77, seed);
	mc_id_from_seed(&intruder, seed);
	uint8_t isec[32];
	mc_id_set_operator(&intruder, r.node.pub);            /* intruder <-> node secret */
	memcpy(isec, intruder.secret, 32);
	r.s.n = 0;
	/* claims to be the operator (same source hash byte) */
	n = mc_wire_build_txt(CRY, isec, r.node.pub[0], r.op.pub[0], 3000,
			      MC_TXT_PLAIN, "disarm", fr, sizeof(fr));
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 0 && r.ctl.state == MC_ARMED && r.nd.rx_bad >= 1);

	/* ...even knowing the operator's public key: that yields the wrong secret */
	uint8_t wrong[32];
	memset(wrong, 0, 32);
	n = mc_wire_build_txt(CRY, wrong, r.node.pub[0], r.op.pub[0], 3001,
			      MC_TXT_PLAIN, "start", fr, sizeof(fr));
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 0 && r.f.start_calls == 0);

	/* traffic for another node, or from another sender, is ignored */
	n = mc_wire_build_txt(CRY, r.node.secret, (uint8_t)(r.node.pub[0] ^ 1), r.op.pub[0],
			      3002, MC_TXT_PLAIN, "disarm", fr, sizeof(fr));
	mc_node_rx(&r.nd, fr, n);
	n = mc_wire_build_txt(CRY, r.node.secret, r.node.pub[0], (uint8_t)(r.op.pub[0] ^ 1),
			      3003, MC_TXT_PLAIN, "disarm", fr, sizeof(fr));
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 0 && r.ctl.state == MC_ARMED);

	/* garbage and truncations never crash */
	for (size_t l = 0; l < 40; l++) {
		uint8_t junk[40];
		for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 31 + l);
		mc_node_rx(&r.nd, junk, l);
	}
	CHECK(r.ctl.state == MC_ARMED);

	/* a CLI-type command is answered in kind, and (per MeshCore) not ACKed */
	rig_setup(&r, 0);
	n = op_frame(&r, 10, MC_TXT_CLI, "version", fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 1);
	CHECK(decode_reply(&r, r.s.frames[0], r.s.lens[0], &t) && t.txt_type == MC_TXT_CLI &&
	      strstr(t.text, "0.1.0+test"));

	/* end to end: arm, hold the gate for 5 s, START, milestones go out */
	rig_setup(&r, 0);
	n = op_frame(&r, 100, MC_TXT_PLAIN, ARM, fr);
	mc_node_rx(&r.nd, fr, n);
	r.s.n = 0;
	r.f.now = 1000;
	adv_run(&r.ctl, &r.f, 3000, 500, -60);
	n = op_frame(&r, 101, MC_TXT_PLAIN, "start", fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.f.start_calls == 0);                         /* only ~3 s held */
	CHECK(decode_reply(&r, r.s.frames[r.s.n - 1], r.s.lens[r.s.n - 1], &t) && strstr(t.text, "refused"));
	adv_run(&r.ctl, &r.f, 2500, 500, -60);
	r.f.now -= 500;
	n = op_frame(&r, 102, MC_TXT_PLAIN, "start", fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.f.start_calls == 1 && r.ctl.state == MC_RUNNING);

	/* adverts: signed, newer than any before, persisted */
	rig_setup(&r, 5000);
	r.s.n = 0;
	mc_node_advertise(&r.nd, "Drone Updater");
	CHECK(r.s.n == 1 && r.s.frames[0][0] == 0x11 && r.s.last.adv_ts >= 5000);
	uint32_t first = r.s.last.adv_ts;
	mc_node_advertise(&r.nd, "Drone Updater");
	CHECK(r.s.last.adv_ts > first);
	/* after a reboot the persisted floor keeps it increasing */
	struct mc_persist keep = r.s.last;
	struct rig r2;
	rig_setup(&r2, 0);
	mc_node_init(&r2.nd, CRY, &r2.node, &r2.ctl, &NOPS, &r2.s, &keep);
	mc_node_advertise(&r2.nd, "Drone Updater");
	CHECK(r2.s.last.adv_ts > keep.adv_ts);
}

/* ---- builtin crypto vs OpenSSL ---------------------------------------- */
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static void rfill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

static void test_crypto_builtin(void)
{
	printf("crypto builtin\n");
	/* FIPS-197 C.1 */
	const uint8_t k[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
	const uint8_t pt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
	const uint8_t ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
	uint8_t o[16];
	mc_aes128_ecb_encrypt(k, pt, o, 16);
	CHECK(!memcmp(o, ct, 16));
	mc_aes128_ecb_decrypt(k, ct, o, 16);
	CHECK(!memcmp(o, pt, 16));
	/* SHA-256("abc") */
	const uint8_t abc[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
				 0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
	uint8_t h[32], h2[32];
	mc_sha256((const uint8_t *)"abc", 3, NULL, 0, h);
	CHECK(!memcmp(h, abc, 32));

	/* compact SHA-512 (used by Ed25519) vs OpenSSL, every length around the block edges */
	for (size_t len = 0; len < 400; len++) {
		uint8_t m[400], d1[64], d2[64];
		unsigned int dl = 64;
		rfill(m, sizeof m);
		sha512(m, len, d1);
		EVP_Digest(m, len, d2, &dl, EVP_sha512(), NULL);
		CHECK(!memcmp(d1, d2, 64));
	}
	for (int i = 0; i < 4000; i++) {
		uint8_t key[32], buf[300], a[300], b[300];
		size_t la = rnd() % 200, lb = rnd() % 100;
		rfill(key, 32);
		rfill(buf, sizeof buf);
		mc_sha256(buf, la, buf + 100, lb, h);
		TEST_CRYPTO.sha256(buf, la, buf + 100, lb, h2);
		CHECK(!memcmp(h, h2, 32));
		mc_hmac_sha256(key, buf, la + lb, h);
		TEST_CRYPTO.hmac_sha256(key, buf, la + lb, h2);
		CHECK(!memcmp(h, h2, 32));
		size_t blocks = (rnd() % 12) * 16;
		mc_aes128_ecb_encrypt(key, buf, a, blocks);
		TEST_CRYPTO.aes128_ecb_encrypt(key, buf, b, blocks);
		CHECK(!memcmp(a, b, blocks));
		mc_aes128_ecb_decrypt(key, buf, a, blocks);
		TEST_CRYPTO.aes128_ecb_decrypt(key, buf, b, blocks);
		CHECK(!memcmp(a, b, blocks));
	}
}

static void test_sx126x(void)
{
	printf("sx126x\n");
	CHECK(mc_sx_bw_code(62500) == 0x03 && mc_sx_bw_code(125000) == 0x04 &&
	      mc_sx_bw_code(250000) == 0x05 && mc_sx_bw_code(500000) == 0x06);
	CHECK(mc_sx_bw_code(62000) < 0 && mc_sx_bw_code(0) < 0);
	CHECK(mc_sx_freq_word(910525000u) == (uint32_t)(910525000ull * 1048576ull / 1000000ull));
	CHECK(mc_sx_freq_word(868000000u) == 910163968u);
	CHECK(!mc_sx_ldro(7, 62500) && !mc_sx_ldro(7, 125000));
	CHECK(mc_sx_ldro(12, 125000) && mc_sx_ldro(11, 125000) && !mc_sx_ldro(10, 125000));
	CHECK(mc_sx_ldro(10, 62500));
	/* SF7 / 62.5k / CR4/5 / 16 preamble / 50 B: 103.25 symbols * 2.048 ms */
	CHECK(mc_sx_airtime_ms(7, 62500, 5, 16, 50) == 212);
	/* SF7 / 125k / 4/5 / 8 preamble / 10 B -> the textbook ~41.2 ms */
	CHECK(mc_sx_airtime_ms(7, 125000, 5, 8, 10) == 42);
	CHECK(mc_sx_airtime_ms(7, 62500, 5, 16, 100) > mc_sx_airtime_ms(7, 62500, 5, 16, 50));
	CHECK(mc_sx_airtime_ms(13, 62500, 5, 16, 50) == 0);
	uint8_t m[4], p[6], sw[2];
	CHECK(mc_sx_modparams(7, 62500, 5, m) && m[0] == 7 && m[1] == 3 && m[2] == 1 && m[3] == 0);
	CHECK(!mc_sx_modparams(7, 63000, 5, m) && !mc_sx_modparams(7, 62500, 9, m));
	mc_sx_pktparams(16, 200, p);
	CHECK(p[0] == 0 && p[1] == 16 && p[2] == 0 && p[3] == 200 && p[4] == 1 && p[5] == 0);
	mc_sx_syncword_regs(0x12, sw);
	CHECK(sw[0] == 0x14 && sw[1] == 0x24);
	mc_sx_syncword_regs(0x34, sw);
	CHECK(sw[0] == 0x34 && sw[1] == 0x44);
	uint8_t ic[2];
	mc_sx_image_cal(910525000u, ic);
	CHECK(ic[0] == 0xE1 && ic[1] == 0xE9);
	mc_sx_image_cal(869525000u, ic);
	CHECK(ic[0] == 0xD7 && ic[1] == 0xDB);
	mc_sx_image_cal(433000000u, ic);
	CHECK(ic[0] == 0x6B && ic[1] == 0x6F);
	CHECK(mc_sx_rssi_dbm(160) == -80 && mc_sx_rssi_dbm(0) == 0);
	CHECK(mc_sx_snr_q2(0xF8) == -8 && mc_sx_snr_q2(20) == 20);
}

/* ---- store: RAM flash with NOR semantics and power-cut injection ---------- */
struct ramflash {
	uint8_t page[MC_STORE_PAGE];
	int ops;            /* write/erase operations performed so far */
	int cut_at;         /* operation number that loses power (-1 = never) */
	int cut_partial;    /* bytes of that write that land before the cut */
	int violations;     /* writes that tried to set a 0 bit back to 1 */
	int dead;
};
static int rf_read(void *c, uint32_t off, void *b, size_t n)
{
	struct ramflash *f = c;
	if (off + n > MC_STORE_PAGE) return -1;
	memcpy(b, f->page + off, n);
	return 0;
}
static int rf_write(void *c, uint32_t off, const void *b, size_t n)
{
	struct ramflash *f = c;
	const uint8_t *p = b;
	if (f->dead) return -1;
	if ((off & 3) || (n & 3) || off + n > MC_STORE_PAGE) { f->violations++; return -1; }
	size_t land = n;
	if (f->cut_at >= 0 && f->ops == f->cut_at) { land = (size_t)f->cut_partial < n ? (size_t)f->cut_partial : n; f->dead = 1; }
	f->ops++;
	for (size_t i = 0; i < land; i++) {
		if ((uint8_t)(p[i] & ~f->page[off + i])) f->violations++;
		f->page[off + i] &= p[i];
	}
	return f->dead ? -1 : 0;
}
static int rf_erase(void *c)
{
	struct ramflash *f = c;
	if (f->dead) return -1;
	if (f->cut_at >= 0 && f->ops == f->cut_at) {
		memset(f->page, 0xFF, MC_STORE_PAGE / 2);    /* half-erased */
		f->dead = 1; f->ops++; return -1;
	}
	f->ops++;
	memset(f->page, 0xFF, MC_STORE_PAGE);
	return 0;
}
static void rf_init(struct ramflash *f) { memset(f, 0, sizeof(*f)); memset(f->page, 0xFF, MC_STORE_PAGE); f->cut_at = -1; }

static void test_store(void)
{
	printf("store\n");
	struct ramflash f; rf_init(&f);
	struct mc_flash fl = { rf_read, rf_write, rf_erase, &f };
	struct mc_store st; struct mc_persist p;
	uint8_t prv[64], pub[32];
	for (int i = 0; i < 64; i++) prv[i] = (uint8_t)(i * 3 + 1);
	for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(i * 5 + 2);

	CHECK(mc_crc32("123456789", 9) == 0xCBF43926u);          /* standard check value */
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_EMPTY);
	CHECK(mc_store_provision(&st, prv, pub) == MC_STORE_OK);
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK);
	CHECK(!memcmp(st.prv, prv, 64) && !memcmp(st.pub, pub, 32) && p.replay_ts == 0 && p.adv_ts == 0);

	struct mc_persist w = { 100, 50 };
	CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	w.replay_ts = 200; w.adv_ts = 60;
	CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK && p.replay_ts == 200 && p.adv_ts == 60);

	/* A corrupted header is "empty", never a half-trusted key. */
	f.page[8] ^= 1;
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_EMPTY);
	f.page[8] ^= 1;
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK && p.replay_ts == 200);

	/* Fill past the page: identity survives, newest record wins, no 0->1 writes. */
	for (uint32_t i = 0; i < 3 * MC_STORE_MAX_RECS + 7; i++) {
		w.replay_ts = 1000 + i; w.adv_ts = 5000 + i;
		CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	}
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK);
	CHECK(p.replay_ts == 1000 + 3 * MC_STORE_MAX_RECS + 6 && !memcmp(st.prv, prv, 64));
	CHECK(f.violations == 0);

	/* Power cut at every operation of a run of appends that includes a rollover.
	 * Outside the rollover the identity must always survive and the counters
	 * must never go backwards past the last acknowledged append. */
	int cut_in_rollover_lost = 0;
	for (int cut = 0; cut < 40; cut++) {
		for (int partial = 0; partial <= 16; partial += 4) {
			struct ramflash g; rf_init(&g);
			struct mc_flash gf = { rf_read, rf_write, rf_erase, &g };
			struct mc_store gs; struct mc_persist gp;
			mc_store_load(&gs, &gf, &gp);
			mc_store_provision(&gs, prv, pub);
			/* run up to just before a rollover */
			uint32_t acked_replay = 0;
			for (uint32_t i = 0; i < MC_STORE_MAX_RECS - 3; i++) {
				struct mc_persist q = { 10 + i, 10 + i };
				mc_store_append(&gs, &q); acked_replay = q.replay_ts;
			}
			g.ops = 0; g.cut_at = cut; g.cut_partial = partial;
			int in_roll = 0;
			for (uint32_t i = 0; i < 8 && !g.dead; i++) {
				struct mc_persist q = { 1000 + i, 1000 + i };
				if (gs.next_slot >= MC_STORE_MAX_RECS) in_roll = 1;
				int rc = mc_store_append(&gs, &q);
				if (rc == MC_STORE_OK) acked_replay = q.replay_ts;
			}
			struct mc_store ls; struct mc_persist lp;
			int rc = mc_store_load(&ls, &gf, &lp);
			if (rc == MC_STORE_OK) {
				CHECK(!memcmp(ls.prv, prv, 64));
				CHECK(lp.replay_ts >= acked_replay || (in_roll && lp.replay_ts == 0));
			} else {
				CHECK(rc == MC_STORE_EMPTY && g.dead);
				CHECK(in_roll);              /* only the documented window may lose the key */
				cut_in_rollover_lost++;
			}
			CHECK(g.violations == 0);
		}
	}
	CHECK(cut_in_rollover_lost > 0);   /* the window is real; the test must see it */
}

int main(void)
{
	load_vectors("vectors/meshcore_vectors.txt");
	test_gate();
	test_cmd();
	test_progress();
	test_replay_evlog();
	test_ctl();
	test_store();
	test_sx126x();
	test_crypto_builtin();
	const struct mc_crypto *both[2] = { &TEST_CRYPTO, &MC_CRYPTO_BUILTIN };
	for (int c = 0; c < 2; c++) {
		CRY = both[c];
		printf("wire/node with %s crypto\n", c ? "builtin" : "openssl");
		test_wire_vectors();
		test_node();
	}
	printf("\n%d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
