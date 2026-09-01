#pragma once

#include "asset.h"
#include "common.h"
#include "memory.h"
#include "result.h"

namespace gltf {

// The interpretation layer: cgltf never escapes gltf.cpp, so these deal purely
// in asset types. Paths are absolute -- asset::load_model owns root resolution
// and path dedup.
asset::ModelResult load_model(asset::Manager *m, asset::Scope s, const char *abs_path,
                              const char *name);

// Channels bind to target's joints by canonical name. Records the bind rate on
// the Clip and warns when nothing matched. Retargeting comes from opts, not
// from the file: it measures the translation ranges and warns when the setting
// looks wrong, but it never decides on the author's behalf.
asset::ClipResult load_clip(asset::Manager *m, asset::Scope s, const char *abs_path,
                            asset::SkeletonHandle target, asset::ClipOptions opts);

// Parses path and logs what it contains. Everything allocated dies with the
// call; this exists to design the asset structs against real files.
result::Error inspect(memory::Arena *scratch, const char *path);

// Exporters disagree on the separator in node names: FBX2glTF writes
// "mixamorig:Hips" where THREE writes "mixamorig_Hips". Every non-alphanumeric
// byte becomes '_' so the two collide. Run this before comparing or hashing any
// node name, or a clip binds to zero joints and silently animates nothing.
void canonical_name(char *dst, u64 cap, const char *src);

} // namespace gltf
