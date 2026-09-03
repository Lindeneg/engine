#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"
#include "gfx/shader.h"
#include "math/intersect.h"
#include "math/linalg.h"
#include "platform/input.h"
#include "platform/window.h"
#include "scene/entity.h"

// The editor: the level held still, looked at from a free camera, with panels
// to see and change what is in it. It is engine tooling, so it knows entities,
// components and the descriptor table, and nothing about any game. Its
// settings are assets/editor.cfg; its keys are the "editor" map of input.cfg,
// which is on only while editing.
//
// The inspector is generated from the descriptor table: a component's fields
// become rows by their type, so a new component gets an inspector for free.
// Every edit goes through the undo journal (editor/undo.h), which also states
// what is not undoable yet.
//
// Entering edit mode loosens the cursor and swaps the game's map for the
// editor's, and puts the world back as it was when play began. The edited
// level is the only authority; play is a copy of it.
//
//     if (input::pressed(in, toggle))
//         editor::set_active(ed, in, win, !editor::active(ed));
//     ...
//     entity::end_frame(world, assets, &scratch, dt);   // world transforms current
//     if (editor::active(ed))
//         editor::frame(ed, world, assets, in, win, dt, fb_w, fb_h);

namespace editor {

// where the free camera is and where it looks, in degrees
struct Viewpoint {
    linalg::Vec3 position;
    f32 yaw;
    f32 pitch;
};

struct CameraConfig {
    f32 speed;       // metres per second
    f32 fast_scale;  // while editor_fast is held
    f32 sensitivity; // degrees per pixel while editor_look is held
    f32 fov;         // vertical, degrees
    f32 z_near;
    f32 z_far;
    Viewpoint start;
};

struct GizmoConfig {
    f32 size;         // an axis line, in pixels at any distance
    f32 tolerance;    // pixels a click may miss a handle by
    f32 plane_size;   // a plane square's side, as a fraction of size
    f32 plane_offset; // the square's near corner from the origin, likewise
    f32 snap;         // metres per step while snapping
};

struct Config {
    const char *play_map; // switched off while editing
    CameraConfig camera;
    GizmoConfig gizmo;
    f32 hierarchy_width;
    f32 inspector_width;
    f32 dashboard_width;
    f32 dashboard_height;
    linalg::Vec4 selection_color;
    linalg::Vec4 collider_color;
    f32 marker_size;   // the axes drawn for an entity with nothing to see, and its pick radius
    f32 frustum_depth; // how far a selected camera's frustum is drawn
    u32 undo_depth;    // edits remembered
    u32 undo_blob;     // bytes an entry holds for a created or deleted subtree
    u32 snapshot_bytes; // room for the level as text while it plays
};

// The document's top level; NULL or missing keys take the defaults. Strings are copied into a.
Config config_from(memory::Arena *a, config::Node *first);

RESULT_TYPE(Config, ConfigResult);

// Reads assets/editor.cfg. A missing or damaged file is a broken deployment.
ConfigResult load_config(memory::Arena *a, memory::Arena *scratch, const char *root);

struct Editor;

// Resolves the maps and actions once. Starts inactive, with the editor map off.
// level is the name save_level writes, copied; scratch serves the journal's
// subtree text.
Editor *init(memory::Arena *a, memory::Arena *scratch, Config cfg, input::State *in,
             const char *level);

// Edit mode is the authority and play runs on a copy: leaving edit mode takes
// a snapshot of the world, and coming back clears the world and rebuilds it
// from that snapshot. Nothing the game did survives, and nothing it spawned.
void set_active(Editor *ed, entity::World *w, asset::Manager *m, input::State *in,
                window::Window *win, b32 on);
b32 active(const Editor *ed);

// The two halves of that, on their own for a test or a caller with its own reasons.
void snapshot(Editor *ed, entity::World *w);
b32 restore(Editor *ed, entity::World *w, asset::Manager *m); // false with no snapshot to restore

// Changed since the last save, undos counted.
b32 dirty(const Editor *ed);

// One frame of edit mode: flies the camera, draws the panels and the markers,
// picks on click. Inside ui's frame, after entity::end_frame.
void frame(Editor *ed, entity::World *w, asset::Manager *m, input::State *in, window::Window *win,
           f32 dt, i32 fb_width, i32 fb_height);

// The view for the renderer, from the free camera.
b32 build_frame(Editor *ed, entity::World *w, f32 aspect, shader::FrameData *out);

// The selection is held by id, so it survives the entity being rebuilt.
entity::Handle selected(const Editor *ed, entity::World *w);
void select(Editor *ed, entity::World *w, entity::Handle h);

const Viewpoint *viewpoint(const Editor *ed);

// The pure parts, for tests. The ray leaves the viewpoint through a framebuffer
// pixel; pick answers the nearest entity along it within max_distance, a
// renderable by its bounds in its own space and anything else by a sphere of
// marker_size at its origin. NONE when nothing is hit.
linalg::Mat4 viewpoint_world(Viewpoint v);
intersect::Ray pick_ray(Viewpoint v, f32 fov, f32 aspect, linalg::Vec2 pixel, i32 fb_width,
                        i32 fb_height);
entity::Handle pick(entity::World *w, asset::Manager *m, intersect::Ray ray, f32 marker_size,
                    f32 max_distance);

} // namespace editor
