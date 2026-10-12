#include "mc_cmd.h"

#include <string.h>

#define MAX_WORDS 5
#define WORD_MAX  64

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool ieq(const char *a, const char *b)
{
	while (*a && *b) {
		if (lower(*a++) != lower(*b++)) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static bool hex_digit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

bool mc_addr_normalise(const char *s, char *out)
{
	if (strlen(s) != MC_ADDR_STR_LEN) {
		return false;
	}
	for (size_t i = 0; i < MC_ADDR_STR_LEN; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':') {
				return false;
			}
			out[i] = ':';
		} else {
			if (!hex_digit(s[i])) {
				return false;
			}
			out[i] = (s[i] >= 'a' && s[i] <= 'f') ? (char)(s[i] - 32) : s[i];
		}
	}
	out[MC_ADDR_STR_LEN] = '\0';
	return true;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool mc_addr_to_le(const char *s, uint8_t le[6])
{
	char norm[MC_ADDR_STR_LEN + 1];

	if (!mc_addr_normalise(s, norm)) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		int hi = hexv(norm[i * 3]), lo = hexv(norm[i * 3 + 1]);
		le[5 - i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

bool mc_addr_equal(const uint8_t armed_le[6], char armed_type,
		   const uint8_t seen_le[6], uint8_t seen_type)
{
	uint8_t want = (armed_type == 'p') ? 0 : 1;
	uint8_t d = (uint8_t)(want ^ seen_type);

	for (int i = 0; i < 6; i++) {
		d |= (uint8_t)(armed_le[i] ^ seen_le[i]);
	}
	return d == 0;
}

bool mc_bundle_name_ok(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > MC_BUNDLE_MAX || s[0] == '.') {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	/* "a..b" is a legal file name and harmless without a '/', but refusing
	 * it costs nothing and keeps the "no .." promise literal. */
	return strstr(s, "..") == NULL;
}

static bool parse_page(const char *w, unsigned *out)
{
	unsigned v = 0;

	if (*w == '\0') {
		return false;
	}
	for (; *w; w++) {
		if (*w < '0' || *w > '9') {
			return false;
		}
		v = v * 10 + (unsigned)(*w - '0');
		if (v > 99) {
			return false;
		}
	}
	if (v == 0) {
		return false;
	}
	*out = v;
	return true;
}

bool mc_cmd_privileged(enum mc_cmd_id id)
{
	switch (id) {
	case MC_CMD_ARM:
	case MC_CMD_DISARM:
	case MC_CMD_START:
	case MC_CMD_ABORT:
		return true;
	default:
		return false;
	}
}

static const char *const NAMES[MC_CMD_COUNT] = {
	[MC_CMD_STATUS]  = "status",
	[MC_CMD_SCAN]    = "scan",
	[MC_CMD_RSSI]    = "rssi",
	[MC_CMD_ARM]     = "arm",
	[MC_CMD_DISARM]  = "disarm",
	[MC_CMD_START]   = "start",
	[MC_CMD_ABORT]   = "abort",
	[MC_CMD_VERSION] = "version",
	[MC_CMD_BATTERY] = "battery",
	[MC_CMD_LOG]     = "log",
};

const char *mc_cmd_name(enum mc_cmd_id id)
{
	return (id >= 0 && id < MC_CMD_COUNT) ? NAMES[id] : "?";
}

enum mc_parse_err mc_cmd_parse(const char *text, struct mc_cmd *out)
{
	char words[MAX_WORDS][WORD_MAX];
	size_t nw = 0;
	const char *p = text;

	memset(out, 0, sizeof(*out));
	out->page = 1;

	while (*p) {
		while (*p && is_space(*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		if (nw == MAX_WORDS) {
			return MC_PARSE_USAGE;
		}
		size_t n = 0;
		while (*p && !is_space(*p)) {
			if (n + 1 >= WORD_MAX) {
				return MC_PARSE_USAGE;
			}
			words[nw][n++] = *p++;
		}
		words[nw][n] = '\0';
		nw++;
	}

	if (nw == 0) {
		return MC_PARSE_EMPTY;
	}

	int id = -1;
	for (int i = 0; i < MC_CMD_COUNT; i++) {
		if (ieq(words[0], NAMES[i])) {
			id = i;
			break;
		}
	}
	if (id < 0) {
		return MC_PARSE_UNKNOWN;
	}
	out->id = (enum mc_cmd_id)id;

	switch (out->id) {
	case MC_CMD_SCAN:
	case MC_CMD_LOG:
		if (nw == 2) {
			return parse_page(words[1], &out->page) ? MC_PARSE_OK
								: MC_PARSE_USAGE;
		}
		return nw == 1 ? MC_PARSE_OK : MC_PARSE_USAGE;

	case MC_CMD_ARM:
		if (nw < 3 || nw > 4) {
			return MC_PARSE_USAGE;
		}
		if (!mc_addr_normalise(words[1], out->addr)) {
			return MC_PARSE_BAD_ADDR;
		}
		if (!mc_bundle_name_ok(words[2])) {
			return MC_PARSE_BAD_BUNDLE;
		}
		strcpy(out->bundle, words[2]);
		out->addr_type = 'r';
		if (nw == 4) {
			if (ieq(words[3], "public")) {
				out->addr_type = 'p';
			} else if (!ieq(words[3], "random")) {
				return MC_PARSE_USAGE;
			}
		}
		return MC_PARSE_OK;

	default:
		return nw == 1 ? MC_PARSE_OK : MC_PARSE_USAGE;
	}
}
