#pragma once

#include "asset.h"
#include "common.h"
#include "config.h"
#include "linalg.h"
#include "memory.h"
#include "shader.h"

namespace render {

// Queue capacities. Overflow drops draws and warns once -- it is a scale
// problem, not a broken build, and a dropped object is visible while a hard
// abort in the middle of a frame is not diagnosable.
struct Config {
    u32 max_draw_items;    // one per submesh submitted, not per model
    u32 max_objects;       // one per submit_model call
    u32 max_skinned_draws; // palette slots; each costs SHADER_MAX_JOINTS matrices
    linalg::Vec4 clear_color;
};

// The `render` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

// Creates the frame and joint uniform buffers and the queue arrays, which are
// pushed once and refilled per frame rather than re-pushed. shader::init must
// already have run: the buffers bind to the points programs were linked
// against. Per-draw state (cull, blend, depth) is deliberately not set here --
// it comes from each material.
void init(memory::Arena *a, Config cfg, i32 fb_width, i32 fb_height);
void shutdown();

// The renderer owns every GPU copy of a mesh in a table indexed by mesh slot,
// and registers with the manager to be told when a slot goes away. Meshes
// upload on first submit; upload_model preloads instead, for a level that
// should not hitch on its first frame.
void attach_assets(memory::Arena *a, asset::Manager *m);
void upload_model(asset::Manager *m, asset::ModelHandle h);

void resize(i32 fb_width, i32 fb_height);

// Uploads the per-frame constants every program reads, empties the queue, and
// clears. camera_pos is kept for the transparent pass's distance sort.
void begin_frame(const shader::FrameData *frame, linalg::Vec4 clear_color);

// Queues one draw per submesh. Issues no GL calls, so submission order carries
// no meaning -- flush decides the real order. palette is NULL for a static
// model; when present it is copied, so the caller may reuse its buffer for the
// next model immediately.
void submit_model(asset::Manager *m, asset::ModelHandle h, linalg::Mat4 model,
                  const linalg::Mat4 *palette, u32 joint_count);

// Sorts and draws everything submitted since begin_frame: opaque grouped by
// program and state, then transparent back-to-front.
void flush(asset::Manager *m);

} // namespace render
