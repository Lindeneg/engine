#pragma once

#include "common.h"
#include "memory.h"
#include "result.h"

namespace gltf {

// Parses path and logs what it contains. Everything allocated dies with the
// call; this exists to design the asset structs against real files.
result::Error inspect(memory::Arena *scratch, const char *path);

// Exporters disagree on the separator in node names: FBX2glTF writes
// "mixamorig:Hips" where THREE writes "mixamorig_Hips". Every non-alphanumeric
// byte becomes '_' so the two collide. Run this before comparing or hashing any
// node name, or a clip binds to zero joints and silently animates nothing.
void canonical_name(char *dst, u64 cap, const char *src);

} // namespace gltf
