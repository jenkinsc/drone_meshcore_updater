#include "mc_radio.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "mc_sx126x.h"

LOG_MODULE_DECLARE(mc);

/* Pins come from the overlay's zephyr,user node (boards/mc_wio_sx1262.overlay),
 * so the application never names a pin number. */
#define ZU DT_PATH(zephyr_user)

static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
static const struct gpio_dt_spec g_reset = GPIO_DT_SPEC_GET(ZU, mc_reset_gpios);
static const struct gpio_dt_spec g_busy  = GPIO_DT_SPEC_GET(ZU, mc_busy_gpios);
static const struct gpio_dt_spec g_dio1  = GPIO_DT_SPEC_GET(ZU, mc_dio1_gpios);
static const struct gpio_dt_spec g_rxen  = GPIO_DT_SPEC_GET(ZU, mc_rxen_gpios);
static const struct gpio_dt_spec g_nss   = GPIO_DT_SPEC_GET(ZU, mc_nss_gpios);

static struct spi_config spi_cfg = {
	.frequency = 4000000,
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

/* Opcodes (datasheet table 11-1). */
#define OP_SET_STANDBY       0x80
#define OP_SET_PKT_TYPE      0x8A
#define OP_SET_RF_FREQ       0x86
#define OP_SET_PA_CFG        0x95
#define OP_SET_TX_PARAMS     0x8E
#define OP_SET_BUF_BASE      0x8F
#define OP_SET_MOD_PARAMS    0x8B
#define OP_SET_PKT_PARAMS    0x8C
#define OP_SET_DIO_IRQ       0x08
#define OP_CLR_IRQ           0x02
#define OP_GET_IRQ           0x12
#define OP_SET_DIO3_TCXO     0x97
#define OP_SET_DIO2_RFSW     0x9D
#define OP_SET_REGULATOR     0x96
#define OP_CALIBRATE         0x89
#define OP_CALIBRATE_IMAGE   0x98
#define OP_WRITE_REG         0x0D
#define OP_READ_REG          0x1D
#define OP_WRITE_BUF         0x0E
#define OP_READ_BUF          0x1E
#define OP_SET_TX            0x83
#define OP_SET_RX            0x82
#define OP_GET_RX_BUF_STATUS 0x13
#define OP_GET_PKT_STATUS    0x14
#define OP_GET_RSSI_INST     0x15
#define OP_CLR_DEV_ERRORS    0x07

#define IRQ_TX_DONE   0x0001
#define IRQ_RX_DONE   0x0002
#define IRQ_HDR_ERR   0x0020
#define IRQ_CRC_ERR   0x0040
#define IRQ_TIMEOUT   0x0200

#define REG_SYNC_WORD 0x0740
#define REG_RX_GAIN   0x08AC
#define REG_OCP       0x08E7

static struct mc_radio_stats st;
static struct gpio_callback dio1_cb;
static void (*notify)(void);

static int wait_busy(uint32_t ms)
{
	int64_t end = k_uptime_get() + ms;

	int64_t spin_end = k_uptime_get() + 2;

	/* BUSY is normally high for microseconds; the long ones (calibration,
	 * a regulator or oscillator start) are milliseconds. Spin briefly, then
	 * sleep, so a slow BUSY never holds the CPU against the BLE threads. */
	while (gpio_pin_get_dt(&g_busy) > 0) {
		if (k_uptime_get() > end) {
			st.busy_timeouts++;
			return -ETIMEDOUT;
		}
		if (k_uptime_get() < spin_end) {
			k_busy_wait(50);
		} else {
			k_msleep(1);
		}
	}
	return 0;
}

/* One SPI transaction with NSS held low across it. tx and rx are the same
 * length; rx may be NULL. */
static int xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
	int rc = wait_busy(100);

	if (rc) {
		LOG_ERR("BUSY stuck high");
		return rc;
	}
	struct spi_buf txb = { .buf = (void *)tx, .len = n };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf rxb = { .buf = rx, .len = n };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

	gpio_pin_set_dt(&g_nss, 1);
	rc = spi_transceive(spi_dev, &spi_cfg, &txs, rx ? &rxs : NULL);
	gpio_pin_set_dt(&g_nss, 0);
	if (rc) {
		st.spi_faults++;
	}
	return rc;
}

static int cmd_w(uint8_t op, const uint8_t *p, size_t n)
{
	uint8_t b[16];

	if (n + 1 > sizeof(b)) {
		return -EINVAL;
	}
	b[0] = op;
	memcpy(&b[1], p, n);
	return xfer(b, NULL, n + 1);
}

