#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "gfx/font.h"
#include "gfx/texture.h"
#include "math/linalg.h"

// The 2D layer over the scene: the HUD now, the editor's panels later. Pixel
// space with the origin at the top left, one quad batch per frame, drawn in
// submission order after everything in the world.
//
// Quads sharing a texture and a clip rectangle share a draw. A fill joins the
// glyph batch before it when there is one, through the white rows every font
// atlas carries, so a panel and its text usually cost one draw between them.
//
//     canvas::begin_frame(fb_w, fb_h);
//     canvas::rect(panel, background);
//     canvas::text(font, x, y + font->ascent, "hello", color);
//     ...
//     canvas::flush();   // after render and debugdraw

namespace canvas {

struct Rect {
    f32 x, y, w, h;
};

struct Config {
    u32 max_quads;      // per frame
    u32 max_batches;    // texture or clip changes per frame
    u32 max_clip_depth; // nested push_clip calls
};

// The `canvas` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

// Pushes the CPU side; the GPU buffers wait for the first flush.
void init(memory::Arena *a, Config cfg);
void shutdown();

// The framebuffer size fixes the projection and the outermost clip.
void begin_frame(i32 fb_width, i32 fb_height);

// Needs a context. Every clip pushed this frame must have been popped.
void flush();

// Overflow drops the quad and warns once.
void rect(Rect r, linalg::Vec4 color);
void rect_outline(Rect r, f32 thickness, linalg::Vec4 color);

// The whole texture stretched over r, multiplied by tint.
void image(const texture::Texture *t, Rect r, linalg::Vec4 tint);

// Pen at x on the baseline y. Returns the advance in pixels.
f32 text(font::Font *f, f32 x, f32 y, const char *s, linalg::Vec4 color);

// Intersected with the clip in force; a quad under an empty clip is not queued.
void push_clip(Rect r);
void pop_clip();

// What is staged, for tests and an overlay.
u32 quad_count();
u32 batch_count();

} // namespace canvas
