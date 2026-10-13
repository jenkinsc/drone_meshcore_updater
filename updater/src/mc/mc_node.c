#include "mc_node.h"

#include <stdio.h>
#include <string.h>

static uint32_t clock_now(const struct mc_node *n)
{
	uint32_t el = n->ops->now_ms(n->ctx) - n->clock_at_ms;

	return n->clock_ts + el / 1000u;
}

static void clock_set_floor(struct mc_node *n, uint32_t ts)
{
	/* Only ever move forward: a stale or wrong timestamp must not drag the
	 * clock back under adverts we already sent. */
	if (ts > clock_now(n)) {
		n->clock_ts = ts;
		n->clock_at_ms = n->ops->now_ms(n->ctx);
	}
}

void mc_node_init(struct mc_node *n, const struct mc_crypto *cr, struct mc_id *id,
		  struct mc_ctl *ctl, const struct mc_node_ops *ops, void *ctx,
		  const struct mc_persist *persisted)
{
	memset(n, 0, sizeof(*n));
	n->cr = cr;
	n->id = id;
	n->ctl = ctl;
	n->ops = ops;
	n->ctx = ctx;
	n->saved = *persisted;
	mc_replay_init(&n->replay);
	n->clock_at_ms = ops->now_ms(ctx);
	/* Adverts must be newer than any sent before a reboot. The persisted value
	 * is a high-water mark written at most once per MC_ADV_PERSIST_S of uptime,
	 * so after a reboot this can be up to that long *behind* what was sent:
	 * contacts then ignore adverts until the clock passes it. Harmless (the
	 * operator is already a contact) and it keeps flash writes rare. */
	n->clock_ts = persisted->adv_ts;
	n->last_adv_ts = persisted->adv_ts;   /* the stored value was already sent */
}

static void send_txt(struct mc_node *n, uint8_t txt_type, const char *text)
{
	uint8_t frame[MC_MAX_RAW];
	size_t len = mc_wire_build_txt(n->cr, n->id->secret, n->id->op_pub[0],
				       n->id->pub[0], clock_now(n), txt_type, text,
				       frame, sizeof(frame));

	if (len) {
		n->ops->tx(n->ctx, frame, len);
	}
}

void mc_node_send_text(struct mc_node *n, const char *text)
{
	if (n->id->have_operator) {
		send_txt(n, MC_TXT_PLAIN, text);
	}
}

void mc_node_advertise(struct mc_node *n, const char *name)
{
	uint8_t frame[MC_MAX_RAW];
	uint32_t ts = clock_now(n);

	/* Strictly newer than the previous advert in this boot. */
	if (ts <= n->last_adv_ts) {
		ts = n->last_adv_ts + 1;
		clock_set_floor(n, ts);
	}
	size_t len = mc_wire_build_advert(n->id->pub, n->id->prv, ts, name, frame,
					  sizeof(frame));
	if (len == 0) {
		return;
	}
	n->ops->tx(n->ctx, frame, len);
	n->last_adv_ts = ts;
	/* Persist rarely: the first advert ever, then once a day of uptime. */
	if (n->ops->persist &&
	    (n->saved.adv_ts == 0 ||
	     (int32_t)(ts - n->saved.adv_ts) >= (int32_t)MC_ADV_PERSIST_S)) {
		n->saved.adv_ts = ts;
		n->ops->persist(n->ctx, &n->saved);
	}
}

void mc_node_rx(struct mc_node *n, const uint8_t *raw, size_t len)
{
	struct mc_pkt pkt;
	uint8_t plain[MC_MAX_PACKET_PAYLOAD];
	struct mc_txt txt;

	n->rx_frames++;

	if (!n->id->have_operator || mc_wire_parse(raw, len, &pkt) != 0) {
		n->rx_bad++;
		return;
	}
	if (pkt.ptype != MC_PT_TXT_MSG || pkt.pver != 0) {
		return;                                   /* not for us; not an error */
	}
	if (pkt.payload_len < 2 || pkt.payload[0] != n->id->pub[0]) {
		return;                                   /* addressed to someone else */
	}
	if (pkt.payload[1] != n->id->op_pub[0]) {
		n->rx_bad++;                              /* someone else's traffic */
		return;
	}

	int pl = mc_wire_open(n->cr, n->id->secret, pkt.payload, pkt.payload_len,
			      plain, sizeof(plain));
	if (pl < 0) {
		/* Right hash byte, wrong MAC: a forgery attempt or a 1-in-256
		 * hash collision with some other node. Silent either way. */
		n->rx_bad++;
		return;
	}
	if (mc_wire_parse_txt(plain, (size_t)pl, &txt) != 0 ||
	    (txt.txt_type != MC_TXT_PLAIN && txt.txt_type != MC_TXT_CLI)) {
		n->rx_bad++;
		return;
	}

	/* Digest covers everything but the attempt counter, so a sender's retry
	 * of the same message is recognised as the same message. */
	uint8_t dg[4 + 1 + MC_TEXT_MAX];
	size_t dl = 0;
	memcpy(dg, plain, 4);
	dl = 4;
	dg[dl++] = txt.txt_type;
	memcpy(&dg[dl], txt.text, txt.text_len);
	dl += txt.text_len;
	uint32_t digest = mc_replay_digest(dg, (unsigned)dl);

	enum mc_replay_verdict rv = mc_replay_accept(&n->replay, digest);

	/* Acknowledge anything that authenticated, including a repeat: a sender
	 * whose first ACK was lost keeps retrying until it hears one. A repeat is
	 * acknowledged but not acted on. */
	if (txt.txt_type == MC_TXT_PLAIN && mc_wire_is_flood(&pkt)) {
		uint8_t ack[6];
		uint8_t frame[MC_MAX_RAW];

		mc_wire_ack_bytes(n->cr, plain, (size_t)pl, txt.text_len, n->id->op_pub,
				  n->ops->random_byte(n->ctx), ack);
		size_t fl = mc_wire_build_path_ack(n->cr, n->id->secret, n->id->op_pub[0],
						   n->id->pub[0], &pkt, ack, frame,
						   sizeof(frame));
		if (fl) {
			n->ops->tx(n->ctx, frame, fl);
		}
	}

	if (rv == MC_REPLAY_DUPLICATE) {
		n->rx_replay++;
		return;
	}

	clock_set_floor(n, txt.ts);

	char reply[MC_REPLY_MAX + 1];
	mc_ctl_handle(n->ctl, true, txt.text, reply, sizeof(reply));
	send_txt(n, txt.txt_type, reply);
}