/* Read command: opcode, one status byte, then `n` data bytes. */
static int cmd_r(uint8_t op, uint8_t *out, size_t n)
{
	uint8_t tx[8] = { op }, rx[8];

	if (n + 2 > sizeof(rx)) {
		return -EINVAL;
	}
	int rc = xfer(tx, rx, n + 2);

	if (rc == 0) {
		memcpy(out, &rx[2], n);
	}
	return rc;
}

static int reg_w(uint16_t addr, const uint8_t *p, size_t n)
{
	uint8_t b[8];

	if (n + 3 > sizeof(b)) {
		return -EINVAL;
	}
	b[0] = OP_WRITE_REG; b[1] = addr >> 8; b[2] = (uint8_t)addr;
	memcpy(&b[3], p, n);
	return xfer(b, NULL, n + 3);
}

static int reg_r1(uint16_t addr, uint8_t *v)
{
	uint8_t tx[5] = { OP_READ_REG, addr >> 8, (uint8_t)addr, 0, 0 }, rx[5];
	int rc = xfer(tx, rx, sizeof(tx));

	if (rc == 0) {
		*v = rx[4];
	}
	return rc;
}

static int get_irq(uint16_t *flags)
{
	uint8_t d[2];
	int rc = cmd_r(OP_GET_IRQ, d, 2);

	if (rc == 0) {
		*flags = (uint16_t)((d[0] << 8) | d[1]);
	}
	return rc;
}

static int clear_irq(uint16_t flags)
{
	uint8_t p[2] = { flags >> 8, (uint8_t)flags };

	return cmd_w(OP_CLR_IRQ, p, 2);
}

static int set_rx(void)
{
	static const uint8_t cont[3] = { 0xFF, 0xFF, 0xFF };   /* continuous */

	gpio_pin_set_dt(&g_rxen, 1);
	return cmd_w(OP_SET_RX, cont, 3);
}

static void dio1_isr(const struct device *d, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(d); ARG_UNUSED(cb); ARG_UNUSED(pins);
	st.dio1_irqs++;
	if (notify) {
		notify();
	}
}

/* One logging call site for every init step, not one per step: the step is
 * identified by its source line, which costs far less flash than a message
 * each (this image has little to spare). */
static int ck(int rc, int line)
{
	if (rc) {
		LOG_ERR("radio init L%d rc=%d", line, rc);
	}
	return rc;
}

#define CK(x) do { int _r = ck((x), __LINE__); if (_r) { return _r; } } while (0)

