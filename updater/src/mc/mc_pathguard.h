#pragma once

/*
 * Which file-system paths touch the armed bundle?
 *
 * The bundle's SHA-256 is taken at ARM and checked again at START, so a swap is
 * caught either way. This is the second line: while a bundle is armed, the
 * upstream write/delete/rename entry points (SMP fs group, the fast upload
 * stream) refuse to modify it at all, so the operator's armed selection cannot
 * be replaced in the first place, and the refusal is visible to whoever tried.
 *
 * Conservative on purpose: a path is blocked if
 *   - its last component equals the locked bundle's name (any directory),
 *   - it is a directory that contains the bundle ("/lfs1", "/"), or
 *   - it is the bundle's full path.
 * Trailing slashes are ignored.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `dir` e.g. "/lfs1"; `locked` the bundle file name ("" = nothing locked). */
bool mc_path_blocked(const char *dir, const char *locked, const char *path);

#ifdef __cplusplus
}
#endif
