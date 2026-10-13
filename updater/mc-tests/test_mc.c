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
#include "test_fake.h"

extern const struct mc_crypto TEST_CRYPTO;
static const struct mc_crypto *CRY;

int g_checks, g_fail;
void run_ctl_tests(void);   /* test_ctl.c */

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

	/* single-use tokens: six hex digits, last word of arm / start */
	CHECK(mc_cmd_parse("start abc123", &c) == MC_PARSE_OK && !strcmp(c.token, "abc123"));
	CHECK(mc_cmd_parse("start ABC123", &c) == MC_PARSE_OK);
	CHECK(mc_cmd_parse("start", &c) == MC_PARSE_OK && c.token[0] == '\0');
	CHECK(mc_cmd_parse("start abc12", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("start abc12g", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("start abc1234", &c) == MC_PARSE_USAGE);
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 fw.zip abc123", &c) == MC_PARSE_OK &&
	      c.addr_type == 'r' && !strcmp(c.token, "abc123"));
	CHECK(mc_cmd_parse("arm EA:7C:11:0B:55:90 fw.zip public abc123", &c) == MC_PARSE_OK &&
	      c.addr_type == 'p' && !strcmp(c.token, "abc123"));
	CHECK(mc_cmd_needs_token(MC_CMD_ARM) && mc_cmd_needs_token(MC_CMD_START));
	CHECK(!mc_cmd_needs_token(MC_CMD_DISARM) && !mc_cmd_needs_token(MC_CMD_ABORT));
	CHECK(!mc_cmd_needs_token(MC_CMD_STATUS));

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

/* ------------------------------------------------------------------ evlog -- */

static void test_evlog(void)
{
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

static void rig_setup(struct rig *r, uint32_t persisted_adv_ts)
{
	uint8_t seed[32];
	memset(r, 0, sizeof(*r));
	seed_for(0x10, seed); mc_id_from_seed(&r->node, seed);
	seed_for(0x55, seed); mc_id_from_seed(&r->op, seed);
	mc_id_set_operator(&r->node, r->op.pub);
	ctl_setup(&r->ctl, &r->f);
	struct mc_persist p = { 0, persisted_adv_ts };
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

/* Send a command as the operator and return the node's reply text. */
static bool op_say(struct rig *r, uint32_t ts, uint8_t type, const char *text, char *reply)
{
	uint8_t fr[MC_MAX_RAW];
	struct mc_txt t;
	int before = r->s.n;
	size_t n = op_frame(r, ts, type, text, fr);

	reply[0] = '\0';
	mc_node_rx(&r->nd, fr, n);
	for (int i = r->s.n - 1; i >= before; i--) {
		if (decode_reply(r, r->s.frames[i], r->s.lens[i], &t)) {
			snprintf(reply, 160, "%s", t.text);
			return true;
		}
	}
	return false;
}

static void test_node(void)
{
	struct rig r;
	uint8_t fr[MC_MAX_RAW];
	struct mc_txt t;
	char rep[160], cmd[160];
	size_t n;
	SECTION("node (packet -> decision)");

	/* status gives a token; arm with it: ACK + reply, nothing written to flash */
	rig_setup(&r, 1000);
	CHECK(op_say(&r, 2000, MC_TXT_PLAIN, "status", rep) && strstr(rep, "IDLE") && strstr(rep, "tok="));
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(rep));
	r.s.n = 0;
	n = op_frame(&r, 2001, MC_TXT_PLAIN, cmd, fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.ctl.state == MC_WAITING);
	CHECK(r.s.n == 2);                                   /* ACK, then reply */
	struct mc_pkt p0;
	CHECK(mc_wire_parse(r.s.frames[0], r.s.lens[0], &p0) == 0 && p0.ptype == MC_PT_PATH);
	CHECK(decode_reply(&r, r.s.frames[1], r.s.lens[1], &t) && strstr(t.text, "ARM ok FW "));
	CHECK(r.s.persists == 0);                            /* commands never touch flash */

	/* the same packet again (a repeater's copy, or a retry): acked, not re-run */
	r.s.n = 0;
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 1);                                   /* ACK only, no reply */
	CHECK(r.f.watch_calls == 1);

	/* a wrong operator clock does not lock anyone out: ancient and far-future
	 * timestamps are both processed */
	CHECK(op_say(&r, 5, MC_TXT_PLAIN, "disarm", rep) && r.ctl.state == MC_IDLE);
	CHECK(op_say(&r, 0xFFFFFFF0u, MC_TXT_PLAIN, "status", rep) && strstr(rep, "IDLE"));
	CHECK(op_say(&r, 10, MC_TXT_PLAIN, "status", rep) && strstr(rep, "IDLE"));

	/* a recorded ARM replayed later (after the duplicate ring has moved on) is
	 * inert: its token was spent */
	for (uint32_t i = 0; i < 12; i++) {
		char v[16];
		snprintf(v, sizeof(v), "version");
		op_say(&r, 100 + i, MC_TXT_PLAIN, v, rep);
	}
	r.f.watch_calls = 0;
	r.s.n = 0;
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.ctl.state == MC_IDLE && r.f.watch_calls == 0);
	CHECK(r.s.n >= 1);   /* it is processed (no timestamp rule) and answered with a fresh demand */

	/* re-arm properly for the key / sender checks below */
	CHECK(op_say(&r, 300, MC_TXT_PLAIN, "status", rep));
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(rep));
	CHECK(op_say(&r, 301, MC_TXT_PLAIN, cmd, rep) && strstr(rep, "ARM ok"));
	CHECK(r.ctl.state == MC_WAITING);

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
	CHECK(r.s.n == 0 && r.ctl.state == MC_WAITING && r.nd.rx_bad >= 1);

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
	CHECK(r.s.n == 0 && r.ctl.state == MC_WAITING);

	/* garbage and truncations never crash */
	for (size_t l = 0; l < 40; l++) {
		uint8_t junk[40];
		for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 31 + l);
		mc_node_rx(&r.nd, junk, l);
	}
	CHECK(r.ctl.state == MC_WAITING);

	/* a CLI-type command is answered in kind, and (per MeshCore) not ACKed */
	rig_setup(&r, 0);
	r.s.n = 0;
	n = op_frame(&r, 10, MC_TXT_CLI, "version", fr);
	mc_node_rx(&r.nd, fr, n);
	CHECK(r.s.n == 1);
	CHECK(decode_reply(&r, r.s.frames[0], r.s.lens[0], &t) && t.txt_type == MC_TXT_CLI &&
	      strstr(t.text, "0.1.0+test"));

	/* end to end over the air: status, arm, hold the gate, START needs a token */
	rig_setup(&r, 0);
	op_say(&r, 100, MC_TXT_PLAIN, "status", rep);
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(rep));
	op_say(&r, 101, MC_TXT_PLAIN, cmd, rep);
	r.f.now = 1000;
	adv_run(&r.ctl, &r.f, 3000, 500, -60);
	op_say(&r, 102, MC_TXT_PLAIN, "start", rep);
	snprintf(cmd, sizeof(cmd), "start %s", tok_of(rep));
	CHECK(op_say(&r, 103, MC_TXT_PLAIN, cmd, rep) && strstr(rep, "refused") && r.f.start_calls == 0);
	adv_run(&r.ctl, &r.f, 3000, 500, -60);
	CHECK(op_say(&r, 104, MC_TXT_PLAIN, "start", rep) && strstr(rep, "needs token"));
	CHECK(r.f.start_calls == 0);
	snprintf(cmd, sizeof(cmd), "start %s", tok_of(rep));
	CHECK(op_say(&r, 105, MC_TXT_PLAIN, cmd, rep) && strstr(rep, "STARTED FW "));
	CHECK(r.f.start_calls == 1 && r.ctl.state == MC_DFU_ACTIVE);

	/* adverts: signed, newer than any before; flash written at most daily */
	rig_setup(&r, 0);
	r.s.n = 0;
	mc_node_advertise(&r.nd, "Drone Updater");
	CHECK(r.s.n == 1 && r.s.frames[0][0] == 0x11);
	CHECK(r.s.persists == 1);                       /* first ever: stored */
	uint32_t first = r.nd.last_adv_ts;
	mc_node_advertise(&r.nd, "Drone Updater");
	CHECK(r.nd.last_adv_ts > first);
	CHECK(r.s.persists == 1);                       /* not again within a day */
	r.s.now += 86400u * 1000u + 5000u;
	mc_node_advertise(&r.nd, "Drone Updater");
	CHECK(r.s.persists == 2 && r.s.last.adv_ts >= first + 86400u);
	/* after a reboot the persisted floor keeps it increasing */
	struct mc_persist keep = r.s.last;
	struct rig r2;
	rig_setup(&r2, 0);
	mc_node_init(&r2.nd, CRY, &r2.node, &r2.ctl, &NOPS, &r2.s, &keep);
	mc_node_advertise(&r2.nd, "Drone Updater");
	CHECK(r2.nd.last_adv_ts > keep.adv_ts);
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
	CHECK(!memcmp(st.prv, prv, 64) && !memcmp(st.pub, pub, 32) && p.reserved == 0 && p.adv_ts == 0);

	struct mc_persist w = { 100, 50 };
	CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	w.reserved = 200; w.adv_ts = 60;
	CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK && p.reserved == 200 && p.adv_ts == 60);

	/* A corrupted header is "empty", never a half-trusted key. */
	f.page[8] ^= 1;
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_EMPTY);
	f.page[8] ^= 1;
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK && p.reserved == 200);

	/* Fill past the page: identity survives, newest record wins, no 0->1 writes. */
	for (uint32_t i = 0; i < 3 * MC_STORE_MAX_RECS + 7; i++) {
		w.reserved = 1000 + i; w.adv_ts = 5000 + i;
		CHECK(mc_store_append(&st, &w) == MC_STORE_OK);
	}
	CHECK(mc_store_load(&st, &fl, &p) == MC_STORE_OK);
	CHECK(p.reserved == 1000 + 3 * MC_STORE_MAX_RECS + 6 && !memcmp(st.prv, prv, 64));
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
			uint32_t acked = 0;
			for (uint32_t i = 0; i < MC_STORE_MAX_RECS - 3; i++) {
				struct mc_persist q = { 10 + i, 10 + i };
				mc_store_append(&gs, &q); acked = q.reserved;
			}
			g.ops = 0; g.cut_at = cut; g.cut_partial = partial;
			int in_roll = 0;
			for (uint32_t i = 0; i < 8 && !g.dead; i++) {
				struct mc_persist q = { 1000 + i, 1000 + i };
				if (gs.next_slot >= MC_STORE_MAX_RECS) in_roll = 1;
				int rc = mc_store_append(&gs, &q);
				if (rc == MC_STORE_OK) acked = q.reserved;
			}
			struct mc_store ls; struct mc_persist lp;
			int rc = mc_store_load(&ls, &gf, &lp);
			if (rc == MC_STORE_OK) {
				CHECK(!memcmp(ls.prv, prv, 64));
				CHECK(lp.reserved >= acked || (in_roll && lp.reserved == 0));
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
	test_cmd();
	test_evlog();
	run_ctl_tests();
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