int mc_radio_init(void (*irq_notify)(void))
{
	notify = irq_notify;

	if (!device_is_ready(spi_dev) || !gpio_is_ready_dt(&g_reset) ||
	    !gpio_is_ready_dt(&g_busy) || !gpio_is_ready_dt(&g_dio1) ||
	    !gpio_is_ready_dt(&g_rxen) || !gpio_is_ready_dt(&g_nss)) {
		LOG_ERR("radio: SPI/GPIO not ready");
		return -ENODEV;
	}
	CK(gpio_pin_configure_dt(&g_nss, GPIO_OUTPUT_INACTIVE));
	CK(gpio_pin_configure_dt(&g_reset, GPIO_OUTPUT_INACTIVE));
	CK(gpio_pin_configure_dt(&g_rxen, GPIO_OUTPUT_INACTIVE));
	CK(gpio_pin_configure_dt(&g_busy, GPIO_INPUT));
	CK(gpio_pin_configure_dt(&g_dio1, GPIO_INPUT));

	/* Hardware reset. */
	gpio_pin_set_dt(&g_reset, 1);
	k_msleep(2);
	gpio_pin_set_dt(&g_reset, 0);
	k_msleep(10);
	CK(wait_busy(200));

	const uint32_t hz = CONFIG_MC_FREQ_HZ;
	uint8_t p[8], mod[4], pkt[6], img[2], sw[2];

	if (!mc_sx_modparams(CONFIG_MC_SF, CONFIG_MC_BW_HZ, CONFIG_MC_CR, mod)) {
		LOG_ERR("radio: unsupported SF/BW/CR");
		return -EINVAL;
	}

	static const struct { uint8_t op, n; uint8_t d[4]; } PRE[] = {
		{ OP_SET_STANDBY,    1, { 0x00 } },                          /* STDBY_RC */
		/* TCXO powered from DIO3: 1.8 V, 5 ms start-up (320 * 15.625 us).
		 * Must precede calibration. */
		{ OP_SET_DIO3_TCXO,  4, { 0x02, 0x00, 0x01, 0x40 } },
		{ OP_CLR_DEV_ERRORS, 2, { 0x00, 0x00 } },
		{ OP_CALIBRATE,      1, { 0x7F } },
	};
	static const struct { uint8_t op, n; uint8_t d[4]; } POST[] = {
		{ OP_SET_STANDBY,    1, { 0x00 } },
		{ OP_SET_REGULATOR,  1, { 0x01 } },                          /* DC-DC */
		{ OP_SET_DIO2_RFSW,  1, { 0x01 } },                          /* DIO2 drives TX switch */
		{ OP_SET_PKT_TYPE,   1, { 0x01 } },                          /* LoRa */
	};

	for (size_t i = 0; i < ARRAY_SIZE(PRE); i++) {
		CK(cmd_w(PRE[i].op, PRE[i].d, PRE[i].n));
	}
	k_msleep(5);
	CK(wait_busy(500));
	for (size_t i = 0; i < ARRAY_SIZE(POST); i++) {
		CK(cmd_w(POST[i].op, POST[i].d, POST[i].n));
	}

	uint32_t fw = mc_sx_freq_word(hz);

	mc_sx_image_cal(hz, img);
	CK(cmd_w(OP_CALIBRATE_IMAGE, img, 2));
	p[0] = fw >> 24; p[1] = fw >> 16; p[2] = fw >> 8; p[3] = (uint8_t)fw;
	CK(cmd_w(OP_SET_RF_FREQ, p, 4));

	/* SX1262 high-power PA: dutyCycle 4, hpMax 7, device 0, paLut 1. */
	p[0] = 0x04; p[1] = 0x07; p[2] = 0x00; p[3] = 0x01;
	CK(cmd_w(OP_SET_PA_CFG, p, 4));
	p[0] = (uint8_t)CONFIG_MC_TX_POWER_DBM; p[1] = 0x04;          /* 200 us ramp */
	CK(cmd_w(OP_SET_TX_PARAMS, p, 2));
	uint8_t ocp = (uint8_t)((CONFIG_MC_OCP_MA * 2) / 5);          /* 2.5 mA steps */

	CK(reg_w(REG_OCP, &ocp, 1));

	p[0] = 0; p[1] = 0; CK(cmd_w(OP_SET_BUF_BASE, p, 2));
	CK(cmd_w(OP_SET_MOD_PARAMS, mod, 4));
	mc_sx_pktparams(CONFIG_MC_PREAMBLE, 255, pkt);
	CK(cmd_w(OP_SET_PKT_PARAMS, pkt, 6));
	mc_sx_syncword_regs(CONFIG_MC_SYNC_WORD, sw);
	CK(reg_w(REG_SYNC_WORD, sw, 2));
	uint8_t gain = 0x96;                                          /* RX boosted */

	CK(reg_w(REG_RX_GAIN, &gain, 1));

	/* DIO1 for receive events only; transmit completion is polled. */
	uint16_t mask = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HDR_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT;
	uint16_t dio1 = IRQ_RX_DONE | IRQ_HDR_ERR | IRQ_CRC_ERR;

	p[0] = mask >> 8; p[1] = (uint8_t)mask; p[2] = dio1 >> 8; p[3] = (uint8_t)dio1;
	p[4] = p[5] = p[6] = p[7] = 0;
	CK(cmd_w(OP_SET_DIO_IRQ, p, 8));
	CK(clear_irq(0xFFFF));

	/* Read-back sanity: the sync word we wrote is what the chip holds. */
	uint8_t chk = 0;

	CK(reg_r1(REG_SYNC_WORD, &chk));
	if (chk != sw[0]) {
		LOG_ERR("radio: sync word read-back %02x != %02x — SPI wiring?", chk, sw[0]);
		return -EIO;
	}

	gpio_init_callback(&dio1_cb, dio1_isr, BIT(g_dio1.pin));
	CK(gpio_add_callback(g_dio1.port, &dio1_cb));
	CK(gpio_pin_interrupt_configure_dt(&g_dio1, GPIO_INT_EDGE_TO_ACTIVE));

	CK(set_rx());
	st.up = true;
	LOG_INF("radio up: %u Hz SF%u BW%u CR4/%u pre%u sync 0x%02x %d dBm",
		hz, CONFIG_MC_SF, CONFIG_MC_BW_HZ, CONFIG_MC_CR, CONFIG_MC_PREAMBLE,
		CONFIG_MC_SYNC_WORD, CONFIG_MC_TX_POWER_DBM);
	return 0;
}

