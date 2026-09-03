#pragma once

#include "core/common.h"
#include "core/memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);

// "soldier.glb" + ".mat.cfg" -> "soldier.mat.cfg"; replaces from the last '.'
// of the final segment, appends when there is none.
char *swap_ext(memory::Arena *a, const char *path, const char *ext);

// The part of abs below root, with forward slashes: what an asset path is.
// Either slash counts, and on Windows so does either case. False when abs is
// not under root, or the result does not fit.
b32 relative_to(const char *root, const char *abs, char *out, u32 cap);

// The final segment, a pointer into path: "D:/games/stuff" -> "stuff". Either
// slash counts. Empty after a trailing slash.
const char *basename(const char *path);
}; // namespace path
