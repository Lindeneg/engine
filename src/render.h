#pragma once

#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"

namespace render {

// Shaders are a fixed set known at compile time, not assets: their CPU-side
// source is throwaway, the GPU program is the real object, and none of the
// scope/epoch/dedup machinery in the asset manager would earn its keep.
enum ShaderId { SHADER_STATIC, SHADER_SKINNED, SHADER_COUNT };

// must match MAX_JOINTS in skinned.vert
#define RENDER_MAX_JOINTS 128

// Sets the GL state this engine assumes everywhere: depth testing on, back
// faces culled, counter-clockwise front faces (glTF's winding).
void init(i32 fb_width, i32 fb_height);

void resize(i32 fb_width, i32 fb_height);

void begin_frame(linalg::Vec4 clear_color);

// Reads and links every shader from <root>/assets/shaders.
result::Error load_shaders(memory::Arena *scratch, const char *root);

// Fills the gpu_* fields the asset manager deliberately leaves alone. Safe to
// call twice: an already-uploaded mesh is skipped.
void upload_model(asset::Manager *m, asset::ModelHandle h);

// Uploads the joint matrix palette that skinned meshes sample. Call before
// drawing a skinned model; entries past count keep whatever was there last.
void set_joint_palette(const linalg::Mat4 *joints, u32 count);

void draw_model(asset::Manager *m, asset::ModelHandle h, linalg::Mat4 model,
                linalg::Mat4 view_proj);

} // namespace render