int mc_radio_service(struct mc_radio_rx *rx)
{
	uint16_t irq = 0;

	if (get_irq(&irq) != 0 || irq == 0) {
		return 0;
	}
	clear_irq(irq);
	if (irq & IRQ_CRC_ERR) { st.rx_crc_err++; }
	if (irq & IRQ_HDR_ERR) { st.rx_hdr_err++; }
	if (!(irq & IRQ_RX_DONE) || (irq & (IRQ_CRC_ERR | IRQ_HDR_ERR))) {
		if (!(irq & (IRQ_CRC_ERR | IRQ_HDR_ERR | IRQ_RX_DONE))) { st.rx_other++; }
		return 0;
	}

	uint8_t bs[2], ps[3];

	if (cmd_r(OP_GET_RX_BUF_STATUS, bs, 2) != 0 || bs[0] == 0) {
		return 0;
	}
	if (cmd_r(OP_GET_PKT_STATUS, ps, 3) != 0) {
		return 0;
	}
	uint8_t tx[3 + 255] = { OP_READ_BUF, bs[1], 0 }, rb[3 + 255];

	if (xfer(tx, rb, 3u + bs[0]) != 0) {
		return 0;
	}
	memcpy(rx->data, &rb[3], bs[0]);
	rx->len = bs[0];
	rx->rssi_dbm = mc_sx_rssi_dbm(ps[0]);
	rx->snr_q2 = mc_sx_snr_q2(ps[1]);
	st.rx_ok++;
	st.have_rx = true;
	st.last_rssi_dbm = rx->rssi_dbm;
	st.last_snr_q2 = rx->snr_q2;
	return 1;
}

/* Channel check: instantaneous RSSI while in receive. Bounded so a noisy band
 * delays a reply but never withholds it. */
static void wait_clear(void)
{
	for (int i = 0; i < CONFIG_MC_LBT_TRIES; i++) {
		uint8_t r;

		k_msleep(3);
		if (cmd_r(OP_GET_RSSI_INST, &r, 1) != 0) {
			return;
		}
		if (mc_sx_rssi_dbm(r) < CONFIG_MC_LBT_RSSI_DBM) {
			return;
		}
		k_msleep(50 + (sys_rand32_get() % 250));
	}
}

int mc_radio_tx(const uint8_t *frame, size_t len)
{
	if (len == 0 || len > 255) {
		return -EINVAL;
	}
	wait_clear();

	uint8_t p[8], pkt[6], std = 0x00;
	int rc;

	gpio_pin_set_dt(&g_rxen, 0);
	rc = cmd_w(OP_SET_STANDBY, &std, 1);
	if (rc) { goto out; }
	mc_sx_pktparams(CONFIG_MC_PREAMBLE, (uint8_t)len, pkt);
	rc = cmd_w(OP_SET_PKT_PARAMS, pkt, 6);
	if (rc) { goto out; }

	uint8_t wb[2 + 255] = { OP_WRITE_BUF, 0 };

	memcpy(&wb[2], frame, len);
	rc = xfer(wb, NULL, 2 + len);
	if (rc) { goto out; }
	clear_irq(0xFFFF);

	p[0] = p[1] = p[2] = 0;                                       /* no chip timeout */
	rc = cmd_w(OP_SET_TX, p, 3);
	if (rc) { goto out; }

	int64_t end = k_uptime_get() + 8000; /* >= SF12 airtime; exits on TX_DONE */
	uint16_t irq = 0;

	while (k_uptime_get() < end) {
		k_msleep(5);
		if (get_irq(&irq) == 0 && (irq & IRQ_TX_DONE)) {
			break;
		}
	}
	if (!(irq & IRQ_TX_DONE)) {
		LOG_ERR("TX did not complete");
		st.tx_timeout++;
		rc = -ETIMEDOUT;
	}
out:
	clear_irq(0xFFFF);
	/* Back to the receive profile: packet length is irrelevant in explicit
	 * header mode, but restore the maximum as init did. */
	mc_sx_pktparams(CONFIG_MC_PREAMBLE, 255, pkt);
	cmd_w(OP_SET_PKT_PARAMS, pkt, 6);
	if (set_rx() != 0) {
		LOG_ERR("could not return to RX after TX");
		st.rx_restore_fail++;
		if (rc == 0) {
			rc = -EIO;
		}
	}
	if (rc) {
		st.tx_fail++;
	} else {
		st.tx_ok++;
	}
	return rc;
}

const struct mc_radio_stats *mc_radio_stats_get(void) { return &st; }

void mc_radio_pins(int *busy, int *dio1, int *reset)
{
	*busy = gpio_pin_get_dt(&g_busy);
	*dio1 = gpio_pin_get_dt(&g_dio1);
	*reset = gpio_pin_get_dt(&g_reset);
}

bool mc_radio_hw_ready(void)
{
	return device_is_ready(spi_dev) && gpio_is_ready_dt(&g_reset) &&
	       gpio_is_ready_dt(&g_busy) && gpio_is_ready_dt(&g_dio1) &&
	       gpio_is_ready_dt(&g_rxen) && gpio_is_ready_dt(&g_nss);
}
