#pragma once
/*
 * Lab-only bring-up support (CONFIG_MC_LAB): first-boot diagnostic report and
 * the read-only `diag` / `radio` / `ble` / `storage` replies. Nothing here
 * changes controller or radio behaviour; it only reads state and logs. Never
 * prints a private key, the identity page contents, or the operator secret.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/kernel.h>

#include "mc_ctl.h"
#include "mc_id.h"
#include "mc_store.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mc_lab_idstat { MC_LAB_ID_NONE, MC_LAB_ID_LOADED, MC_LAB_ID_GENERATED, MC_LAB_ID_REGENERATED };

struct mc_lab_env {
	struct mc_ctl         *ctl;
	struct mc_store       *store;
	const struct mc_id    *id;
	const struct mc_flash *flash;
	struct k_thread       *thread;
	struct k_msgq         *queue;
	const char            *bundle_dir;
	uint32_t               id_start, id_end;      /* flash offsets, end exclusive */
	enum mc_lab_idstat     id_stat;
	bool                   op_key_ok;
	uint8_t                op_pub[32];
	uint32_t               reset_cause;
	const volatile long   *lost;                  /* adv samples dropped (atomic) */
};

void mc_lab_bind(const struct mc_lab_env *env);

/* Log the full first-boot report; ends with AIRDFU LAB READY or
 * AIRDFU INIT FAILED: <reason>. `radio_rc` is mc_radio_init()'s result. */
void mc_lab_boot_report(int radio_rc);

/* Log a hard failure that stops bring-up before the report can run. */
void mc_lab_init_failed(const char *reason, int rc);

/* Every advertisement, from the BLE thread: tracks the scan rate, the last
 * DFU-service advertiser and its address relation to the watched address.
 * `watch_le` may be NULL when nothing is armed. */
void mc_lab_adv(const bt_addr_le_t *a, int8_t rssi, bool dfu_uuid, const char *name,
		const uint8_t *watch_le);

/* mc_ops.diag: cmd_id is MC_CMD_DIAG / RADIO / BLE / STORAGE. */
size_t mc_lab_diag(void *ctx, int cmd_id, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
