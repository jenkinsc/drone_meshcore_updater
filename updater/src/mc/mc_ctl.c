#include "mc_ctl.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const STATE_NAMES[] = {
	"IDLE", "WAITING", "TARGET_DETECTED", "RSSI_QUALIFIED", "ARMED",
	"START_AUTHORIZED", "DFU_ACTIVE",
};

const char *mc_state_str(enum mc_state s)
{
	return (unsigned)s < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) ? STATE_NAMES[s] : "?";
}

static const char *result_str(enum dfu_status_result r)
{
	switch (r) {
	case DFU_STATUS_RESULT_NONE:               return "none";
	case DFU_STATUS_RESULT_OK:                 return "ok";
	case DFU_STATUS_RESULT_NO_TARGET:          return "no target";
	case DFU_STATUS_RESULT_SCAN_ERROR:         return "scan error";
	case DFU_STATUS_RESULT_BAD_BUNDLE:         return "bad bundle";
	case DFU_STATUS_RESULT_CONNECT_FAILED:     return "connect failed";
	case DFU_STATUS_RESULT_SERVICE_MISSING:    return "dfu service missing";
	case DFU_STATUS_RESULT_CHAR_MISSING:       return "dfu char missing";
	case DFU_STATUS_RESULT_DISCONNECTED:       return "disconnected";
	case DFU_STATUS_RESULT_TIMEOUT:            return "timeout";
	case DFU_STATUS_RESULT_REMOTE_ERROR:       return "remote error";
	case DFU_STATUS_RESULT_FS_ERROR:           return "fs error";
	case DFU_STATUS_RESULT_RETRIES_EXHAUSTED:  return "retries exhausted";
	case DFU_STATUS_RESULT_TARGET_REJECTED:    return "target rejected";
	case DFU_STATUS_RESULT_AUTH_REQUIRED:      return "pin required";
	case DFU_STATUS_RESULT_AUTH_FAILED:        return "pin rejected";
	case DFU_STATUS_RESULT_UNREACHABLE_TARGET: return "unreachable target";
	case DFU_STATUS_RESULT_NO_APP_RULE:        return "no app rule";
	}
	return "error";
}

static size_t put(char *out, size_t cap, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static size_t put(char *out, size_t cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out, cap, fmt, ap);
	va_end(ap);
	if (n < 0) {
		out[0] = '\0';
		return 0;
	}
	return (size_t)n < cap ? (size_t)n : cap - 1;
}

