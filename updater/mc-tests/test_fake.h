/* Shared fakes for the controller and node tests: a scriptable platform. */
#ifndef TEST_FAKE_H
#define TEST_FAKE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mc/mc_authz.h"
#include "../src/mc/mc_ctl.h"

#define UNUSED __attribute__((unused))

extern int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
	printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define SECTION(n) printf("%s\n", n)

#define WHO_MC     ((uintptr_t)0x1000)    /* the mc thread */
#define WHO_OTHER  ((uintptr_t)0x2000)    /* any other thread */

struct fake {
	uint32_t now;
	struct mc_ctl *ctl;            /* to observe the state at dfu_start */
	struct mc_authz az;

	int start_calls, abort_calls, watch_calls, end_calls, lock_calls, unlock_calls;
	char start_bundle[64], start_addr[24];
	char start_type;
	bool start_plus1;
	enum mc_state state_at_start;
	enum mc_authz_state az_at_start;
	bool runner_consumed;          /* the simulated runner got the authorisation */
	bool bypass_ok;                /* an "other thread" consume during START worked */
	bool busy;
	int start_rc;
	bool dry;                      /* simulate a CONFIG_MC_DFU_DRY_RUN build */
	int diag_calls;

	char watching[24];
	char locked[64];

	char content[64];              /* what the "file" currently contains */
	bool have_bundle;
	int digest_calls;

	uint32_t rnd;
	char reports[32][160];
	int nreports;
	char notes[128][160];
	int nnotes;
};

