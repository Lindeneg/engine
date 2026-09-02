#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "math/linalg.h"
#include "core/memory.h"
#include "gfx/shader.h"

namespace render {

// queue capacities; overflow drops draws and warns once, since a dropped object
// is visible and an abort mid-frame is not diagnosable
struct Config {
    u32 max_draw_items;    // one per submesh submitted, not per model
    u32 max_objects;       // one per submit_model call
    u32 max_skinned_draws; // palette slots; each costs SHADER_MAX_JOINTS matrices
    linalg::Vec4 clear_color;
};

// The `render` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

// Creates the uniform buffers and queue arrays once. shader::init must have
// run: the buffers bind to the points programs linked against. Per-draw state
// comes from each material, not from here.
void init(memory::Arena *a, Config cfg, i32 fb_width, i32 fb_height);
void shutdown();

// The renderer owns the GPU copy of every mesh, uploaded on first submit, and
// hears from the manager when a slot dies. upload_model preloads.
void attach_assets(memory::Arena *a, asset::Manager *m);
void upload_model(asset::Manager *m, asset::ModelHandle h);

void resize(i32 fb_width, i32 fb_height);

// Uploads the frame constants, empties the queue, clears.
void begin_frame(const shader::FrameData *frame, linalg::Vec4 clear_color);

// One draw per submesh, no GL calls; flush decides the order. palette is NULL
// for a static model, and copied otherwise, so the caller may reuse it.
void submit_model(asset::Manager *m, asset::ModelHandle h, linalg::Mat4 model,
                  const linalg::Mat4 *palette, u32 joint_count);

// Sorts and draws everything submitted since begin_frame: opaque grouped by
// program and state, then transparent back-to-front.
void flush(asset::Manager *m);

} // namespace render
