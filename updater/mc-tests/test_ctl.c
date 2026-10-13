/*
 * Host tests for the safety logic around START: RSSI qualification, target
 * identity, the one-shot authorisation, progress throttling, the fail-safe
 * behaviours of the controller, and the flash-layout arithmetic.
 *
 * Shares g_checks / g_fail and the fakes (test_fake.h) with test_mc.c.
 */
#include "test_fake.h"

#include "../src/mc/mc_authz.h"
#include "../src/mc/mc_gate.h"
#include "../src/mc/mc_layout.h"
#include "../src/mc/mc_progress.h"
#include "../src/mc/mc_replay.h"
#include "../src/mc/mc_target.h"
#include "../src/mc/mc_pathguard.h"

/* ================================================================= layout == */

/* The map the real build generates (see MESHCORE.md). If the board's partition
 * table changes, the BUILD_ASSERTs in mc_zephyr.c catch it in the firmware build;
 * these tests pin the arithmetic and the numbers. */
struct part { const char *name; uint32_t off, len; };
static const struct part MAP[] = {
	{ "softdevice", 0x000000, 0x027000 },
	{ "mcuboot",    0x027000, 0x00A000 },
	{ "slot0",      0x031000, 0x061000 },
	{ "slot1",      0x092000, 0x061000 },
	{ "mc_identity",0x0F3000, 0x001000 },
	{ "uf2_boot",   0x0F4000, 0x00C000 },
};
#define FLASH_SIZE 0x100000u

static void test_layout(void)
{
	SECTION("layout");
	CHECK(MC_RANGES_OVERLAP(0, 10, 5, 10));
	CHECK(!MC_RANGES_OVERLAP(0, 10, 10, 10));            /* adjacent is not overlap */
	CHECK(MC_RANGES_OVERLAP(5, 1, 0, 100));
	CHECK(!MC_RANGES_OVERLAP(100, 4, 0, 100));
	CHECK(MC_RANGE_INSIDE(0xFF000, 0x1000, FLASH_SIZE));
	CHECK(!MC_RANGE_INSIDE(0xFF001, 0x1000, FLASH_SIZE));

	const struct part *id = &MAP[4];

	CHECK(id->len == 4096 && id->off % 4096 == 0);
	CHECK(MC_RANGE_INSIDE(id->off, id->len, FLASH_SIZE));
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (&MAP[i] == id) continue;
		CHECK(!MC_RANGES_OVERLAP(id->off, id->len, MAP[i].off, MAP[i].len));
	}
	/* no two partitions in the map overlap at all, and the map tiles flash */
	uint32_t end = 0;
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		CHECK(MAP[i].off == end);
		end = MAP[i].off + MAP[i].len;
	}
	CHECK(end == FLASH_SIZE);
	/* a partition moved onto slot1 would be caught */
	CHECK(MC_RANGES_OVERLAP(0x0F3000 - 0x1000, 0x2000, MAP[3].off, MAP[3].len));   /* straddling slot1 */
	CHECK(MC_RANGES_OVERLAP(0x092000 + 0x60000, 0x1000, MAP[3].off, MAP[3].len));
	CHECK(MC_RANGES_OVERLAP(0x0F4000, 0x1000, MAP[5].off, MAP[5].len));
}

/* ============================================================= path guard == */

static void test_pathguard(void)
{
	SECTION("path guard (armed bundle is write-protected)");
	const char *D = "/lfs1";

	CHECK(!mc_path_blocked(D, "", "/lfs1/fw.zip"));            /* nothing armed: nothing blocked */
	CHECK(!mc_path_blocked(D, NULL, "/lfs1/fw.zip"));
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/fw.zip"));
	CHECK(mc_path_blocked(D, "fw.zip", "fw.zip"));
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1//fw.zip") );
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/./fw.zip"));
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/sub/../fw.zip"));       /* by name */
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/fw.zip/"));
	CHECK(mc_path_blocked(D, "fw.zip", "/elsewhere/fw.zip"));  /* by name, wherever */
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1"));              /* deleting the directory */
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/"));
	CHECK(mc_path_blocked(D, "fw.zip", "/"));
	CHECK(!mc_path_blocked(D, "fw.zip", "/lfs1/other.zip"));
	CHECK(!mc_path_blocked(D, "fw.zip", "/lfs1/fw.zip2"));
	CHECK(!mc_path_blocked(D, "fw.zip", "/lfs1/xfw.zip"));
	CHECK(!mc_path_blocked(D, "fw.zip", "/lfs1/config.txt"));
	CHECK(!mc_path_blocked(D, "fw.zip", "/lfs"));
	CHECK(!mc_path_blocked(D, "fw.zip", NULL));
	/* rename: either end naming the bundle blocks it (the glue checks both) */
	CHECK(mc_path_blocked(D, "fw.zip", "/lfs1/fw.zip"));
}

/* ==================================================================== gate == */

static void feed(struct mc_gate *g, uint32_t from, uint32_t to, uint32_t step, int8_t rssi)
{
	for (uint32_t t = from; t <= to; t += step) mc_gate_sample(g, t, rssi);
}

static const struct mc_gate_cfg GC = {
	.avg_min = -65, .floor = -75, .hyst_db = 3, .hold_ms = 5000, .max_gap_ms = 3000,
	.min_samples = 8,
};

