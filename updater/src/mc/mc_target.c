#include "mc_target.h"

#include <string.h>

void mc_addr_plus_one(const uint8_t in[6], uint8_t out[6])
{
	memcpy(out, in, 6);
	for (int i = 0; i < 6; i++) {
		out[i]++;
		if (out[i] != 0) {
			break;
		}
	}
}

enum mc_target_verdict mc_target_check(const struct mc_target_cfg *cfg,
				       const uint8_t armed_le[6], char armed_type,
				       const uint8_t seen_le[6], uint8_t seen_type,
				       bool has_dfu_uuid, const char *name)
{
	uint8_t want_type = (armed_type == 'p') ? 0 : 1;
	uint8_t plus1[6];
	bool exact = memcmp(armed_le, seen_le, 6) == 0;
	bool p1;

	mc_addr_plus_one(armed_le, plus1);
	p1 = memcmp(plus1, seen_le, 6) == 0;

	if (!exact && !p1) {
		return MC_TGT_REJECT_ADDR;
	}
	if (seen_type != want_type) {
		return MC_TGT_REJECT_TYPE;
	}
	if (exact) {
		return MC_TGT_EXACT;
	}
	if (!cfg->allow_plus1) {
		return MC_TGT_REJECT_PLUS1_OFF;
	}
	if (cfg->require_dfu_uuid && !has_dfu_uuid) {
		return MC_TGT_REJECT_UUID;
	}
	if (cfg->dfu_name != NULL && cfg->dfu_name[0] != '\0' && name != NULL &&
	    name[0] != '\0' && strcmp(name, cfg->dfu_name) != 0) {
		return MC_TGT_REJECT_NAME;
	}
	return MC_TGT_DFU_PLUS1;
}

const char *mc_target_verdict_str(enum mc_target_verdict v)
{
	switch (v) {
	case MC_TGT_EXACT:            return "exact";
	case MC_TGT_DFU_PLUS1:        return "dfu+1";
	case MC_TGT_REJECT_ADDR:      return "wrong address";
	case MC_TGT_REJECT_TYPE:      return "wrong address type";
	case MC_TGT_REJECT_UUID:      return "no dfu service uuid";
	case MC_TGT_REJECT_NAME:      return "wrong name";
	case MC_TGT_REJECT_PLUS1_OFF: return "+1 not allowed";
	}
	return "?";
}
