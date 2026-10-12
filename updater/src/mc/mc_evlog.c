#include "mc_evlog.h"

#include <stdio.h>
#include <string.h>

void mc_evlog_init(struct mc_evlog *l)
{
	memset(l, 0, sizeof(*l));
}

void mc_evlog_add(struct mc_evlog *l, uint32_t sec, const char *text)
{
	struct mc_evlog_entry *e = &l->e[l->head];

	e->sec = sec;
	strncpy(e->text, text, MC_EVLOG_TEXT_MAX);
	e->text[MC_EVLOG_TEXT_MAX] = '\0';

	l->head = (l->head + 1) % MC_EVLOG_ENTRIES;
	if (l->count < MC_EVLOG_ENTRIES) {
		l->count++;
	}
}

size_t mc_evlog_page(const struct mc_evlog *l, unsigned page, char *out, size_t cap)
{
	unsigned pages = (l->count + MC_EVLOG_PAGE_LINES - 1) / MC_EVLOG_PAGE_LINES;
	int n;

	if (cap == 0) {
		return 0;
	}
	if (l->count == 0) {
		n = snprintf(out, cap, "log empty");
		return (size_t)n < cap ? (size_t)n : cap - 1;
	}
	if (page == 0) {
		page = 1;
	}
	if (page > pages) {
		n = snprintf(out, cap, "log: no page %u (of %u)", page, pages);
		return (size_t)n < cap ? (size_t)n : cap - 1;
	}

	size_t len = (size_t)snprintf(out, cap, "log %u/%u:", page, pages);
	if (len >= cap) {
		return cap - 1;
	}

	unsigned first = (page - 1) * MC_EVLOG_PAGE_LINES;      /* 0 = newest */
	for (unsigned i = 0; i < MC_EVLOG_PAGE_LINES && first + i < l->count; i++) {
		unsigned idx = (l->head + MC_EVLOG_ENTRIES - 1 - (first + i)) % MC_EVLOG_ENTRIES;
		const struct mc_evlog_entry *e = &l->e[idx];
		int w = snprintf(out + len, cap - len, "%s t+%us %s",
				 i == 0 ? "" : " |", (unsigned)e->sec, e->text);
		if (w < 0 || (size_t)w >= cap - len) {
			len = cap - 1;
			break;
		}
		len += (size_t)w;
	}
	return len;
}