static void test_gate(void)
{
	struct mc_gate g;
	SECTION("gate");

	mc_gate_init(&g, &GC);
	CHECK(mc_gate_check(&g, 0) == MC_GATE_NOT_SEEN);

	/* hold success: -60 every 500 ms; ready only once 5 s of clean samples exist */
	feed(&g, 1000, 5500, 500, -60);
	CHECK(mc_gate_check(&g, 5500) == MC_GATE_ACQUIRING);          /* 4500 ms in */
	feed(&g, 6000, 6000, 500, -60);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);              /* 5000 ms in, 11 samples */
	CHECK(mc_gate_avg(&g) == -60 && mc_gate_min(&g) == -60);
	CHECK(mc_gate_held_ms(&g, 6000) == 5000);

	/* the window ends if the target goes quiet; ready is not remembered */
	CHECK(mc_gate_check(&g, 6000 + 3001) == MC_GATE_STALE);
	CHECK(mc_gate_check(&g, 6000 + 3002) == MC_GATE_STALE);

	/* hard floor: one sample under it throws the acquisition away */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 4500, 500, -50);
	mc_gate_sample(&g, 5000, -76);
	CHECK(mc_gate_check(&g, 5000) == MC_GATE_BELOW_FLOOR);
	CHECK(mc_gate_held_ms(&g, 5000) == 0);
	feed(&g, 5500, 10000, 500, -50);
	CHECK(mc_gate_check(&g, 10000) == MC_GATE_ACQUIRING);         /* restarted at 5500 */
	feed(&g, 10500, 10500, 500, -50);
	CHECK(mc_gate_check(&g, 10500) == MC_GATE_READY);

	/* floor resets even when the *average* would still pass */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 10000, 500, -40);
	CHECK(mc_gate_check(&g, 10000) == MC_GATE_READY);
	mc_gate_sample(&g, 10500, -80);
	CHECK(mc_gate_check(&g, 10500) == MC_GATE_BELOW_FLOOR);
	mc_gate_sample(&g, 11000, -40);
	CHECK(mc_gate_check(&g, 11000) != MC_GATE_READY);             /* must re-earn it */

	/* the floor is inclusive: -75 is not a reset (but its average is too weak) */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 6000, 500, -75);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_WEAK_AVG);

	/* a weak average fails even above the floor */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 6000, 500, -70);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_WEAK_AVG);
	/* the threshold itself is inclusive */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 6000, 500, -65);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);

	/* a noisy-but-fine link: swings of +-8 dB around -60 pass on the mean */
	mc_gate_init(&g, &GC);
	for (uint32_t t = 0; t <= 6000; t += 400) mc_gate_sample(&g, t, (t / 400) % 2 ? -68 : -52);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);
	CHECK(mc_gate_avg(&g) == -60 && mc_gate_min(&g) == -68);

	/* insufficient samples: held long enough, but only 3 adverts in the window */
	mc_gate_init(&g, &GC);
	mc_gate_sample(&g, 0, -50);
	mc_gate_sample(&g, 2900, -50);
	mc_gate_sample(&g, 5800, -50);
	CHECK(mc_gate_check(&g, 5800) == MC_GATE_FEW_SAMPLES);
	/* ...and exactly min_samples is enough */
	mc_gate_init(&g, &GC);
	for (int i = 0; i < 7; i++) mc_gate_sample(&g, (uint32_t)i * 100, -50);   /* 7 fast */
	mc_gate_sample(&g, 2500, -50);
	mc_gate_sample(&g, 5000, -50);                                            /* 9th */
	CHECK(mc_gate_check(&g, 5000) == MC_GATE_READY);
	mc_gate_init(&g, &GC);
	for (int i = 0; i < 6; i++) mc_gate_sample(&g, (uint32_t)i * 100, -50);
	mc_gate_sample(&g, 2500, -50);
	mc_gate_sample(&g, 5000, -50);                                            /* 8th */
	CHECK(mc_gate_check(&g, 5000) == MC_GATE_READY);
	mc_gate_init(&g, &GC);
	for (int i = 0; i < 5; i++) mc_gate_sample(&g, (uint32_t)i * 100, -50);
	mc_gate_sample(&g, 2500, -50);
	mc_gate_sample(&g, 5000, -50);                                            /* 7th */
	CHECK(mc_gate_check(&g, 5000) == MC_GATE_FEW_SAMPLES);

	/* silence beyond max_gap restarts the acquisition even if it recovers */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 2000, 500, -50);
	feed(&g, 6000, 6000, 500, -50);                                           /* 4 s gap */
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_ACQUIRING);
	feed(&g, 6500, 11000, 500, -50);
	CHECK(mc_gate_check(&g, 11000) == MC_GATE_READY);

	/* hysteresis: once ready, a few mildly weak packets do not flap it */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 6000, 500, -62);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);
	mc_gate_sample(&g, 6500, -70);
	CHECK(mc_gate_check(&g, 6500) == MC_GATE_READY);
	mc_gate_sample(&g, 7000, -70);
	CHECK(mc_gate_check(&g, 7000) == MC_GATE_READY);
	/* sustained decline below avg_min - hyst (-68) does drop it... */
	for (uint32_t t = 7500; t <= 14000; t += 500) mc_gate_sample(&g, t, -72);
	CHECK(mc_gate_check(&g, 14000) == MC_GATE_WEAK_AVG);
	/* ...and recovering the mean above avg_min (not merely above -68) restores it */
	for (uint32_t t = 14500; t <= 22000; t += 500) mc_gate_sample(&g, t, -64);
	CHECK(mc_gate_check(&g, 22000) == MC_GATE_READY);
	/* a latched-but-falling mean between -65 and -68 is still READY (hysteresis) */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 6000, 500, -64);
	CHECK(mc_gate_check(&g, 6000) == MC_GATE_READY);
	for (uint32_t t = 6500; t <= 20000; t += 500) mc_gate_sample(&g, t, -67);
	CHECK(mc_gate_check(&g, 20000) == MC_GATE_READY);
	/* a fresh acquisition at -67 (never latched) is not ready */
	mc_gate_init(&g, &GC);
	feed(&g, 0, 20000, 500, -67);
	CHECK(mc_gate_check(&g, 20000) == MC_GATE_WEAK_AVG);

	/* configurable hold */
	struct mc_gate_cfg c2 = GC;
	c2.hold_ms = 8000;
	mc_gate_init(&g, &c2);
	feed(&g, 0, 7500, 500, -50);
	CHECK(mc_gate_check(&g, 7500) == MC_GATE_ACQUIRING);
	feed(&g, 8000, 8000, 500, -50);
	CHECK(mc_gate_check(&g, 8000) == MC_GATE_READY);

	/* millisecond clock wrap */
	mc_gate_init(&g, &GC);
	for (uint32_t i = 0; i <= 12; i++) mc_gate_sample(&g, 0xFFFFFF00u + i * 500u, -50);
	CHECK(mc_gate_check(&g, 0xFFFFFF00u + 12 * 500u) == MC_GATE_READY);
}

/* ================================================================= authz == */

