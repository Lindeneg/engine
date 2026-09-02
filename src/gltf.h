#pragma once

#include "asset.h"
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

} // namespace gltf
