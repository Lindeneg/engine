#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "math/linalg.h"

// Lines in world space, for seeing what the simulation did: a bullet's path,
// the sphere a body was swept as, a model's bounds, later the editor's gizmos
// and selection. Everything is a line segment; a box is twelve of them.
//
// Immediate by default: what is drawn between one begin_frame and the next is
// gone after it. A Style with a lifetime keeps the primitive for that many
// seconds, which is how a shot fired inside one fixed step stays on screen long
// enough to look at. x_ray draws over everything, depth ignored.
//
//     debugdraw::line(muzzle, hit, red, debugdraw::Style{2.0f, false});
//
// Drawn after the scene through the same Frame block the mesh shaders read, so
// no camera is passed in. The frame is bracketed the way render's is:
//
//     debugdraw::begin_frame(dt);   // drops the immediate set, ages the kept one
//     ... simulation and game code draw ...
//     render::flush(assets);
//     debugdraw::flush();           // one upload, two draws

namespace debugdraw {

struct Config {
    u32 max_lines;       // immediate, per frame
    u32 max_retained;    // with a lifetime, alive at once
    u32 sphere_segments; // per circle; a sphere is three circles

    // axes() draws with these; every other primitive is given its colour
    linalg::Vec4 axis_color[3];
};

// The `debug_draw` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

struct Style {
    f32 lifetime; // seconds; 0 is this frame only
    b32 x_ray;    // depth test off
};

// Pushes the CPU side. The GPU buffers wait for the first flush, so a run
// without a GL context (the tests) never needs one.
void init(memory::Arena *a, Config cfg);
void shutdown();

// Overflow drops the primitive and warns once, as render's queue does.
void line(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec4 color);
void line(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec4 color, Style s);

// Axis-aligned, or the same box under a transform: a model's bounds in the
// entity's world matrix.
void box(linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color);
void box(linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color, Style s);
void box(linalg::Mat4 transform, linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color);
void box(linalg::Mat4 transform, linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color, Style s);

// Three great circles, one per axis plane.
void sphere(linalg::Vec3 center, f32 radius, linalg::Vec4 color);
void sphere(linalg::Vec3 center, f32 radius, linalg::Vec4 color, Style s);

// The transform's basis from its origin, each axis `length` long whatever the
// transform's scale.
void axes(linalg::Mat4 transform, f32 length);
void axes(linalg::Mat4 transform, f32 length, Style s);

// Three world-axis lines through a point, `size` end to end. A hit point.
void cross(linalg::Vec3 p, f32 size, linalg::Vec4 color);
void cross(linalg::Vec3 p, f32 size, linalg::Vec4 color, Style s);

// Clears the immediate set and ages the kept one by dt. GL-free.
void begin_frame(f32 dt);

// Uploads and draws both sets, depth-tested first and x-ray over it. Needs a context.
void flush();

// What is staged, for tests and an overlay.
u32 line_count();
u32 retained_count();

} // namespace debugdraw
