#pragma once

#include "core/common.h"
#include "core/memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);

// "soldier.glb" + ".mat.cfg" -> "soldier.mat.cfg"; replaces from the last '.'
// of the final segment, appends when there is none.
char *swap_ext(memory::Arena *a, const char *path, const char *ext);
}; // namespace path