static void test_authz(void)
{
	struct mc_authz a;
	const char *P = "/lfs1/fw.zip", *N = "EA:7C:11:0B:55:90 (random)";
	SECTION("authz (one-shot START authorisation)");

	mc_authz_init(&a);
	CHECK(mc_authz_state(&a) == MC_AZ_IDLE);
	/* nothing to consume without a grant: auto-flash / SMP TRIGGER_DFU paths */
	CHECK(!mc_authz_consume(&a, WHO_MC, P, N));
	CHECK(!mc_authz_consume(&a, WHO_OTHER, P, N));
	CHECK(!mc_authz_consume(&a, WHO_MC, "", ""));
	CHECK(mc_authz_state(&a) == MC_AZ_IDLE);

	CHECK(mc_authz_grant(&a, WHO_MC, P, N));
	CHECK(mc_authz_state(&a) == MC_AZ_START_AUTHORIZED);
	CHECK(!mc_authz_grant(&a, WHO_MC, P, N));                  /* no second grant on top */

	/* wrong thread, wrong path, wrong pin: refused, grant untouched */
	CHECK(!mc_authz_consume(&a, WHO_OTHER, P, N));
	CHECK(!mc_authz_consume(&a, WHO_MC, "/lfs1/other.zip", N));
	CHECK(!mc_authz_consume(&a, WHO_MC, P, "EA:7C:11:0B:55:91 (random)"));
	CHECK(!mc_authz_consume(&a, WHO_MC, P, "EA:7C:11:0B:55:90 (public)"));
	CHECK(!mc_authz_consume(&a, WHO_MC, NULL, N));
	CHECK(mc_authz_state(&a) == MC_AZ_START_AUTHORIZED);

	/* the right caller consumes it exactly once */
	CHECK(mc_authz_consume(&a, WHO_MC, P, N));
	CHECK(mc_authz_state(&a) == MC_AZ_DFU_ACTIVE);
	CHECK(!mc_authz_consume(&a, WHO_MC, P, N));                /* double START */
	CHECK(!mc_authz_consume(&a, WHO_OTHER, P, N));
	CHECK(!mc_authz_grant(&a, WHO_MC, P, N));                  /* not while active */

	mc_authz_end(&a);
	CHECK(mc_authz_state(&a) == MC_AZ_IDLE);
	CHECK(!mc_authz_consume(&a, WHO_MC, P, N));                /* nothing left to reuse */

	/* grant, then the start is refused: end() clears it, nothing lingers */
	CHECK(mc_authz_grant(&a, WHO_MC, P, N));
	mc_authz_end(&a);
	CHECK(!mc_authz_consume(&a, WHO_MC, P, N));

	/* oversize strings are refused, not truncated into a different identity */
	char big[200];
	memset(big, 'a', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
	CHECK(!mc_authz_grant(&a, WHO_MC, big, N));
	CHECK(!mc_authz_grant(&a, WHO_MC, P, big));
	CHECK(mc_authz_state(&a) == MC_AZ_IDLE);
}

/* ================================================================= target == */

static void test_target(void)
{
	SECTION("target identity");
	const struct mc_target_cfg T = { .require_dfu_uuid = true, .dfu_name = "4631_DFU", .allow_plus1 = true };
	uint8_t a[6], s[6];

	mc_addr_to_le("EA:7C:11:0B:55:90", a);
	memcpy(s, a, 6);

	/* the address the operator typed, right type */
	CHECK(mc_target_check(&T, a, 'r', s, 1, false, "") == MC_TGT_EXACT);
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, "whatever") == MC_TGT_EXACT);
	CHECK(mc_target_check(&T, a, 'p', s, 0, false, NULL) == MC_TGT_EXACT);
	/* wrong address type, same six octets */
	CHECK(mc_target_check(&T, a, 'r', s, 0, true, "4631_DFU") == MC_TGT_REJECT_TYPE);
	CHECK(mc_target_check(&T, a, 'p', s, 1, true, "4631_DFU") == MC_TGT_REJECT_TYPE);

	/* Nordic DFU address: A + 1 */
	mc_addr_plus_one(a, s);
	CHECK(s[0] == 0x91 && s[5] == 0xEA);
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, "4631_DFU") == MC_TGT_DFU_PLUS1);
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, "") == MC_TGT_DFU_PLUS1);       /* name not in this packet */
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, NULL) == MC_TGT_DFU_PLUS1);
	CHECK(mc_target_accepts(MC_TGT_DFU_PLUS1) && mc_target_accepts(MC_TGT_EXACT));
	/* A+1 without the DFU service UUID is just a neighbour */
	CHECK(mc_target_check(&T, a, 'r', s, 1, false, "4631_DFU") == MC_TGT_REJECT_UUID);
	/* A+1 with the UUID but the wrong name */
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, "EvilDFU") == MC_TGT_REJECT_NAME);
	CHECK(mc_target_check(&T, a, 'r', s, 1, true, "4631_DFU2") == MC_TGT_REJECT_NAME);
	/* A+1 with the wrong type */
	CHECK(mc_target_check(&T, a, 'r', s, 0, true, "4631_DFU") == MC_TGT_REJECT_TYPE);
	/* a name never makes the wrong address acceptable */
	uint8_t o[6];
	memcpy(o, a, 6); o[0] = 0x92;                                                  /* A + 2 */
	CHECK(mc_target_check(&T, a, 'r', o, 1, true, "4631_DFU") == MC_TGT_REJECT_ADDR);
	memcpy(o, a, 6); o[0] = 0x8F;                                                  /* A - 1 */
	CHECK(mc_target_check(&T, a, 'r', o, 1, true, "4631_DFU") == MC_TGT_REJECT_ADDR);
	memcpy(o, a, 6); o[5] ^= 0x80;
	CHECK(mc_target_check(&T, a, 'r', o, 1, true, "4631_DFU") == MC_TGT_REJECT_ADDR);
	CHECK(!mc_target_accepts(MC_TGT_REJECT_ADDR) && !mc_target_accepts(MC_TGT_REJECT_TYPE) &&
	      !mc_target_accepts(MC_TGT_REJECT_UUID) && !mc_target_accepts(MC_TGT_REJECT_NAME) &&
	      !mc_target_accepts(MC_TGT_REJECT_PLUS1_OFF));

	/* carry across octets, both ends */
	uint8_t c1[6] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x10 }, c2[6];
	mc_addr_plus_one(c1, c2);
	CHECK(c2[0] == 0 && c2[1] == 0 && c2[2] == 1 && c2[5] == 0x10);
	uint8_t c3[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	mc_addr_plus_one(c3, c2);
	CHECK(c2[0] == 0 && c2[5] == 0);
	CHECK(mc_target_check(&T, c1, 'r', c2, 1, true, "") == MC_TGT_REJECT_ADDR);

	/* plus-one disabled (armed device was already in DFU mode, or config says so) */
	struct mc_target_cfg off = T;
	off.allow_plus1 = false;
	mc_addr_plus_one(a, s);
	CHECK(mc_target_check(&off, a, 'r', s, 1, true, "4631_DFU") == MC_TGT_REJECT_PLUS1_OFF);
	CHECK(mc_target_check(&off, a, 'r', a, 1, true, "") == MC_TGT_EXACT);

	/* UUID requirement can be relaxed in config, the name check stays */
	struct mc_target_cfg nouuid = T;
	nouuid.require_dfu_uuid = false;
	CHECK(mc_target_check(&nouuid, a, 'r', s, 1, false, "4631_DFU") == MC_TGT_DFU_PLUS1);
	CHECK(mc_target_check(&nouuid, a, 'r', s, 1, false, "x") == MC_TGT_REJECT_NAME);
	/* no expected name configured: name not checked */
	struct mc_target_cfg noname = T;
	noname.dfu_name = "";
	CHECK(mc_target_check(&noname, a, 'r', s, 1, true, "anything") == MC_TGT_DFU_PLUS1);
}

