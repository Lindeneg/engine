#pragma once

#include "common.h"
#include "memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);

// "characters/soldier.glb" + ".mat.cfg" -> "characters/soldier.mat.cfg".
// Replaces from the last '.' in the final segment, appends when there is none.
// How sidecar files are addressed: an editor writes them beside the asset.
char *swap_ext(memory::Arena *a, const char *path, const char *ext);
}; // namespace path