static UNUSED uint32_t f_now(void *c) { return ((struct fake *)c)->now; }
static UNUSED uint32_t f_rand(void *c)
{
	struct fake *f = c;
	f->rnd = f->rnd * 1664525u + 1013904223u;
	return f->rnd;
}
static UNUSED size_t f_scan(void *c, struct mc_scan_row *rows, size_t max, size_t off, size_t *total)
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
static UNUSED void f_watch(void *c, const char *a, char t)
{
	struct fake *f = c; (void)t;
	f->watch_calls++;
	snprintf(f->watching, sizeof(f->watching), "%s", a ? a : "");
}
/* A digest that depends on the content, not the name: FNV-ish spread over 32 bytes. */
static UNUSED int f_digest(void *c, const char *b, uint8_t out[32])
{
	struct fake *f = c; (void)b;
	f->digest_calls++;
	if (!f->have_bundle) return -2;
	uint32_t h = 2166136261u;
	for (int i = 0; i < 32; i++) {
		for (const char *p = f->content; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
		h ^= (uint32_t)i * 0x9E3779B1u;
		out[i] = (uint8_t)(h >> 13);
	}
	return 0;
}
static UNUSED void f_lock(void *c, const char *b)
{
	struct fake *f = c;
	if (b) { f->lock_calls++; snprintf(f->locked, sizeof(f->locked), "%s", b); }
	else { f->unlock_calls++; f->locked[0] = '\0'; }
}
/* Mirrors the Zephyr glue: grant -> (runner consumes) -> return. Also probes the
 * authorisation from "another thread" while it is live. */
static UNUSED int f_start(void *c, const char *b, const char *a, char t, bool plus1)
{
	struct fake *f = c;
	char path[80], pin[40];
	f->start_calls++;
	snprintf(f->start_bundle, sizeof(f->start_bundle), "%s", b);
	snprintf(f->start_addr, sizeof(f->start_addr), "%s", a);
	f->start_type = t; f->start_plus1 = plus1;
	f->state_at_start = f->ctl->state;
	snprintf(path, sizeof(path), "/lfs1/%s", b);
	snprintf(pin, sizeof(pin), "%s (%s)", a, t == 'p' ? "public" : "random");
	if (!mc_authz_grant(&f->az, WHO_MC, path, pin)) return -16;
	f->az_at_start = mc_authz_state(&f->az);
	/* an unrelated caller (SMP trigger, auto_flash) tries to use it */
	f->bypass_ok = mc_authz_consume(&f->az, WHO_OTHER, path, pin);
	if (f->start_rc == 0) f->runner_consumed = mc_authz_consume(&f->az, WHO_MC, path, pin);
	if (f->dry && f->start_rc == 0) return MC_START_DRY_RUN;
	return f->start_rc;
}
static UNUSED void f_end(void *c) { struct fake *f = c; f->end_calls++; mc_authz_end(&f->az); }
static UNUSED int f_abort(void *c) { ((struct fake *)c)->abort_calls++; return 0; }
static UNUSED bool f_busy(void *c) { return ((struct fake *)c)->busy; }
static UNUSED int f_batt(void *c, uint16_t *mv, uint8_t *pct, int *chg) { (void)c; *mv = 3912; *pct = 71; *chg = 0; return 0; }
static UNUSED const char *f_ver(void *c) { (void)c; return "0.1.0+test"; }
static UNUSED size_t f_logp(void *c, unsigned p, char *o, size_t cap) { (void)c; return (size_t)snprintf(o, cap, "log page %u", p); }
static UNUSED void f_report(void *c, const char *m)
{
	struct fake *f = c;
	if (f->nreports < 32) snprintf(f->reports[f->nreports++], 160, "%s", m);
}
static UNUSED void f_note(void *c, const char *m)
{
	struct fake *f = c;
	if (f->nnotes < 128) snprintf(f->notes[f->nnotes++], 160, "%s", m);
}

static UNUSED size_t f_diag(void *c, int id, char *o, size_t cap)
{
	((struct fake *)c)->diag_calls++;
	return (size_t)snprintf(o, cap, "diag-%s", mc_cmd_name((enum mc_cmd_id)id));
}

static const UNUSED struct mc_ops FOPS = {
	.now_ms = f_now, .rand32 = f_rand, .scan_rows = f_scan, .watch = f_watch,
	.bundle_digest = f_digest, .bundle_lock = f_lock, .dfu_start = f_start,
	.dfu_end = f_end, .dfu_abort = f_abort, .dfu_busy = f_busy, .battery = f_batt,
	.version = f_ver, .log_page = f_logp, .report = f_report, .note = f_note,
};

static const UNUSED struct mc_ops FOPS_DIAG = {
	.now_ms = f_now, .rand32 = f_rand, .scan_rows = f_scan, .watch = f_watch,
	.bundle_digest = f_digest, .bundle_lock = f_lock, .dfu_start = f_start,
	.dfu_end = f_end, .dfu_abort = f_abort, .dfu_busy = f_busy, .battery = f_batt,
	.version = f_ver, .log_page = f_logp, .report = f_report, .note = f_note,
	.diag = f_diag,
};

static const UNUSED struct mc_cfg CFG = {
	.gate = { .avg_min = -65, .floor = -75, .hyst_db = 3, .hold_ms = 5000,
		  .max_gap_ms = 3000, .min_samples = 8 },
	.arm_timeout_s = 60, .token_ttl_s = 120, .report_gap_ms = 4000,
};

static UNUSED void ctl_setup(struct mc_ctl *c, struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->have_bundle = true;
	f->rnd = 12345;
	strcpy(f->content, "firmware v1");
	f->ctl = c;
	mc_authz_init(&f->az);
	mc_ctl_init(c, &FOPS, f, &CFG);
}

static UNUSED char R[MC_REPLY_MAX + 1];
static UNUSED size_t H(struct mc_ctl *c, bool authed, const char *t)
{
	size_t n = mc_ctl_handle(c, authed, t, R, sizeof(R));
	CHECK(n == strlen(R) && n >= 1 && n <= MC_REPLY_MAX);
	return n;
}

/* One advertisement every `step` ms for `ms` ms. */
static UNUSED void adv_run(struct mc_ctl *c, struct fake *f, uint32_t ms, uint32_t step, int8_t rssi)
{
	for (uint32_t t = 0; t <= ms; t += step) {
		mc_ctl_on_adv(c, rssi, false);
		f->now += step;
	}
}

/* Pull the token out of the last reply: "tok=abcdef" or "resend with abcdef". */
static UNUSED const char *tok_of(const char *r)
{
	static char t[8];
	const char *p = strstr(r, "tok=");
	if (p) p += 4;
	else if ((p = strstr(r, "resend with "))) p += 12;
	else return "";
	memcpy(t, p, 6); t[6] = '\0';
	return t;
}

/* START the operator's way: ask, get the node's token if it wants one, resend. */
static UNUSED void start_tok(struct mc_ctl *c)
{
	char cmd[32];
	H(c, true, "start");
	if (strstr(R, "needs token")) {
		snprintf(cmd, sizeof(cmd), "start %s", tok_of(R));
		H(c, true, cmd);
	}
}

#define ADDR "EA:7C:11:0B:55:90"
#define ARM_NOTOK "arm " ADDR " fw.zip"

/* Operator flow helper: get a token, arm, hold the gate, return with ARMED. */
static UNUSED void get_armed(struct mc_ctl *c, struct fake *f)
{
	char cmd[128];
	H(c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(c, true, cmd);
	adv_run(c, f, 6000, 500, -60);
}

#endif