/* ================================================================ progress == */

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
	SECTION("progress throttling");

	mc_progress_begin(&p);
	CHECK(prog(&p, ev(DFU_STATUS_SCANNING, 0, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_CONNECTING, 0, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 0, 0, 1), m) == 0);          /* no 0% message */
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 24, 0, 1), m) == 0);
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 25, 0, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_MILESTONE && m[0].milestone == 25);
	/* per-percent / per-packet updates between milestones say nothing */
	int sent = 0;
	for (int pc = 26; pc < 50; pc++) sent += (int)prog(&p, ev(DFU_STATUS_UPLOADING, (uint8_t)pc, 0, 1), m);
	CHECK(sent == 0);
	/* the same status repeated 1000 times says nothing */
	for (int i = 0; i < 1000; i++) sent += (int)prog(&p, ev(DFU_STATUS_UPLOADING, 30, 0, 1), m);
	CHECK(sent == 0);
	/* a jump past two milestones reports only the highest */
	n = prog(&p, ev(DFU_STATUS_UPLOADING, 80, 0, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_MILESTONE && m[0].milestone == 75);
	/* never repeated */
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 90, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 75, 0, 1), m) == 0);
	/* no 100% message: completion is reported as the result */
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 100, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_VALIDATING, 100, 0, 1), m) == 0);
	n = prog(&p, ev(DFU_STATUS_VERIFYING, 100, 0, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_VERIFYING);
	CHECK(prog(&p, ev(DFU_STATUS_VERIFYING, 100, 0, 1), m) == 0);       /* once */
	n = prog(&p, ev(DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK, 1), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_DONE);
	CHECK(prog(&p, ev(DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK, 1), m) == 0);   /* terminal once */

	/* a retry (attempt 2 starts uploading again from 0) does not re-announce */
	mc_progress_begin(&p);
	prog(&p, ev(DFU_STATUS_UPLOADING, 60, 0, 1), m);
	CHECK(prog(&p, ev(DFU_STATUS_COOLDOWN, 0, 0, 1), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 10, 0, 2), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 40, 0, 2), m) == 0);        /* 25 and 50 already out */

	/* failure: one message with the reason, then nothing */
	mc_progress_begin(&p);
	prog(&p, ev(DFU_STATUS_UPLOADING, 30, 0, 1), m);
	n = prog(&p, ev(DFU_STATUS_FAILED, 31, DFU_STATUS_RESULT_DISCONNECTED, 3), m);
	CHECK(n == 1 && m[0].kind == MC_MSG_FAILED && m[0].result == DFU_STATUS_RESULT_DISCONNECTED);
	CHECK(prog(&p, ev(DFU_STATUS_FAILED, 31, DFU_STATUS_RESULT_DISCONNECTED, 3), m) == 0);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 90, 0, 1), m) == 0);

	/* inactive until begun, and after end() */
	mc_progress_end(&p);
	CHECK(prog(&p, ev(DFU_STATUS_UPLOADING, 90, 0, 1), m) == 0);

	/* a whole run in one stream of 1000 events produces at most 6 messages */
	mc_progress_begin(&p);
	int total = 0;
	for (int i = 0; i <= 100; i++) {
		for (int k = 0; k < 10; k++) total += (int)prog(&p, ev(DFU_STATUS_UPLOADING, (uint8_t)i, 0, 1), m);
	}
	total += (int)prog(&p, ev(DFU_STATUS_VERIFYING, 100, 0, 1), m);
	total += (int)prog(&p, ev(DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK, 1), m);
	CHECK(total == 5);                                          /* 25, 50, 75, verifying, success */
}

/* ================================================================== replay == */

static void test_replay(void)
{
	struct mc_replay r;
	SECTION("replay (duplicate suppression only)");
	mc_replay_init(&r);
	CHECK(mc_replay_accept(&r, 1) == MC_REPLAY_OK);
	CHECK(mc_replay_accept(&r, 1) == MC_REPLAY_DUPLICATE);
	CHECK(mc_replay_accept(&r, 2) == MC_REPLAY_OK);
	CHECK(mc_replay_digest("a", 1) != mc_replay_digest("b", 1));
	/* the ring forgets after MC_REPLAY_RING newer entries: that is why the
	 * authority is the single-use token, not this ring */
	for (uint32_t i = 10; i < 10 + MC_REPLAY_RING; i++) mc_replay_accept(&r, i);
	CHECK(mc_replay_accept(&r, 1) == MC_REPLAY_OK);
	/* a reboot (fresh ring) forgets everything: replay then relies on tokens */
	mc_replay_init(&r);
	CHECK(mc_replay_accept(&r, 2) == MC_REPLAY_OK);
}

/* =============================================================== controller == */

static bool has_note(const struct fake *f, const char *s)
{
	for (int i = 0; i < f->nnotes; i++) if (strstr(f->notes[i], s)) return true;
	return false;
}

static bool has_report(const struct fake *f, const char *s)
{
	for (int i = 0; i < f->nreports; i++) if (strstr(f->reports[i], s)) return true;
	return false;
}

static void start_with_token(struct mc_ctl *c) { start_tok(c); }

/* An accepted START, leaving the controller in DFU_ACTIVE. */
static void run_to_active(struct mc_ctl *c, struct fake *f)
{
	get_armed(c, f);
	start_with_token(c);
	(void)f;
}

static void dfu_ev(struct mc_ctl *c, uint8_t st, uint8_t pct, uint8_t res)
{
	struct dfu_status_event e = { st, pct, res, 1, 5 };
	mc_ctl_on_dfu_event(c, &e);
}

