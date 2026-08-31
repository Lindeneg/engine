#pragma once

#include "common.h"
#include "memory.h"
#include "result.h"

namespace gltf {

// Parses path and logs what it contains. Everything allocated dies with the
// call; this exists to design the asset structs against real files.
result::Error inspect(memory::Arena *scratch, const char *path);

} // namespace gltf
