#include "mc_ctl.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *mc_state_str(enum mc_state s)
{
	switch (s) {
	case MC_DISARMED: return "DISARMED";
	case MC_ARMED:    return "ARMED";
	case MC_RUNNING:  return "RUNNING";
	}
	return "?";
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

static void report(struct mc_ctl *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void report(struct mc_ctl *c, const char *fmt, ...)
{
	char msg[MC_REPLY_MAX + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	note(c, "tx %s", msg);
	if (c->ops->report) {
		c->ops->report(c->ctx, msg);
	}
}

void mc_ctl_init(struct mc_ctl *c, const struct mc_ops *ops, void *ctx,
		 const struct mc_cfg *cfg)
{
	memset(c, 0, sizeof(*c));
	c->ops = ops;
	c->ctx = ctx;
	c->cfg = *cfg;
	c->state = MC_DISARMED;
	mc_gate_init(&c->gate, cfg->rssi_min, cfg->hold_ms, cfg->max_gap_ms,
		     cfg->min_samples);
}

static void to_disarmed(struct mc_ctl *c)
{
	c->state = MC_DISARMED;
	c->addr[0] = '\0';
	c->bundle[0] = '\0';
	mc_gate_reset(&c->gate);
	if (c->ops->watch) {
		c->ops->watch(c->ctx, NULL, 0);
	}
}

/* ---- commands ----------------------------------------------------------- */

static size_t gate_line(struct mc_ctl *c, char *out, size_t cap)
{
	uint32_t now = c->ops->now_ms(c->ctx);
	enum mc_gate_verdict v = mc_gate_check(&c->gate, now);
	uint32_t held = mc_gate_held_ms(&c->gate, now);

	if (!c->gate.seen_any) {
		return put(out, cap, "%s b=%s: target not seen yet (need >= %d dBm for %u.%us)",
			   c->addr, c->bundle, c->cfg.rssi_min,
			   (unsigned)(c->cfg.hold_ms / 1000),
			   (unsigned)((c->cfg.hold_ms % 1000) / 100));
	}
	return put(out, cap, "%s b=%s rssi %d (min %d) held %u.%u/%u.%us: %s",
		   c->addr, c->bundle, c->gate.last_rssi, c->cfg.rssi_min,
		   (unsigned)(held / 1000), (unsigned)((held % 1000) / 100),
		   (unsigned)(c->cfg.hold_ms / 1000),
		   (unsigned)((c->cfg.hold_ms % 1000) / 100),
		   mc_gate_verdict_str(v));
}

static size_t cmd_status(struct mc_ctl *c, char *out, size_t cap)
{
	char tmp[MC_REPLY_MAX + 1];

	switch (c->state) {
	case MC_ARMED:
		gate_line(c, tmp, sizeof(tmp));
		return put(out, cap, "ARMED %s", tmp);
	case MC_RUNNING:
		return put(out, cap, "RUNNING %s %u%% attempt %u/%u state %u",
			   c->addr, c->ev.percent, c->ev.attempt, c->ev.retries,
			   c->ev.state);
	case MC_DISARMED:
	default:
		if (c->last == MC_LAST_OK) {
			return put(out, cap, "DISARMED last=ok");
		}
		if (c->last == MC_LAST_FAILED) {
			return put(out, cap, "DISARMED last=failed (%s)",
				   result_str(c->last_result));
		}
		if (c->last == MC_LAST_ABORTED) {
			return put(out, cap, "DISARMED last=aborted");
		}
		return put(out, cap, "DISARMED");
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
	if (c->state != MC_ARMED) {
		return put(out, cap, "rssi: not armed (arm <addr> <bundle> first)");
	}
	return gate_line(c, out, cap);
}

static size_t cmd_arm(struct mc_ctl *c, const struct mc_cmd *cmd, char *out,
		      size_t cap)
{
	if (c->state == MC_RUNNING) {
		return put(out, cap, "ARM refused: DFU running (abort first)");
	}
	if (c->ops->bundle_check(c->ctx, cmd->bundle) != 0) {
		return put(out, cap, "ARM refused: bundle %s not found", cmd->bundle);
	}

	strcpy(c->addr, cmd->addr);
	c->addr_type = cmd->addr_type;
	strcpy(c->bundle, cmd->bundle);
	c->armed_at = c->ops->now_ms(c->ctx);
	c->state = MC_ARMED;
	c->last = MC_LAST_NONE;
	c->last_result = DFU_STATUS_RESULT_NONE;
	mc_gate_reset(&c->gate);
	if (c->ops->watch) {
		c->ops->watch(c->ctx, c->addr, c->addr_type);
	}

	return put(out, cap, "ARMED %s (%s) b=%s: need >= %d dBm for %u.%us, then START",
		   c->addr, c->addr_type == 'p' ? "public" : "random", c->bundle,
		   c->cfg.rssi_min, (unsigned)(c->cfg.hold_ms / 1000),
		   (unsigned)((c->cfg.hold_ms % 1000) / 100));
}

static size_t cmd_disarm(struct mc_ctl *c, char *out, size_t cap)
{
	if (c->state == MC_RUNNING) {
		return put(out, cap, "DISARM refused: DFU running (use abort)");
	}
	if (c->state == MC_DISARMED) {
		return put(out, cap, "already DISARMED");
	}
	to_disarmed(c);
	return put(out, cap, "DISARMED");
}

static size_t cmd_start(struct mc_ctl *c, char *out, size_t cap)
{
	if (c->state == MC_RUNNING) {
		return put(out, cap, "START refused: already running");
	}
	if (c->state != MC_ARMED) {
		return put(out, cap, "START refused: not armed");
	}

	uint32_t now = c->ops->now_ms(c->ctx);
	enum mc_gate_verdict v = mc_gate_check(&c->gate, now);
	if (v != MC_GATE_READY) {
		char g[MC_REPLY_MAX + 1];
		gate_line(c, g, sizeof(g));
		return put(out, cap, "START refused: %s", g);
	}
	if (c->ops->dfu_busy(c->ctx)) {
		return put(out, cap, "START refused: DFU runner busy");
	}

	int rc = c->ops->dfu_start(c->ctx, c->bundle, c->addr, c->addr_type);
	if (rc != 0) {
		return put(out, cap, "START failed (rc %d); still ARMED", rc);
	}

	/* Arming is consumed. Everything the reply needs is read first. */
	size_t n = put(out, cap, "STARTED %s b=%s rssi %d", c->addr, c->bundle,
		       c->gate.last_rssi);
	mc_progress_begin(&c->prog);
	memset(&c->ev, 0, sizeof(c->ev));
	c->state = MC_RUNNING;
	c->last = MC_LAST_NONE;
	c->last_result = DFU_STATUS_RESULT_NONE;
	if (c->ops->watch) {
		c->ops->watch(c->ctx, NULL, 0);
	}
	return n;
}

static size_t cmd_abort(struct mc_ctl *c, char *out, size_t cap)
{
	if (c->state == MC_RUNNING) {
		c->ops->dfu_abort(c->ctx);
		mc_progress_end(&c->prog);
		c->last = MC_LAST_ABORTED;
		to_disarmed(c);
		return put(out, cap, "ABORT requested; DISARMED");
	}
	if (c->state == MC_ARMED) {
		to_disarmed(c);
		return put(out, cap, "ABORT: was only ARMED; now DISARMED");
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
			n = put(reply, cap, "usage: arm <addr> <bundle> [random|public] | scan [n] | log [n] | <cmd>");
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
	case MC_CMD_START:   n = cmd_start(c, reply, cap); break;
	case MC_CMD_ABORT:   n = cmd_abort(c, reply, cap); break;
	case MC_CMD_VERSION:
		n = put(reply, cap, "drone_meshcore_updater %s",
			c->ops->version ? c->ops->version(c->ctx) : "?");
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

void mc_ctl_on_adv(struct mc_ctl *c, int8_t rssi)
{
	if (c->state != MC_ARMED) {
		return;
	}
	mc_gate_sample(&c->gate, c->ops->now_ms(c->ctx), rssi);
}

void mc_ctl_samples_lost(struct mc_ctl *c)
{
	if (c->state == MC_ARMED) {
		mc_gate_reset(&c->gate);
		note(c, "gate reset: samples lost");
	}
}

void mc_ctl_on_dfu_event(struct mc_ctl *c, const struct dfu_status_event *ev)
{
	struct mc_progress_msg msgs[MC_PROGRESS_MAX_MSGS];

	if (c->state != MC_RUNNING) {
		return;
	}
	c->ev = *ev;

	/* The runner was reset from somewhere other than this layer (an SMP
	 * stop, say): the run is over and nobody said so. Report it and stand
	 * down rather than sit in RUNNING forever. Our own abort never gets
	 * here — it leaves RUNNING first. */
	if (ev->state == DFU_STATUS_IDLE) {
		report(c, "DFU stopped %s (runner reset)", c->addr);
		mc_progress_end(&c->prog);
		c->last = MC_LAST_ABORTED;
		to_disarmed(c);
		return;
	}

	size_t n = mc_progress_update(&c->prog, ev, msgs);
	for (size_t i = 0; i < n; i++) {
		const struct mc_progress_msg *m = &msgs[i];

		switch (m->kind) {
		case MC_MSG_MILESTONE:
			report(c, "DFU %u%%", m->milestone);
			break;
		case MC_MSG_RETRY:
			report(c, "DFU retry %u/%u", m->attempt, m->retries);
			break;
		case MC_MSG_DONE:
			report(c, "DFU SUCCESS %s", c->addr);
			c->last = MC_LAST_OK;
			c->last_result = DFU_STATUS_RESULT_OK;
			to_disarmed(c);
			break;
		case MC_MSG_FAILED:
			report(c, "DFU FAILED %s: %s", c->addr, result_str(m->result));
			c->last = MC_LAST_FAILED;
			c->last_result = m->result;
			to_disarmed(c);
			break;
		}
	}
}

void mc_ctl_tick(struct mc_ctl *c)
{
	if (c->state != MC_ARMED || c->cfg.arm_timeout_s == 0) {
		return;
	}
	uint32_t age = c->ops->now_ms(c->ctx) - c->armed_at;
	if (age >= c->cfg.arm_timeout_s * 1000u) {
		report(c, "ARM expired for %s (no START in %us)", c->addr,
		       (unsigned)c->cfg.arm_timeout_s);
		to_disarmed(c);
		note(c, "arm expired");
	}
}