static void test_ctl(void)
{
	struct mc_ctl c;
	struct fake f;
	char cmd[160];
	SECTION("controller");

	/* ---- boots IDLE; unprivileged senders cannot command -------------- */
	ctl_setup(&c, &f);
	CHECK(c.state == MC_IDLE);
	H(&c, true, "status");
	CHECK(strstr(R, "IDLE") && strstr(R, "tok="));
	H(&c, false, "arm " ADDR " fw.zip");
	CHECK(strstr(R, "DENIED") && c.state == MC_IDLE && f.digest_calls == 0);
	H(&c, false, "start"); CHECK(strstr(R, "DENIED"));
	H(&c, false, "abort"); CHECK(strstr(R, "DENIED"));
	H(&c, false, "disarm"); CHECK(strstr(R, "DENIED"));
	H(&c, false, "rssi"); CHECK(strstr(R, "not armed"));     /* read-only commands are open */
	H(&c, false, "version"); CHECK(strstr(R, "0.1.0+test"));
	H(&c, false, "battery"); CHECK(strstr(R, "3.91V") && strstr(R, "71%"));
	H(&c, false, "log 1"); CHECK(strstr(R, "log page 1"));
	H(&c, false, "scan"); CHECK(strstr(R, "scan 1/2") && strstr(R, "RAK4631_OTA"));
	H(&c, false, "scan 2"); CHECK(strstr(R, "scan 2/2"));
	H(&c, false, "scan 9"); CHECK(strstr(R, "no page 9"));
	H(&c, false, "frobnicate"); CHECK(strstr(R, "? cmds"));

	/* ---- START while unarmed ------------------------------------------ */
	H(&c, true, "start");
	CHECK(strstr(R, "not armed") && f.start_calls == 0);
	start_with_token(&c);
	CHECK(strstr(R, "not armed") && f.start_calls == 0 && f.az.state == MC_AZ_IDLE);
	H(&c, true, "abort"); CHECK(strstr(R, "nothing running"));
	H(&c, true, "disarm"); CHECK(strstr(R, "already IDLE"));

	/* ---- tokens -------------------------------------------------------- */
	ctl_setup(&c, &f);
	H(&c, true, ARM_NOTOK);                                   /* no token */
	CHECK(strstr(R, "needs token") && c.state == MC_IDLE && f.digest_calls == 0);
	char t1[8]; snprintf(t1, sizeof(t1), "%s", tok_of(R));
	CHECK(strlen(t1) == 6);
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", "000000");     /* wrong token */
	H(&c, true, cmd);
	CHECK(strstr(R, "needs token") && c.state == MC_IDLE);
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", t1);           /* right token */
	H(&c, true, cmd);
	CHECK(strstr(R, "ARM ok FW ") && c.state == MC_WAITING);
	/* replay of the very same command: the token is spent */
	H(&c, true, "disarm");
	H(&c, true, cmd);
	CHECK(strstr(R, "needs token") && c.state == MC_IDLE);
	/* a token expires */
	H(&c, true, "status");
	snprintf(t1, sizeof(t1), "%s", tok_of(R));
	f.now += 121 * 1000;
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", t1);
	H(&c, true, cmd);
	CHECK(strstr(R, "needs token") && c.state == MC_IDLE);
	CHECK(strcmp(tok_of(R), t1) != 0);                         /* a fresh one is offered */
	/* upper-case hex is accepted */
	snprintf(t1, sizeof(t1), "%s", tok_of(R));
	for (char *p = t1; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", t1);
	H(&c, true, cmd);
	CHECK(strstr(R, "ARM ok") && c.state == MC_WAITING);
	/* an unrelated (wrong) attempt does not burn the live token */
	H(&c, true, "disarm");
	H(&c, true, "status");
	snprintf(t1, sizeof(t1), "%s", tok_of(R));
	H(&c, true, "arm " ADDR " fw.zip 000000");
	CHECK(strstr(R, "needs token"));
	CHECK(strcmp(tok_of(R), t1) == 0);                         /* same live token */

	/* ---- reboot: a fresh controller is IDLE and an old token is dead -- */
	ctl_setup(&c, &f);
	CHECK(c.state == MC_IDLE && c.token_valid == false);
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", t1);
	H(&c, true, cmd);
	CHECK(c.state == MC_IDLE);

	/* ---- the happy path, state by state --------------------------------- */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	CHECK(c.state == MC_WAITING && !strcmp(f.watching, ADDR) && f.lock_calls == 1);
	CHECK(strstr(R, "WAITING") && strstr(R, "FW ") && c.fw[0] != '\0');
	char fw[16]; snprintf(fw, sizeof(fw), "%s", c.fw);
	CHECK(strlen(fw) == 8);
	H(&c, true, "status");
	CHECK(strstr(R, "WAITING") && strstr(R, fw) && strstr(R, "TARGET not seen") && strstr(R, "exp"));
	H(&c, true, "rssi");
	CHECK(strstr(R, "TARGET not seen"));
	mc_ctl_on_adv(&c, -60, false);
	CHECK(c.state == MC_TARGET_DETECTED);
	f.now += 500;
	for (int i = 0; i < 9; i++) { mc_ctl_on_adv(&c, -60, false); f.now += 500; }
	CHECK(c.state == MC_TARGET_DETECTED);                      /* ~4.5 s, not 5 */
	mc_ctl_on_adv(&c, -60, false);
	CHECK(c.state == MC_ARMED);                                /* QUALIFIED passed through */
	CHECK(has_note(&f, "TARGET_DETECTED -> RSSI_QUALIFIED") && has_note(&f, "RSSI_QUALIFIED -> ARMED"));
	H(&c, true, "rssi");
	CHECK(strstr(R, "TARGET -60 AVG -60 MIN -60") && strstr(R, "/5s"));
	H(&c, true, "status");
	CHECK(strstr(R, "ARMED") && strstr(R, "FW ") && strstr(R, fw));
	snprintf(cmd, sizeof(cmd), "start %s", tok_of(R));
	H(&c, true, cmd);
	CHECK(strstr(R, "STARTED FW ") && strstr(R, fw));
	CHECK(f.start_calls == 1 && c.state == MC_DFU_ACTIVE);
	CHECK(f.state_at_start == MC_START_AUTHORIZED);            /* never observable outside START */
	CHECK(f.az_at_start == MC_AZ_START_AUTHORIZED && f.runner_consumed);
	CHECK(!f.bypass_ok);                                       /* another thread could not take it */
	CHECK(f.az.state == MC_AZ_DFU_ACTIVE);
	CHECK(!strcmp(f.start_bundle, "fw.zip") && !strcmp(f.start_addr, ADDR) && f.start_type == 'r');
	CHECK(f.start_plus1 == true);                              /* seen in app mode */
	CHECK(f.watching[0] == '\0');                              /* stopped watching */
	CHECK(f.locked[0] != '\0');                                /* bundle stays locked during the run */

	/* ---- double START ---------------------------------------------------- */
	H(&c, true, "start");
	CHECK(strstr(R, "already running") && f.start_calls == 1);
	start_with_token(&c);
	CHECK(strstr(R, "already running") && f.start_calls == 1);
	H(&c, true, "arm " ADDR " other.zip");
	CHECK(strstr(R, "refused: DFU running") && c.state == MC_DFU_ACTIVE);
	H(&c, true, "disarm");
	CHECK(strstr(R, "DFU running") && c.state == MC_DFU_ACTIVE);
	/* the authorisation is spent: nothing else can start a run */
	CHECK(!mc_authz_consume(&f.az, WHO_MC, "/lfs1/fw.zip", "EA:7C:11:0B:55:90 (random)"));
	CHECK(!mc_authz_consume(&f.az, WHO_OTHER, "/lfs1/fw.zip", "EA:7C:11:0B:55:90 (random)"));

	/* ---- milestones: the only unsolicited traffic -------------------------- */
	f.nreports = 0;
	dfu_ev(&c, DFU_STATUS_SCANNING, 0, 0);
	dfu_ev(&c, DFU_STATUS_CONNECTING, 0, 0);
	dfu_ev(&c, DFU_STATUS_UPLOADING, 3, 0);
	for (int p = 4; p < 25; p++) dfu_ev(&c, DFU_STATUS_UPLOADING, (uint8_t)p, 0);
	CHECK(f.nreports == 0);
	dfu_ev(&c, DFU_STATUS_UPLOADING, 25, 0);
	CHECK(f.nreports == 1 && !strcmp(f.reports[0], "DFU 25%"));
	/* 50 and 75 arrive within the report gap: the newer replaces the older queued one */
	f.now += 1000;
	dfu_ev(&c, DFU_STATUS_UPLOADING, 50, 0);
	CHECK(f.nreports == 1);                                    /* held back by the gap */
	f.now += 1000;
	dfu_ev(&c, DFU_STATUS_UPLOADING, 75, 0);
	CHECK(f.nreports == 1);
	f.now += 4000; mc_ctl_tick(&c);
	CHECK(f.nreports == 2 && !strcmp(f.reports[1], "DFU 75%"));  /* 50 was superseded */
	dfu_ev(&c, DFU_STATUS_UPLOADING, 100, 0);
	dfu_ev(&c, DFU_STATUS_VALIDATING, 100, 0);
	dfu_ev(&c, DFU_STATUS_VERIFYING, 100, 0);
	f.now += 4000; mc_ctl_tick(&c);
	CHECK(has_report(&f, "DFU verifying"));
	/* terminal messages are never lost and return the controller to IDLE */
	dfu_ev(&c, DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK);
	f.now += 4000; mc_ctl_tick(&c);
	CHECK(has_report(&f, "DFU SUCCESS " ADDR " FW "));
	CHECK(c.state == MC_IDLE && f.az.state == MC_AZ_IDLE && f.locked[0] == '\0');
	CHECK(f.end_calls >= 1);
	H(&c, true, "status");
	CHECK(strstr(R, "IDLE last=ok"));
	CHECK(f.nreports <= 6);

	/* ---- DFU failure mid-transfer ------------------------------------------ */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	CHECK(c.state == MC_DFU_ACTIVE);
	dfu_ev(&c, DFU_STATUS_UPLOADING, 40, 0);
	f.now += 5000;
	dfu_ev(&c, DFU_STATUS_FAILED, 41, DFU_STATUS_RESULT_DISCONNECTED);
	f.now += 5000; mc_ctl_tick(&c);
	CHECK(has_report(&f, "DFU FAILED " ADDR ": disconnected"));
	CHECK(c.state == MC_IDLE && f.az.state == MC_AZ_IDLE && f.locked[0] == '\0');
	H(&c, true, "status");
	CHECK(strstr(R, "last=failed") && strstr(R, "disconnected"));
	/* ...safe: a new START needs a full re-arm */
	H(&c, true, "start");
	CHECK(strstr(R, "not armed"));
	CHECK(f.start_calls == 1);

	/* ---- DFU connection fails right away ------------------------------------ */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	dfu_ev(&c, DFU_STATUS_SCANNING, 0, 0);
	dfu_ev(&c, DFU_STATUS_FAILED, 0, DFU_STATUS_RESULT_CONNECT_FAILED);
	f.now += 5000; mc_ctl_tick(&c);
	CHECK(has_report(&f, "connect failed") && c.state == MC_IDLE && f.az.state == MC_AZ_IDLE);
	/* and the controller can be armed again straight away */
	get_armed(&c, &f);
	CHECK(c.state == MC_ARMED);

	/* ---- runner refuses to start (busy / error): back to ARMED, no authorisation left */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	f.start_rc = -16;
	start_with_token(&c);
	CHECK(strstr(R, "START failed") && c.state == MC_ARMED && f.az.state == MC_AZ_IDLE);
	CHECK(!f.runner_consumed);
	/* then a retry works once the runner is fine */
	f.start_rc = 0;
	start_with_token(&c);
	CHECK(strstr(R, "STARTED") && c.state == MC_DFU_ACTIVE);

	/* ---- runner already busy: START refused before any authorisation exists */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	f.busy = true;
	start_with_token(&c);
	CHECK(strstr(R, "runner busy") && f.start_calls == 0 && f.az.state == MC_AZ_IDLE);
	CHECK(c.state == MC_ARMED);

	/* ---- ABORT before DFU starts ---------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	H(&c, true, "abort");
	CHECK(strstr(R, "was only armed") && c.state == MC_IDLE && f.abort_calls == 0);
	CHECK(f.locked[0] == '\0' && f.watching[0] == '\0');
	H(&c, true, "start");
	CHECK(strstr(R, "not armed"));

	/* ---- ABORT during DFU: allowed only while the target app is intact ----------- */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	dfu_ev(&c, DFU_STATUS_SCANNING, 0, 0);
	H(&c, true, "abort");
	CHECK(strstr(R, "ABORTED") && c.state == MC_IDLE && f.abort_calls == 1 && f.az.state == MC_AZ_IDLE);
	H(&c, true, "status"); CHECK(strstr(R, "last=aborted"));
	/* connecting, enabling (buttonless jump not done yet), cooldown, awaiting PIN: also fine */
	uint8_t early[] = { DFU_STATUS_CONNECTING, DFU_STATUS_ENABLING, DFU_STATUS_COOLDOWN, DFU_STATUS_AWAITING_PIN };
	for (size_t i = 0; i < sizeof(early); i++) {
		ctl_setup(&c, &f);
		run_to_active(&c, &f);
		dfu_ev(&c, early[i], 0, 0);
		H(&c, true, "abort");
		CHECK(strstr(R, "ABORTED") && c.state == MC_IDLE);
	}
	/* once the DFU START packet is out, the target app is erased: refuse, clearly */
	uint8_t late[] = { DFU_STATUS_STARTING, DFU_STATUS_UPLOADING, DFU_STATUS_VALIDATING,
			   DFU_STATUS_DISCONNECTING, DFU_STATUS_VERIFYING };
	for (size_t i = 0; i < sizeof(late); i++) {
		ctl_setup(&c, &f);
		run_to_active(&c, &f);
		dfu_ev(&c, late[i], 10, 0);
		H(&c, true, "abort");
		CHECK(strstr(R, "refused") && strstr(R, "erased") && c.state == MC_DFU_ACTIVE);
		CHECK(f.abort_calls == 0 && f.az.state == MC_AZ_DFU_ACTIVE);
	}

	/* ---- the run is reset from elsewhere (SMP stop): stand down, tell the operator */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	dfu_ev(&c, DFU_STATUS_UPLOADING, 10, 0);
	dfu_ev(&c, DFU_STATUS_IDLE, 0, 0);
	f.now += 5000; mc_ctl_tick(&c);
	CHECK(c.state == MC_IDLE && has_report(&f, "DFU stopped") && f.az.state == MC_AZ_IDLE);

	/* ---- ARM timeout (configurable) ---------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	CHECK(c.state == MC_ARMED);
	f.now += 30 * 1000; mc_ctl_tick(&c);
	CHECK(c.state != MC_IDLE);                                  /* well inside 60 s from arm? */
	uint32_t armed_at = c.armed_at;
	f.now = armed_at + 59 * 1000; mc_ctl_tick(&c);
	CHECK(c.state != MC_IDLE);
	f.now = armed_at + 60 * 1000; mc_ctl_tick(&c);
	CHECK(c.state == MC_IDLE && has_report(&f, "ARM expired") && f.locked[0] == '\0');
	H(&c, true, "status"); CHECK(strstr(R, "last=expired"));
	start_with_token(&c); CHECK(strstr(R, "not armed") && f.start_calls == 0);
	/* a different timeout */
	struct mc_cfg c10 = CFG;
	c10.arm_timeout_s = 10;
	ctl_setup(&c, &f);
	mc_ctl_init(&c, &FOPS, &f, &c10);
	get_armed(&c, &f);                                          /* takes 6 s of adverts */
	f.now += 5000; mc_ctl_tick(&c);
	CHECK(c.state == MC_IDLE);
	/* timeout 0 = never */
	struct mc_cfg c0 = CFG;
	c0.arm_timeout_s = 0;
	mc_ctl_init(&c, &FOPS, &f, &c0);
	get_armed(&c, &f);
	f.now += 3600u * 1000; mc_ctl_tick(&c);
	CHECK(c.state != MC_IDLE);
	/* the timeout also covers an arm that never saw its target */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	f.now += 60 * 1000; mc_ctl_tick(&c);
	CHECK(c.state == MC_IDLE && f.watching[0] == '\0');

	/* ---- target disappears before START ---------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	CHECK(c.state == MC_ARMED);
	f.now += 3500;                                              /* silence > max_gap */
	mc_ctl_tick(&c);
	CHECK(c.state == MC_TARGET_DETECTED);
	start_with_token(&c);
	CHECK(strstr(R, "START refused") && f.start_calls == 0 && f.az.state == MC_AZ_IDLE);
	/* START evaluated at its own instant, even if the tick has not run yet */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	f.now += 3500;
	start_with_token(&c);
	CHECK(strstr(R, "START refused") && f.start_calls == 0);
	CHECK(c.state == MC_TARGET_DETECTED);
	/* it recovers by being seen again for the full hold */
	adv_run(&c, &f, 6000, 500, -60);
	CHECK(c.state == MC_ARMED);

	/* ---- RSSI drops during qualification ---------------------------------------------- */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	adv_run(&c, &f, 3500, 500, -60);
	mc_ctl_on_adv(&c, -80, false);                              /* below the hard floor */
	f.now += 500;
	CHECK(c.state == MC_TARGET_DETECTED);
	adv_run(&c, &f, 4000, 500, -60);                            /* only 4 s since the reset */
	CHECK(c.state == MC_TARGET_DETECTED);
	adv_run(&c, &f, 2000, 500, -60);
	CHECK(c.state == MC_ARMED);
	/* weak average: -70 steady never qualifies */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	adv_run(&c, &f, 20000, 500, -70);
	CHECK(c.state == MC_TARGET_DETECTED);
	H(&c, true, "rssi");
	CHECK(strstr(R, "avg too weak"));
	/* ...and RSSI dropping once ARMED walks it back, never forward */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	adv_run(&c, &f, 15000, 500, -73);                           /* mean slides below -68 */
	CHECK(c.state == MC_TARGET_DETECTED);

	/* ---- START with the target not seen yet ----------------------------------------- */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	start_with_token(&c);
	CHECK(strstr(R, "START refused") && strstr(R, "WAITING") && f.start_calls == 0);

	/* ---- bundle changed after ARM --------------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	strcpy(f.content, "firmware v2 (swapped)");
	start_with_token(&c);
	CHECK(strstr(R, "bundle changed") && c.state == MC_IDLE && f.start_calls == 0);
	CHECK(f.locked[0] == '\0' && f.az.state == MC_AZ_IDLE);
	H(&c, true, "status"); CHECK(strstr(R, "last=bundle changed"));
	/* same name, same size, different content: the digest still catches it */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	strcpy(f.content, "firmware v2");                           /* same length as "firmware v1" */
	start_with_token(&c);
	CHECK(strstr(R, "bundle changed") && f.start_calls == 0);
	/* changed while still qualifying: cancelled when the gate completes */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	adv_run(&c, &f, 3000, 500, -60);
	strcpy(f.content, "tampered");
	adv_run(&c, &f, 3500, 500, -60);
	CHECK(c.state == MC_IDLE && has_report(&f, "bundle") && f.locked[0] == '\0');
	/* removed after ARM */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	f.have_bundle = false;
	start_with_token(&c);
	CHECK(strstr(R, "bundle changed") && f.start_calls == 0);
	/* unchanged: START goes through, and the digest was verified at START */
	ctl_setup(&c, &f);
	int before = f.digest_calls;
	get_armed(&c, &f);
	int mid = f.digest_calls;
	start_with_token(&c);
	CHECK(mid > before && f.digest_calls > mid && strstr(R, "STARTED"));
	/* ARM on a missing bundle is refused up front */
	ctl_setup(&c, &f);
	f.have_bundle = false;
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	CHECK(strstr(R, "unreadable") && c.state == MC_IDLE && f.lock_calls == 0);

	/* ---- digest identity is the content, shown short ----------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	char fw1[16]; snprintf(fw1, sizeof(fw1), "%s", c.fw);
	ctl_setup(&c, &f);
	strcpy(f.content, "firmware v3");
	get_armed(&c, &f);
	CHECK(strcmp(fw1, c.fw) != 0);
	H(&c, true, "status");
	CHECK(strstr(R, "ARMED FW ") && strstr(R, c.fw));
	/* the identity is immutable for the session: fw does not change while armed */
	char fw2[16]; snprintf(fw2, sizeof(fw2), "%s", c.fw);
	strcpy(f.content, "something else");
	mc_ctl_tick(&c);
	CHECK(!strcmp(fw2, c.fw) || c.state == MC_IDLE);
	/* status mid-run keeps showing the armed FW id */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	char fw3[16]; snprintf(fw3, sizeof(fw3), "%s", c.fw);
	strcpy(f.content, "changed during run");
	H(&c, true, "status");
	CHECK(strstr(R, "DFU_ACTIVE") && strstr(R, fw3));

	/* ---- re-arm replaces the selection; WRONG target is not the armed one ---------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), "arm C0:11:22:33:44:55 fw.zip %s", tok_of(R));
	H(&c, true, cmd);
	CHECK(c.state == MC_WAITING && !strcmp(f.watching, "C0:11:22:33:44:55"));
	/* the new target has to qualify on its own samples */
	start_with_token(&c);
	CHECK(strstr(R, "refused") && f.start_calls == 0);

	/* ---- DISARM ------------------------------------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	H(&c, true, "disarm");
	CHECK(!strcmp(R, "IDLE") && c.state == MC_IDLE && f.locked[0] == '\0' && f.watching[0] == '\0');
	mc_ctl_on_adv(&c, -50, false);                              /* late samples are ignored */
	CHECK(c.state == MC_IDLE);

	/* ---- samples lost: the gate starts over, never trusts a gap in its window ------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	mc_ctl_samples_lost(&c);
	CHECK(c.state == MC_TARGET_DETECTED);
	/* the mc-lost report path while idle is harmless */
	ctl_setup(&c, &f);
	mc_ctl_samples_lost(&c);
	CHECK(c.state == MC_IDLE);

	/* ---- DFU-mode target: A itself is the DFU address, no +1 allowed ------------------------- */
	ctl_setup(&c, &f);
	H(&c, true, "status");
	snprintf(cmd, sizeof(cmd), ARM_NOTOK " %s", tok_of(R));
	H(&c, true, cmd);
	for (uint32_t t = 0; t <= 6000; t += 500) { mc_ctl_on_adv(&c, -60, true); f.now += 500; }
	start_with_token(&c);
	CHECK(strstr(R, "STARTED") && f.start_plus1 == false);

	/* ---- nothing ever starts a DFU on its own ---------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	for (int i = 0; i < 100; i++) { f.now += 1000; mc_ctl_tick(&c); mc_ctl_on_adv(&c, -55, false); }
	CHECK(f.start_calls == 0 || c.state == MC_IDLE);
	CHECK(f.start_calls == 0);

	/* ---- state transitions are all logged ------------------------------------------------- */
	ctl_setup(&c, &f);
	run_to_active(&c, &f);
	dfu_ev(&c, DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK);
	CHECK(has_note(&f, "IDLE -> WAITING") && has_note(&f, "WAITING -> TARGET_DETECTED") &&
	      has_note(&f, "ARMED -> START_AUTHORIZED") && has_note(&f, "START_AUTHORIZED -> DFU_ACTIVE") &&
	      has_note(&f, "DFU_ACTIVE -> IDLE"));

	/* ---- replies always fit one LoRa text --------------------------------------------------- */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	H(&c, true, "status");
	CHECK(strlen(R) <= MC_REPLY_MAX);
}

static void test_lab_features(void)
{
	struct mc_ctl c;
	struct fake f;
	struct mc_cmd cmd;
	SECTION("lab diagnostics, state log format, dry run");

	/* new read-only commands parse, are not privileged, take no arguments */
	const char *names[] = { "diag", "radio", "ble", "storage" };
	for (int i = 0; i < 4; i++) {
		CHECK(mc_cmd_parse(names[i], &cmd) == MC_PARSE_OK);
		CHECK(!mc_cmd_privileged(cmd.id) && !mc_cmd_needs_token(cmd.id));
		char t[16]; snprintf(t, sizeof(t), "%s x", names[i]);
		CHECK(mc_cmd_parse(t, &cmd) == MC_PARSE_USAGE);
	}
	/* a build without them says so, and nothing else happens */
	ctl_setup(&c, &f);
	for (int i = 0; i < 4; i++) {
		H(&c, false, names[i]);
		CHECK(strstr(R, "lab build only") && c.state == MC_IDLE);
	}
	/* with the ops present, any sender may ask, state is untouched */
	ctl_setup(&c, &f);
	mc_ctl_init(&c, &FOPS_DIAG, &f, &CFG);
	H(&c, false, "diag"); CHECK(!strcmp(R, "diag-diag"));
	H(&c, false, "radio"); CHECK(!strcmp(R, "diag-radio"));
	H(&c, false, "ble"); CHECK(!strcmp(R, "diag-ble"));
	H(&c, false, "storage"); CHECK(!strcmp(R, "diag-storage"));
	CHECK(f.diag_calls == 4 && c.state == MC_IDLE && f.start_calls == 0);

	/* STATE old -> new : reason, for every transition on the happy path */
	ctl_setup(&c, &f);
	get_armed(&c, &f);
	start_tok(&c);
	dfu_ev(&c, DFU_STATUS_DONE, 100, DFU_STATUS_RESULT_OK);
	const char *want[] = {
		"STATE IDLE -> WAITING : ARM accepted",
		"STATE WAITING -> TARGET_DETECTED : MAC matched",
		"STATE TARGET_DETECTED -> RSSI_QUALIFIED : hold complete",
		"STATE RSSI_QUALIFIED -> ARMED : bundle digest verified",
		"STATE ARMED -> START_AUTHORIZED : valid operator token",
		"STATE START_AUTHORIZED -> DFU_ACTIVE : permit consumed",
		"STATE DFU_ACTIVE -> IDLE : success",
	};
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		CHECK(has_note(&f, want[i]));
	}
	/* every state change carries a reason: no bare "A -> B" lines */
	for (int i = 0; i < f.nnotes; i++) {
		if (strstr(f.notes[i], "STATE ")) CHECK(strstr(f.notes[i], " : "));
	}

	/* dry run: all checks run, START reports PASS, nothing is started */
	ctl_setup(&c, &f);
	f.dry = true;
	get_armed(&c, &f);
	start_tok(&c);
	CHECK(strstr(R, "DRY RUN PASS FW ") && strstr(R, ADDR));
	CHECK(c.state == MC_IDLE && f.az.state == MC_AZ_IDLE && f.locked[0] == '\0');
	CHECK(f.start_calls == 1 && f.runner_consumed);        /* the gate path was exercised */
	CHECK(has_note(&f, "START_AUTHORIZED -> IDLE : dry run complete"));
	H(&c, true, "status"); CHECK(strstr(R, "last=dry run pass"));
	/* the dry run still obeys every gate: unarmed / unqualified / bad token refuse */
	ctl_setup(&c, &f);
	f.dry = true;
	start_tok(&c); CHECK(strstr(R, "not armed") && f.start_calls == 0);
	H(&c, true, "status");
	{ char cmdl[96]; snprintf(cmdl, sizeof(cmdl), ARM_NOTOK " %s", tok_of(R)); H(&c, true, cmdl); }
	adv_run(&c, &f, 2000, 500, -60);
	start_tok(&c); CHECK(strstr(R, "refused") && f.start_calls == 0);
	strcpy(f.content, "other");
	adv_run(&c, &f, 5000, 500, -60);
	CHECK(c.state == MC_IDLE);                                  /* digest mismatch cancelled it */
	/* and it is repeatable: arm again, pass again */
	strcpy(f.content, "firmware v1");
	get_armed(&c, &f);
	start_tok(&c); CHECK(strstr(R, "DRY RUN PASS"));
}

void run_ctl_tests(void)
{
	test_lab_features();
	test_layout();
	test_pathguard();
	test_gate();
	test_authz();
	test_target();
	test_progress();
	test_replay();
	test_ctl();
}