static void note(struct mc_ctl *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void note(struct mc_ctl *c, const char *fmt, ...)
{
	char line[MC_REPLY_MAX + 1];
	va_list ap;

	if (!c->ops->note) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	c->ops->note(c->ctx, line);
}

static void set_state(struct mc_ctl *c, enum mc_state s)
{
	if (c->state != s) {
		note(c, "%s -> %s", mc_state_str(c->state), mc_state_str(s));
		c->state = s;
	}
}

/* ---- report queue ------------------------------------------------------- */

static void rq_flush(struct mc_ctl *c)
{
	uint32_t now = c->ops->now_ms(c->ctx);

	if (c->rq[0].kind == 0) {
		return;
	}
	if (c->any_tx && (uint32_t)(now - c->last_tx_ms) < c->cfg.report_gap_ms) {
		return;
	}
	note(c, "tx %s", c->rq[0].text);
	if (c->ops->report) {
		c->ops->report(c->ctx, c->rq[0].text);
	}
	c->last_tx_ms = now;
	c->any_tx = true;
	memmove(&c->rq[0], &c->rq[1], sizeof(c->rq[0]) * (MC_REPORT_Q - 1));
	c->rq[MC_REPORT_Q - 1].kind = 0;
}

static void rq_drop(struct mc_ctl *c, size_t i)
{
	memmove(&c->rq[i], &c->rq[i + 1], sizeof(c->rq[0]) * (MC_REPORT_Q - 1 - i));
	c->rq[MC_REPORT_Q - 1].kind = 0;
}

/* kind 1 = a milestone: a newer one replaces any still-queued older one. */
static void report(struct mc_ctl *c, int kind, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void report(struct mc_ctl *c, int kind, const char *fmt, ...)
{
	va_list ap;
	size_t n = 0;

	if (kind == 1) {
		for (size_t i = 0; i < MC_REPORT_Q; i++) {
			if (c->rq[i].kind == 1) {
				rq_drop(c, i);
				break;
			}
		}
	}
	while (n < MC_REPORT_Q && c->rq[n].kind != 0) {
		n++;
	}
	if (n == MC_REPORT_Q) {
		/* Full of non-replaceable messages (cannot happen with the current
		 * producers); drop the oldest rather than the newest. */
		rq_drop(c, 0);
		n = MC_REPORT_Q - 1;
	}
	c->rq[n].kind = (uint8_t)kind;
	va_start(ap, fmt);
	vsnprintf(c->rq[n].text, sizeof(c->rq[n].text), fmt, ap);
	va_end(ap);
	rq_flush(c);
}

/* ---- token -------------------------------------------------------------- */

static bool token_live(const struct mc_ctl *c)
{
	return c->token_valid &&
	       (uint32_t)(c->ops->now_ms(c->ctx) - c->token_at) < c->cfg.token_ttl_s * 1000u;
}

static uint32_t token_issue(struct mc_ctl *c)
{
	if (!token_live(c)) {
		c->token = c->ops->rand32(c->ctx) & 0xFFFFFFu;
		c->token_at = c->ops->now_ms(c->ctx);
		c->token_valid = true;
	}
	return c->token;
}

/* True iff `given` is the live token. A live token is spent by *any* attempt
 * that presents the right value, so it can never be tried twice. */
static bool token_take(struct mc_ctl *c, const char *given)
{
	char want[MC_TOKEN_LEN + 1];

	if (!token_live(c) || given[0] == '\0') {
		return false;
	}
	snprintf(want, sizeof(want), "%06x", (unsigned)c->token);
	for (size_t i = 0; i < MC_TOKEN_LEN; i++) {
		char g = given[i];

		if (g >= 'A' && g <= 'F') {
			g = (char)(g - 'A' + 'a');
		}
		if (g != want[i]) {
			return false;
		}
	}
	c->token_valid = false;
	return true;
}

/* ---- common ------------------------------------------------------------- */

static void to_idle(struct mc_ctl *c)
{
	if (c->ops->watch) {
		c->ops->watch(c->ctx, NULL, 0);
	}
	if (c->ops->bundle_lock) {
		c->ops->bundle_lock(c->ctx, NULL);
	}
	c->addr[0] = '\0';
	c->bundle[0] = '\0';
	c->fw[0] = '\0';
	memset(c->digest, 0, sizeof(c->digest));
	c->target_dfu_mode = false;
	mc_gate_reset(&c->gate);
	set_state(c, MC_IDLE);
}

static bool in_arm_phase(const struct mc_ctl *c)
{
	return c->state >= MC_WAITING && c->state <= MC_ARMED;
}

static void fw_id(struct mc_ctl *c)
{
	static const char d[] = "0123456789abcdef";

	for (size_t i = 0; i < MC_FW_ID_LEN / 2; i++) {
		c->fw[2 * i] = d[c->digest[i] >> 4];
		c->fw[2 * i + 1] = d[c->digest[i] & 15];
	}
	c->fw[MC_FW_ID_LEN] = '\0';
}

/* "TARGET -61 AVG -63 MIN -68 4.2/5s n12" (+ the reason when not ready). */
static size_t gate_line(struct mc_ctl *c, char *out, size_t cap)
{
	uint32_t now = c->ops->now_ms(c->ctx);
	enum mc_gate_verdict v = mc_gate_check(&c->gate, now);
	uint32_t held = mc_gate_held_ms(&c->gate, now);
	size_t n;

	if (!c->gate.seen_any) {
		return put(out, cap, "TARGET not seen (need avg>=%d floor>=%d %us n>=%u)",
			   c->cfg.gate.avg_min, c->cfg.gate.floor,
			   (unsigned)(c->cfg.gate.hold_ms / 1000), c->cfg.gate.min_samples);
	}
	n = put(out, cap, "TARGET %d AVG %d MIN %d %u.%u/%us n%u", c->gate.last_rssi,
		mc_gate_avg(&c->gate), mc_gate_min(&c->gate), (unsigned)(held / 1000),
		(unsigned)((held % 1000) / 100), (unsigned)(c->cfg.gate.hold_ms / 1000),
		(unsigned)c->gate.acq_samples);
	if (v != MC_GATE_READY) {
		n += put(out + n, cap - n, " %s", mc_gate_verdict_str(v));
	}
	return n;
}

static void with_token(struct mc_ctl *c, char *out, size_t cap, size_t n)
{
	if (n < cap) {
		put(out + n, cap - n, " tok=%06x", (unsigned)token_issue(c));
	}
}

/* ---- commands ----------------------------------------------------------- */

static size_t cmd_status(struct mc_ctl *c, char *out, size_t cap)
{
	char tmp[MC_REPLY_MAX + 1];
	size_t n;

	switch (c->state) {
	case MC_WAITING:
	case MC_TARGET_DETECTED:
	case MC_RSSI_QUALIFIED:
	case MC_ARMED: {
		uint32_t left = c->cfg.arm_timeout_s;
		uint32_t age = (c->ops->now_ms(c->ctx) - c->armed_at) / 1000u;

		gate_line(c, tmp, sizeof(tmp));
		n = put(out, cap, "%s FW %s %s %s exp%us", mc_state_str(c->state), c->fw,
			c->addr, tmp, (unsigned)(left > age ? left - age : 0));
		if (c->state == MC_ARMED) {
			with_token(c, out, cap, n);
		}
		return strlen(out);
	}
	case MC_START_AUTHORIZED:
	case MC_DFU_ACTIVE:
		return put(out, cap, "%s FW %s %s %u%% try %u/%u st%u", mc_state_str(c->state),
			   c->fw, c->addr, c->ev.percent, c->ev.attempt, c->ev.retries,
			   c->ev.state);
	case MC_IDLE:
	default: {
		static const char *const LAST[] = { "", " last=ok", " last=failed",
			" last=aborted", " last=expired", " last=bundle changed" };

		n = put(out, cap, "IDLE%s", LAST[c->last]);
		if (c->last == MC_LAST_FAILED) {
			n += put(out + n, cap - n, " (%s)", result_str(c->last_result));
		}
		with_token(c, out, cap, n);
		return strlen(out);
	}
	}
}

static size_t cmd_scan(struct mc_ctl *c, unsigned page, char *out, size_t cap)
{
	enum { PER_PAGE = 3 };
	struct mc_scan_row rows[PER_PAGE];
	size_t total = 0;
	size_t off = (size_t)(page - 1) * PER_PAGE;
	size_t n = c->ops->scan_rows(c->ctx, rows, PER_PAGE, off, &total);
	size_t pages = (total + PER_PAGE - 1) / PER_PAGE;
	size_t len;

	if (total == 0) {
		return put(out, cap, "scan: nothing heard");
	}
	if (n == 0) {
		return put(out, cap, "scan: no page %u (of %u)", page, (unsigned)pages);
	}
	len = put(out, cap, "scan %u/%u:", page, (unsigned)pages);
	for (size_t i = 0; i < n && len + 1 < cap; i++) {
		len += put(out + len, cap - len, "%s %s %c %d %s",
			   i == 0 ? "" : " |", rows[i].addr, rows[i].type,
			   rows[i].rssi, rows[i].name[0] ? rows[i].name : "-");
	}
	return len;
}

static size_t cmd_rssi(struct mc_ctl *c, char *out, size_t cap)
{
	if (!in_arm_phase(c)) {
		return put(out, cap, "rssi: not armed (arm <addr> <bundle> first)");
	}
	return gate_line(c, out, cap);
}

/* A privileged token-gated command arrived without a live token. */
static size_t need_token(struct mc_ctl *c, const char *what, char *out, size_t cap)
{
	return put(out, cap, "%s needs token: resend with %06x", what,
		   (unsigned)token_issue(c));
}

static size_t cmd_arm(struct mc_ctl *c, const struct mc_cmd *cmd, char *out, size_t cap)
{
	uint8_t d[MC_DIGEST_LEN];

	if (c->state == MC_START_AUTHORIZED || c->state == MC_DFU_ACTIVE) {
		return put(out, cap, "ARM refused: DFU running (abort first)");
	}
	if (!token_take(c, cmd->token)) {
		return need_token(c, "ARM", out, cap);
	}
	int rc = c->ops->bundle_digest(c->ctx, cmd->bundle, d);

	if (rc != 0) {
		size_t n = put(out, cap, "ARM refused: bundle %s unreadable (%d)", cmd->bundle, rc);

		with_token(c, out, cap, n);
		return strlen(out);
	}

	if (in_arm_phase(c)) {
		to_idle(c);          /* re-arming replaces the previous selection */
	}
	strcpy(c->addr, cmd->addr);
	c->addr_type = cmd->addr_type;
	strcpy(c->bundle, cmd->bundle);
	memcpy(c->digest, d, sizeof(d));
	fw_id(c);
	c->target_dfu_mode = false;
	c->armed_at = c->ops->now_ms(c->ctx);
	c->last = MC_LAST_NONE;
	c->last_result = DFU_STATUS_RESULT_NONE;
	mc_gate_reset(&c->gate);
	if (c->ops->bundle_lock) {
		c->ops->bundle_lock(c->ctx, c->bundle);
	}
	if (c->ops->watch) {
		c->ops->watch(c->ctx, c->addr, c->addr_type);
	}
	set_state(c, MC_WAITING);

	size_t n = put(out, cap, "ARM ok FW %s %s %c b=%s WAITING", c->fw, c->addr,
		       c->addr_type, c->bundle);
	with_token(c, out, cap, n);
	return strlen(out);
}

static size_t cmd_disarm(struct mc_ctl *c, char *out, size_t cap)
{
	if (c->state == MC_START_AUTHORIZED || c->state == MC_DFU_ACTIVE) {
		return put(out, cap, "DISARM refused: DFU running (use abort)");
	}
	if (c->state == MC_IDLE) {
		return put(out, cap, "already IDLE");
	}
	to_idle(c);
	return put(out, cap, "IDLE");
}

/* Re-hash the bundle against the digest taken at ARM. */
static bool digest_matches(struct mc_ctl *c)
{
	uint8_t d[MC_DIGEST_LEN];

	return c->ops->bundle_digest(c->ctx, c->bundle, d) == 0 &&
	       memcmp(d, c->digest, sizeof(d)) == 0;
}

static size_t cmd_start(struct mc_ctl *c, const struct mc_cmd *cmd, char *out, size_t cap)
{
	char g[MC_REPLY_MAX + 1];
	size_t n;

	if (c->state == MC_START_AUTHORIZED || c->state == MC_DFU_ACTIVE) {
		return put(out, cap, "START refused: already running");
	}
	if (c->state == MC_IDLE) {
		return put(out, cap, "START refused: not armed");
	}
	if (!token_take(c, cmd->token)) {
		return need_token(c, "START", out, cap);
	}
	if (c->state != MC_ARMED) {
		gate_line(c, g, sizeof(g));
		n = put(out, cap, "START refused: %s %s", mc_state_str(c->state), g);
		with_token(c, out, cap, n);
		return strlen(out);
	}

	/* Re-evaluate the gate at this instant; a stored verdict is not enough. */
	uint32_t now = c->ops->now_ms(c->ctx);
	enum mc_gate_verdict v = mc_gate_check(&c->gate, now);

	if (v != MC_GATE_READY) {
		gate_line(c, g, sizeof(g));
		set_state(c, MC_TARGET_DETECTED);
		n = put(out, cap, "START refused: %s", g);
		with_token(c, out, cap, n);
		return strlen(out);
	}
	if (!digest_matches(c)) {
		note(c, "bundle %s changed since ARM (FW %s)", c->bundle, c->fw);
		c->last = MC_LAST_BUNDLE_CHANGED;
		to_idle(c);
		n = put(out, cap, "START refused: bundle changed since ARM; IDLE");
		with_token(c, out, cap, n);
		return strlen(out);
	}
	if (c->ops->dfu_busy(c->ctx)) {
		n = put(out, cap, "START refused: DFU runner busy");
		with_token(c, out, cap, n);
		return strlen(out);
	}

	/* Everything the reply needs is read first. */
	char reply[MC_REPLY_MAX + 1];

	put(reply, sizeof(reply), "STARTED FW %s %s b=%s avg %d", c->fw, c->addr, c->bundle,
	    mc_gate_avg(&c->gate));
	set_state(c, MC_START_AUTHORIZED);
	int rc = c->ops->dfu_start(c->ctx, c->bundle, c->addr, c->addr_type,
				   !c->target_dfu_mode);

	if (rc != 0) {
		c->ops->dfu_end(c->ctx);
		set_state(c, MC_ARMED);
		n = put(out, cap, "START failed (rc %d); still ARMED", rc);
		with_token(c, out, cap, n);
		return strlen(out);
	}

	mc_progress_begin(&c->prog);
	memset(&c->ev, 0, sizeof(c->ev));
	set_state(c, MC_DFU_ACTIVE);
	c->last = MC_LAST_NONE;
	c->last_result = DFU_STATUS_RESULT_NONE;
	if (c->ops->watch) {
		c->ops->watch(c->ctx, NULL, 0);
	}
	return put(out, cap, "%s", reply);
}

/* States in which the target's application has not yet been erased, so
 * cancelling leaves it exactly as it was. From the DFU START packet onward the
 * single-bank bootloader has erased it; a cancel then buys nothing. */
static bool dfu_abortable(uint8_t s)
{
	switch (s) {
	case DFU_STATUS_IDLE:
	case DFU_STATUS_SCANNING:
	case DFU_STATUS_CONNECTING:
	case DFU_STATUS_ENABLING:
	case DFU_STATUS_COOLDOWN:
	case DFU_STATUS_AWAITING_PIN:
		return true;
	default:
		return false;
	}
}

static size_t cmd_abort(struct mc_ctl *c, char *out, size_t cap)
{
	if (c->state == MC_START_AUTHORIZED) {
		return put(out, cap, "ABORT refused: START in progress, retry");
	}
	if (c->state == MC_DFU_ACTIVE) {
		if (!dfu_abortable(c->ev.state)) {
			return put(out, cap, "ABORT refused: upload under way, target app already "
				   "erased; it will finish or fail");
		}
		c->ops->dfu_abort(c->ctx);
		c->ops->dfu_end(c->ctx);
		mc_progress_end(&c->prog);
		c->last = MC_LAST_ABORTED;
		to_idle(c);
		return put(out, cap, "ABORTED; IDLE");
	}
	if (in_arm_phase(c)) {
		to_idle(c);
		return put(out, cap, "ABORT: was only armed; IDLE");
	}
	return put(out, cap, "ABORT: nothing running");
}

static size_t cmd_battery(struct mc_ctl *c, char *out, size_t cap)
{
	uint16_t mv = 0;
	uint8_t pct = 0;
	int charging = -1;   /* -1 unknown, 0 no, 1 yes */

	if (c->ops->battery == NULL || c->ops->battery(c->ctx, &mv, &pct, &charging) != 0) {
		return put(out, cap, "battery: not available");
	}
	return put(out, cap, "battery %u.%02uV %u%%%s", (unsigned)(mv / 1000),
		   (unsigned)((mv % 1000) / 10), pct,
		   charging > 0 ? " charging" : "");
}

size_t mc_ctl_handle(struct mc_ctl *c, bool authed, const char *text,
		     char *reply, size_t cap)
{
	struct mc_cmd cmd;
	enum mc_parse_err pe = mc_cmd_parse(text, &cmd);
	size_t n;

	if (pe != MC_PARSE_OK) {
		switch (pe) {
		case MC_PARSE_UNKNOWN:
			n = put(reply, cap, "? cmds: status scan rssi arm disarm start abort version battery log");
			break;
		case MC_PARSE_BAD_ADDR:
			n = put(reply, cap, "bad address: need AA:BB:CC:DD:EE:FF");
			break;
		case MC_PARSE_BAD_BUNDLE:
			n = put(reply, cap, "bad bundle: file name only, [A-Za-z0-9._-]");
			break;
		case MC_PARSE_EMPTY:
			n = put(reply, cap, "empty command");
			break;
		default:
			n = put(reply, cap, "usage: arm <addr> <file> [random|public] [tok] | start [tok] | scan|log [n]");
			break;
		}
		note(c, "-> %s", reply);
		return n ? n : 1;
	}

	if (mc_cmd_privileged(cmd.id) && !authed) {
		n = put(reply, cap, "DENIED: %s needs the configured operator key",
			mc_cmd_name(cmd.id));
		note(c, "-> %s", reply);
		return n;
	}

	note(c, "rx %s", mc_cmd_name(cmd.id));

	switch (cmd.id) {
	case MC_CMD_STATUS:  n = cmd_status(c, reply, cap); break;
	case MC_CMD_SCAN:    n = cmd_scan(c, cmd.page, reply, cap); break;
	case MC_CMD_RSSI:    n = cmd_rssi(c, reply, cap); break;
	case MC_CMD_ARM:     n = cmd_arm(c, &cmd, reply, cap); break;
	case MC_CMD_DISARM:  n = cmd_disarm(c, reply, cap); break;
	case MC_CMD_START:   n = cmd_start(c, &cmd, reply, cap); break;
	case MC_CMD_ABORT:   n = cmd_abort(c, reply, cap); break;
	case MC_CMD_VERSION:
		n = put(reply, cap, "%s", c->ops->version ? c->ops->version(c->ctx) : "?");
		break;
	case MC_CMD_BATTERY: n = cmd_battery(c, reply, cap); break;
	case MC_CMD_LOG:
		n = c->ops->log_page ? c->ops->log_page(c->ctx, cmd.page, reply, cap)
				     : put(reply, cap, "log: not available");
		break;
	default:
		n = put(reply, cap, "?");
		break;
	}
	/* One line for what a state-changing command did or why it was refused.
	 * Read-only commands are not logged: `log` would otherwise record itself. */
	if (mc_cmd_privileged(cmd.id)) {
		note(c, "-> %s", reply);
	}
	return n ? n : 1;
}

/* ---- events ------------------------------------------------------------- */

/* Move along the arm phase according to the gate. Cheap except for the one
 * re-hash on the way to ARMED. */
static void evaluate(struct mc_ctl *c)
{
	uint32_t now = c->ops->now_ms(c->ctx);
	enum mc_gate_verdict v;

	if (c->state < MC_TARGET_DETECTED || c->state > MC_ARMED) {
		return;
	}
	v = mc_gate_check(&c->gate, now);
	if (v == MC_GATE_READY) {
		if (c->state == MC_TARGET_DETECTED) {
			set_state(c, MC_RSSI_QUALIFIED);
			if (!digest_matches(c)) {
				report(c, 2, "ARM cancelled: bundle %s changed since ARM (FW %s)",
				       c->bundle, c->fw);
				c->last = MC_LAST_BUNDLE_CHANGED;
				to_idle(c);
				return;
			}
			set_state(c, MC_ARMED);
		}
	} else if (c->state == MC_RSSI_QUALIFIED || c->state == MC_ARMED) {
		set_state(c, MC_TARGET_DETECTED);
	}
}

void mc_ctl_on_adv(struct mc_ctl *c, int8_t rssi, bool dfu_uuid)
{
	if (!in_arm_phase(c)) {
		return;
	}
	if (dfu_uuid) {
		c->target_dfu_mode = true;
	}
	mc_gate_sample(&c->gate, c->ops->now_ms(c->ctx), rssi);
	if (c->state == MC_WAITING) {
		set_state(c, MC_TARGET_DETECTED);
	}
	evaluate(c);
}

void mc_ctl_samples_lost(struct mc_ctl *c)
{
	if (in_arm_phase(c)) {
		mc_gate_reset(&c->gate);
		note(c, "gate reset: samples lost");
		if (c->state != MC_WAITING) {
			set_state(c, MC_TARGET_DETECTED);
		}
	}
}

void mc_ctl_on_dfu_event(struct mc_ctl *c, const struct dfu_status_event *ev)
{
	struct mc_progress_msg msgs[MC_PROGRESS_MAX_MSGS];

	if (c->state != MC_DFU_ACTIVE) {
		return;
	}
	c->ev = *ev;

	/* The runner was reset from somewhere other than this layer (an SMP
	 * stop, say): the run is over and nobody said so. Report it and stand
	 * down rather than sit in DFU_ACTIVE forever. Our own abort never gets
	 * here — it leaves DFU_ACTIVE first. */
	if (ev->state == DFU_STATUS_IDLE) {
		report(c, 2, "DFU stopped %s (runner reset)", c->addr);
		mc_progress_end(&c->prog);
		c->ops->dfu_end(c->ctx);
		c->last = MC_LAST_ABORTED;
		to_idle(c);
		return;
	}

	size_t n = mc_progress_update(&c->prog, ev, msgs);

	for (size_t i = 0; i < n; i++) {
		const struct mc_progress_msg *m = &msgs[i];

		switch (m->kind) {
		case MC_MSG_MILESTONE:
			report(c, 1, "DFU %u%%", m->milestone);
			break;
		case MC_MSG_VERIFYING:
			report(c, 2, "DFU verifying");
			break;
		case MC_MSG_DONE:
			report(c, 2, "DFU SUCCESS %s FW %s", c->addr, c->fw);
			c->last = MC_LAST_OK;
			c->last_result = DFU_STATUS_RESULT_OK;
			c->ops->dfu_end(c->ctx);
			to_idle(c);
			break;
		case MC_MSG_FAILED:
			report(c, 2, "DFU FAILED %s: %s", c->addr, result_str(m->result));
			c->last = MC_LAST_FAILED;
			c->last_result = m->result;
			c->ops->dfu_end(c->ctx);
			to_idle(c);
			break;
		}
	}
}

void mc_ctl_tick(struct mc_ctl *c)
{
	if (in_arm_phase(c)) {
		evaluate(c);
		if (c->cfg.arm_timeout_s != 0 && in_arm_phase(c) &&
		    (uint32_t)(c->ops->now_ms(c->ctx) - c->armed_at) >=
			    c->cfg.arm_timeout_s * 1000u) {
			report(c, 2, "ARM expired %s (no START in %us)", c->addr,
			       (unsigned)c->cfg.arm_timeout_s);
			c->last = MC_LAST_EXPIRED;
			to_idle(c);
		}
	}
	rq_flush(c);
}

void mc_ctl_init(struct mc_ctl *c, const struct mc_ops *ops, void *ctx,
		 const struct mc_cfg *cfg)
{
	memset(c, 0, sizeof(*c));
	c->ops = ops;
	c->ctx = ctx;
	c->cfg = *cfg;
	c->state = MC_IDLE;
	mc_gate_init(&c->gate, &cfg->gate);
}
