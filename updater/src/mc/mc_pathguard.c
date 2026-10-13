#include "mc_pathguard.h"

#include <string.h>

bool mc_path_blocked(const char *dir, const char *locked, const char *path)
{
	char norm[128], full[128];

	if (locked == NULL || locked[0] == '\0' || path == NULL) {
		return false;
	}
	size_t n = 0;

	while (n < sizeof(norm) - 1 && path[n] != '\0') {
		n++;
	}

	memcpy(norm, path, n);
	norm[n] = '\0';
	while (n > 1 && norm[n - 1] == '/') {
		norm[--n] = '\0';
	}
	const char *base = strrchr(norm, '/');

	base = base ? base + 1 : norm;
	if (strcmp(base, locked) == 0) {
		return true;
	}
	if (strlen(dir) + 1 + strlen(locked) >= sizeof(full)) {
		return true;                 /* cannot reason about it: refuse */
	}
	strcpy(full, dir);
	strcat(full, "/");
	strcat(full, locked);
	size_t fl = strlen(full);

	/* The full path (without relying on the last component), or an ancestor. */
	return n <= fl && strncmp(full, norm, n) == 0 && (n == 1 || full[n] == '/' || full[n] == '\0');
}
