#pragma once

/*
 * Small ring of recent events, for the `log` command.
 *
 * Deliberately not a Zephyr log backend. The `log` command exists so an
 * operator who cannot reach a console can see *what the command layer did*:
 * which commands arrived, which were refused and why, what the gate decided,
 * how a run ended. Those are a handful of lines the MeshCore modules write
 * themselves. Mirroring the whole system log would burn RAM and airtime on
 * littlefs and Bluetooth chatter, and the detailed DFU log is already served
 * over the GATT log stream for anyone within Bluetooth range.
 *
 * Not thread-safe by itself; the MeshCore thread is the only writer and
 * reader (see mc_zephyr.c).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_EVLOG_ENTRIES    24
#define MC_EVLOG_TEXT_MAX   44          /* characters kept per event */
#define MC_EVLOG_PAGE_LINES 3

struct mc_evlog_entry {
	uint32_t sec;
	char     text[MC_EVLOG_TEXT_MAX + 1];
};

struct mc_evlog {
	struct mc_evlog_entry e[MC_EVLOG_ENTRIES];
	unsigned head;     /* next slot to write */
	unsigned count;
};

void mc_evlog_init(struct mc_evlog *l);

/* Append one event, truncating to MC_EVLOG_TEXT_MAX. `sec` is uptime seconds. */
void mc_evlog_add(struct mc_evlog *l, uint32_t sec, const char *text);

/* Page 1 = the newest MC_EVLOG_PAGE_LINES events, newest first. Writes
 * "log p/N: t+12s text | t+9s text | ..." into `out` (at most `cap` bytes
 * including the NUL) and returns its length. A page past the end reports so. */
size_t mc_evlog_page(const struct mc_evlog *l, unsigned page, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
