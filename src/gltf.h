#pragma once

#include "asset.h"
#include "memory.h"
#include "result.h"

namespace gltf {

// The interpretation layer: cgltf never escapes gltf.cpp. Paths are absolute;
// asset:: owns root resolution and dedup.
asset::ModelResult load_model(asset::Manager *m, asset::Scope s, const char *abs_path,
                              const char *name);

// Binds channels to target's joints by canonical name. rotation_only comes from
// opts, never from the file.
asset::ClipResult load_clip(asset::Manager *m, asset::Scope s, const char *abs_path,
                            asset::SkeletonHandle target, asset::ClipOptions opts);

// Parses path and logs its contents; for designing the asset structs against real files.
result::Error inspect(memory::Arena *scratch, const char *path);

} // namespace gltf
