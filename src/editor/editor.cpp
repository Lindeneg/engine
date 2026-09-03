#include "editor/editor.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h> // offsetof
#include <string.h>

#include "core/logger.h"
#include "editor/gizmo.h"
#include "editor/undo.h"
#include "gfx/canvas.h"
#include "gfx/debugdraw.h"
#include "platform/dialog.h"
#include "platform/file.h"
#include "platform/path.h"
#include "ui/ui.h"

#define EDITOR_CFG "editor.cfg"

#define DEFAULT_PLAY_MAP "gameplay"
#define EDITOR_MAP "editor"

#define DEFAULT_SPEED 5.0f
#define DEFAULT_FAST_SCALE 4.0f
#define DEFAULT_SENSITIVITY 0.15f
#define DEFAULT_FOV 70.0f
#define DEFAULT_Z_NEAR 0.05f
#define DEFAULT_Z_FAR 500.0f
#define DEFAULT_YAW 0.0f
#define DEFAULT_PITCH -10.0f
#define DEFAULT_HIERARCHY_WIDTH 260.0f
#define DEFAULT_INSPECTOR_WIDTH 320.0f
#define DEFAULT_ASSETS_HEIGHT 300.0f
#define DEFAULT_DASHBOARD_WIDTH 640.0f
#define DEFAULT_DASHBOARD_HEIGHT 600.0f
#define DEFAULT_DASHBOARD_PADDING 24.0f
#define DEFAULT_MARKER_SIZE 0.3f
#define DEFAULT_FRUSTUM_DEPTH 10.0f
#define DEFAULT_UNDO_DEPTH 256
#define DEFAULT_UNDO_BLOB 4096
#define DEFAULT_SNAPSHOT_BYTES 262144

#define DIRTY_MARK " *"

#define DEFAULT_GIZMO_SIZE 90.0f
#define DEFAULT_GIZMO_TOLERANCE 8.0f
#define DEFAULT_GIZMO_PLANE_SIZE 0.25f
#define DEFAULT_GIZMO_PLANE_OFFSET 0.25f
#define DEFAULT_GIZMO_SNAP 0.25f

// the arrowhead's bars, as a fraction of the axis length
#define ARROWHEAD 0.08f

// a gizmo at the eye or behind it has no size on screen; hold it at this depth
#define MIN_GIZMO_DEPTH 0.01f

// straight up or down flips the basis; stop short of it
#define PITCH_LIMIT 89.0f

// focus_selection parks the camera this many bounding radii from the target
#define FOCUS_DISTANCE_SCALE 3.0f

#define NEW_ENTITY_NAME "entity"
#define INSPECTOR_LINE_CAP 160
#define STATUS_CAP 256

// the picker's filter: every asset is a .glb
#define ASSET_EXTENSION "glb"
#define ASSET_FILTER_LABEL "glTF binary"
#define BROWSE_LABEL "..."
#define ABS_PATH_CAP 1024

// what the listings hold; the file layer lists no more than this either
#define LIST_CAP FILE_LIST_CAP

// what the panels say when they refuse
#define STATUS_SAVE_FIRST "unsaved changes: save first, or open it again to discard them"
#define STATUS_NEEDS_NAME "a name is needed first"
#define STATUS_BAD_NAME "a name cannot hold a path separator"

// no component: nothing waits for the keyboard to be released
#define PENDING_NONE (-1)

namespace editor {

static const linalg::Vec3 DEFAULT_START_POSITION = {0.0f, 2.0f, 6.0f};
static const linalg::Vec4 DEFAULT_SELECTION_COLOR = {1.0f, 0.5f, 0.1f, 1.0f};
static const linalg::Vec4 DEFAULT_COLLIDER_COLOR = {0.3f, 0.9f, 0.9f, 1.0f};

// the two kinds of file the project imports; the assets panel shows one at a time
enum AssetKind { ASSET_KIND_MODEL, ASSET_KIND_CLIP, ASSET_KIND_COUNT };

// an asset's file and sidecar, looked up when its path first appears
struct AssetShown {
    char path[ENTITY_PATH_CAP];
    b32 exists;
    asset::ImportOptions model;
    asset::ClipOptions clip;
};

struct AssetPath {
    char v[ENTITY_PATH_CAP];
};

static const linalg::Vec3 FORWARD = {0.0f, 0.0f, -1.0f};
static const linalg::Vec3 RIGHT = {1.0f, 0.0f, 0.0f};
static const linalg::Vec3 UP = {0.0f, 1.0f, 0.0f};

// label, field, browse button: the field gets most of the row
static const f32 ASSET_ROW_WEIGHTS[3] = {0.3f, 0.55f, 0.15f};

// a label and the rest of the row; a level's name and its start mark
static const f32 SPLIT_ROW[2] = {0.3f, 0.7f};
static const f32 LEVEL_ROW[2] = {0.7f, 0.3f};

Config config_from(memory::Arena *a, config::Node *first) {
    ASSERT(a);

    config::Node *cam_block = config::find(first, "camera");
    config::Node *cam = cam_block ? cam_block->first_child : NULL;
    config::Node *panels_block = config::find(first, "panels");
    config::Node *panels = panels_block ? panels_block->first_child : NULL;

    Config c{};
    c.play_map = memory::push_str(a, config::get(first, "play_map", DEFAULT_PLAY_MAP));

    c.camera.speed = config::get_f32(cam, "speed", DEFAULT_SPEED);
    c.camera.fast_scale = config::get_f32(cam, "fast_scale", DEFAULT_FAST_SCALE);
    c.camera.sensitivity = config::get_f32(cam, "sensitivity", DEFAULT_SENSITIVITY);
    c.camera.fov = config::get_f32(cam, "fov", DEFAULT_FOV);
    c.camera.z_near = config::get_f32(cam, "near", DEFAULT_Z_NEAR);
    c.camera.z_far = config::get_f32(cam, "far", DEFAULT_Z_FAR);
    c.camera.start.position =
        config::value_vec3(config::find(cam, "position"), DEFAULT_START_POSITION);
    c.camera.start.yaw = config::get_f32(cam, "yaw", DEFAULT_YAW);
    c.camera.start.pitch = config::get_f32(cam, "pitch", DEFAULT_PITCH);

    config::Node *gizmo_block = config::find(first, "gizmo");
    config::Node *gz = gizmo_block ? gizmo_block->first_child : NULL;
    c.gizmo.size = config::get_f32(gz, "size", DEFAULT_GIZMO_SIZE);
    c.gizmo.tolerance = config::get_f32(gz, "tolerance", DEFAULT_GIZMO_TOLERANCE);
    c.gizmo.plane_size = config::get_f32(gz, "plane_size", DEFAULT_GIZMO_PLANE_SIZE);
    c.gizmo.plane_offset = config::get_f32(gz, "plane_offset", DEFAULT_GIZMO_PLANE_OFFSET);
    c.gizmo.snap = config::get_f32(gz, "snap", DEFAULT_GIZMO_SNAP);

    c.hierarchy_width = config::get_f32(panels, "hierarchy_width", DEFAULT_HIERARCHY_WIDTH);
    c.inspector_width = config::get_f32(panels, "inspector_width", DEFAULT_INSPECTOR_WIDTH);
    c.assets_height = config::get_f32(panels, "assets_height", DEFAULT_ASSETS_HEIGHT);

    config::Node *dash_block = config::find(first, "dashboard");
    config::Node *dash = dash_block ? dash_block->first_child : NULL;
    c.dashboard_width = config::get_f32(dash, "width", DEFAULT_DASHBOARD_WIDTH);
    c.dashboard_height = config::get_f32(dash, "height", DEFAULT_DASHBOARD_HEIGHT);
    c.dashboard_padding = config::get_f32(dash, "padding", DEFAULT_DASHBOARD_PADDING);

    c.selection_color =
        config::value_vec4(config::find(first, "selection_color"), DEFAULT_SELECTION_COLOR);
    c.collider_color =
        config::value_vec4(config::find(first, "collider_color"), DEFAULT_COLLIDER_COLOR);
    c.marker_size = config::get_f32(first, "marker_size", DEFAULT_MARKER_SIZE);
    c.frustum_depth = config::get_f32(first, "frustum_depth", DEFAULT_FRUSTUM_DEPTH);
    config::Node *undo_block = config::find(first, "undo");
    config::Node *un = undo_block ? undo_block->first_child : NULL;
    c.undo_depth = (u32)config::get_i32(un, "depth", DEFAULT_UNDO_DEPTH);
    c.undo_blob = (u32)config::get_i32(un, "blob", DEFAULT_UNDO_BLOB);
    c.snapshot_bytes = (u32)config::get_i32(first, "snapshot", DEFAULT_SNAPSHOT_BYTES);

    if (c.camera.z_near <= 0.0f || c.camera.z_far <= c.camera.z_near) {
        LOG_WARN("editor: camera near/far of %.3f/%.3f is not a valid range, using %.3f/%.1f",
                 (f64)c.camera.z_near, (f64)c.camera.z_far, (f64)DEFAULT_Z_NEAR,
                 (f64)DEFAULT_Z_FAR);
        c.camera.z_near = DEFAULT_Z_NEAR;
        c.camera.z_far = DEFAULT_Z_FAR;
    }
    if (c.undo_depth == 0) {
        LOG_WARN("editor: an undo depth of 0 remembers nothing, using %d", DEFAULT_UNDO_DEPTH);
        c.undo_depth = DEFAULT_UNDO_DEPTH;
    }
    if (c.undo_blob == 0) {
        LOG_WARN("editor: an undo blob of 0 bytes holds nothing, using %d", DEFAULT_UNDO_BLOB);
        c.undo_blob = DEFAULT_UNDO_BLOB;
    }
    if (c.snapshot_bytes == 0) {
        LOG_WARN("editor: a snapshot of 0 bytes holds no level, using %d", DEFAULT_SNAPSHOT_BYTES);
        c.snapshot_bytes = DEFAULT_SNAPSHOT_BYTES;
    }
    return c;
}

ConfigResult load_config(memory::Arena *a, memory::Arena *scratch, const char *root) {
    ASSERT(a && scratch && root);

    memory::ArenaTemp t = memory::temp_begin(scratch);
    const char *path = FPATH(scratch, root, EDITOR_CFG);

    config::DocResult doc = config::load(scratch, path);
    if (doc.err != result::OK) {
        LOG_ERR("editor: cannot read %s", path);
        memory::temp_end(t);
        return ConfigResult{.err = doc.err};
    }
    if (doc.val->problems) {
        LOG_ERR("editor: %s has %u structural problem(s)", path, doc.val->problems);
        memory::temp_end(t);
        return ConfigResult{.err = result::ERR_PARSE};
    }

    Config c = config_from(a, doc.val->first);
    memory::temp_end(t);
    return ConfigResult{.val = c};
}

struct Editor {
    Config cfg;
    Viewpoint view;
    b32 active;
    b32 looking; // editor_look held: the cursor is captured and the mouse turns the camera

    // by id, resolved to a handle at the top of each frame; the handle is what
    // the rest of the frame reads
    u32 selected_id;
    entity::Handle selected;
    undo::Journal *journal;

    // the project, and the level being edited, which need not be its start level
    project::Project *project;
    b32 project_pending; // the title was typed into; written once the keyboard lets go
    char level[ENTITY_NAME_CAP];
    char new_level_name[ENTITY_NAME_CAP];
    char discard_level[ENTITY_NAME_CAP]; // opening this again abandons unsaved changes
    char level_status[STATUS_CAP];

    // the name the selection would be saved under, defaulted once per selection
    char definition_name[ENTITY_NAME_CAP];
    u32 definition_for_id;
    char entity_status[STATUS_CAP];

    // what the project folder holds, scanned when first shown and after anything is written
    b32 scanned;
    entity::Name *levels;
    u32 level_count;
    entity::Name *templates;
    u32 template_count;
    entity::Name *definitions;
    u32 definition_count;
    AssetPath *models;
    u32 model_count;
    AssetPath *animations;
    u32 animation_count;

    // the assets panel's selection, whose settings it shows
    char asset_selected[ENTITY_PATH_CAP];
    AssetKind asset_kind;
    char asset_status[STATUS_CAP];

    // the asset field whose list of imports is unfolded under it, if any: the
    // entity, the component and the field's key, which is a static string
    u32 picker_id;
    i32 picker_component;
    const char *picker_field;

    // the level as text from the moment play began; its own arena, reset per snapshot
    memory::Arena *scratch;
    memory::Arena snapshot_arena;
    char *snapshot_text; // NULL when nothing is playing

    // a path typed into a component resolves once the keyboard lets go, not per keystroke
    entity::Handle pending_entity;
    i32 pending_component;

    // the assets whose settings are on show, read once per path rather than per frame
    AssetShown shown[ASSET_KIND_COUNT];

    // the translate gizmo: its handles this frame, which one the pointer is
    // over, and the drag in progress
    gizmo::Frame gizmo_frame;
    b32 gizmo_shown;
    gizmo::Handle hot_handle;
    gizmo::Drag drag;
    linalg::Vec3 drag_world_start;   // where the entity was in the world when grabbed
    entity::Transform drag_before;   // for the journal
    b32 gizmo_local;                 // axes from the entity rather than the world

    input::Map play_map;
    input::Map editor_map;
    input::Action look;
    input::Action select;
    input::Action forward, back, left, right, up, down, fast;
    input::Action focus;
    input::Action delete_entity;
    input::Action undo_action;
    input::Action redo_action;
    input::Action save;
    input::Action toggle_space;
    input::Action snap;
};

Editor *init(memory::Arena *a, memory::Arena *scratch, Config cfg, input::State *in,
             project::Project *proj) {
    ASSERT(a && scratch && in && proj);

    Editor *ed = push_struct(a, Editor);
    ed->cfg = cfg;
    ed->view = cfg.camera.start;
    ed->journal = undo::create(a, scratch, cfg.undo_depth, cfg.undo_blob);
    ed->scratch = scratch;
    ed->snapshot_arena = memory::create(cfg.snapshot_bytes);
    ed->pending_component = PENDING_NONE;
    ed->hot_handle = gizmo::HANDLE_NONE;
    ed->drag.handle = gizmo::HANDLE_NONE;
    ed->project = proj;
    snprintf(ed->level, sizeof(ed->level), "%s", proj->level);

    ed->levels = push_array(a, entity::Name, LIST_CAP);
    ed->templates = push_array(a, entity::Name, LIST_CAP);
    ed->definitions = push_array(a, entity::Name, LIST_CAP);
    ed->models = push_array(a, AssetPath, LIST_CAP);
    ed->animations = push_array(a, AssetPath, LIST_CAP);

    ed->play_map = input::find_map(in, cfg.play_map);
    ed->editor_map = input::find_map(in, EDITOR_MAP);
    ed->look = input::find(in, "editor_look");
    ed->select = input::find(in, "editor_select");
    ed->forward = input::find(in, "editor_forward");
    ed->back = input::find(in, "editor_back");
    ed->left = input::find(in, "editor_left");
    ed->right = input::find(in, "editor_right");
    ed->up = input::find(in, "editor_up");
    ed->down = input::find(in, "editor_down");
    ed->fast = input::find(in, "editor_fast");
    ed->focus = input::find(in, "focus_selection");
    ed->delete_entity = input::find(in, "delete_entity");
    ed->undo_action = input::find(in, "undo");
    ed->redo_action = input::find(in, "redo");
    ed->save = input::find(in, "save_level");
    ed->toggle_space = input::find(in, "toggle_gizmo_space");
    ed->snap = input::find(in, "gizmo_snap");

    // maps start enabled; the editor's waits for edit mode
    input::set_map_enabled(in, ed->editor_map, false);
    return ed;
}

void snapshot(Editor *ed, entity::World *w) {
    ASSERT(ed && w);
    memory::reset(&ed->snapshot_arena);
    ed->snapshot_text = entity::write_level(&ed->snapshot_arena, w);
}

b32 restore(Editor *ed, entity::World *w, asset::Manager *m) {
    ASSERT(ed && w && m);
    if (!ed->snapshot_text)
        return false;

    // the parser tokenizes in place, so it works on a copy and the snapshot stays whole
    memory::ArenaTemp t = memory::temp_begin(ed->scratch);
    char *text = memory::push_str(ed->scratch, ed->snapshot_text);
    entity::clear(w);
    u32 spawned = entity::load_level_text(w, m, text, "<snapshot>");
    memory::temp_end(t);

    ed->snapshot_text = NULL;
    LOG_INFO("editor: the level is back as it was, %u instance(s)", spawned);
    return true;
}

b32 dirty(const Editor *ed) {
    ASSERT(ed);
    return !undo::is_clean(ed->journal);
}

void set_active(Editor *ed, entity::World *w, asset::Manager *m, input::State *in,
                window::Window *win, b32 on) {
    ASSERT(ed && w && m && in && win);

    if ((ed->active != 0) == (on != 0))
        return;
    ed->active = on ? 1 : 0;
    ed->looking = false;

    // play runs on a copy: what the game does to the world is undone by coming back
    if (on)
        restore(ed, w, m);
    else
        snapshot(ed, w);

    input::set_map_enabled(in, ed->play_map, !on);
    input::set_map_enabled(in, ed->editor_map, on);

    // editing wants the cursor loose for the panels; play wants it captured for aim
    input::set_mouse_look(in, win, !on);

    LOG_INFO("editor: %s", on ? "editing" : "playing");
}

b32 active(const Editor *ed) {
    ASSERT(ed);
    return ed->active;
}

void select(Editor *ed, entity::World *w, entity::Handle h) {
    ASSERT(ed && w);
    ed->selected = h;
    ed->selected_id = entity::id_of(w, h);
}

entity::Handle selected(const Editor *ed, entity::World *w) {
    ASSERT(ed && w);
    return entity::find_by_id(w, ed->selected_id);
}

const Viewpoint *viewpoint(const Editor *ed) {
    ASSERT(ed);
    return &ed->view;
}

static linalg::Quat orientation(Viewpoint v) {
    return linalg::quat_from_euler(linalg::Vec3{v.pitch, v.yaw, 0.0f});
}

linalg::Mat4 viewpoint_world(Viewpoint v) {
    return linalg::mat4_from_trs(v.position, orientation(v), linalg::Vec3{1.0f, 1.0f, 1.0f});
}

intersect::Ray pick_ray(Viewpoint v, f32 fov, f32 aspect, linalg::Vec2 pixel, i32 fb_width,
                        i32 fb_height) {
    ASSERT(fb_width > 0 && fb_height > 0);

    // the pixel in clip space, then on the plane one unit ahead of the eye:
    // the inverse of what mat4_perspective does
    f32 nx = pixel.x / (f32)fb_width * 2.0f - 1.0f;
    f32 ny = 1.0f - pixel.y / (f32)fb_height * 2.0f;
    f32 half = tanf(DEG2RAD(fov) * 0.5f);

    linalg::Vec3 local{nx * half * aspect, ny * half, -1.0f};
    linalg::Vec3 dir = linalg::vec3_normalize(linalg::quat_rotate(orientation(v), local));
    return intersect::Ray{v.position, dir};
}

entity::Handle pick(entity::World *w, asset::Manager *m, intersect::Ray ray, f32 marker_size,
                    f32 max_distance) {
    ASSERT(w && m);

    entity::Handle best = entity::NONE;
    f32 best_distance = max_distance;

    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h)) {
        entity::Entity *e = entity::get(w, h);
        if (!e || !e->active)
            continue;

        f32 distance;
        entity::Renderable *r = entity::renderable(w, h);
        if (r && r->visible) {
            // the box is tested in the entity's own space, where it is axis-aligned
            linalg::Mat4 inv = linalg::mat4_inverse_affine(e->world);
            linalg::Vec3 origin = linalg::mat4_mul_point(inv, ray.origin);
            linalg::Vec3 dir = linalg::vec3_normalize_or(linalg::mat4_mul_dir(inv, ray.dir), FORWARD);

            intersect::Aabb box;
            asset::model_bounds(m, r->model, &box.min, &box.max);

            intersect::Hit hit;
            if (!intersect::ray_aabb(intersect::Ray{origin, dir}, box, max_distance, &hit))
                continue;

            // the local t is in scaled units; measure in the world instead
            linalg::Vec3 point = linalg::mat4_mul_point(e->world, hit.point);
            distance = linalg::vec3_length(point - ray.origin);
        } else {
            intersect::Hit hit;
            if (!intersect::ray_sphere(ray, linalg::mat4_translation(e->world), marker_size,
                                       max_distance, &hit))
                continue;
            distance = hit.t;
        }

        if (distance < best_distance) {
            best_distance = distance;
            best = h;
        }
    }
    return best;
}

b32 build_frame(Editor *ed, entity::World *w, f32 aspect, shader::FrameData *out) {
    ASSERT(ed && w && out);

    entity::Camera lens{};
    lens.fov_y = ed->cfg.camera.fov;
    lens.z_near = ed->cfg.camera.z_near;
    lens.z_far = ed->cfg.camera.z_far;
    return entity::build_frame(w, viewpoint_world(ed->view), &lens, aspect, out);
}

static void fly(Editor *ed, input::State *in, window::Window *win, f32 dt) {
    const CameraConfig *c = &ed->cfg.camera;

    b32 look = input::down(in, ed->look);
    if ((ed->looking != 0) != (look != 0)) {
        ed->looking = look;
        input::set_mouse_look(in, win, look);
    }
    if (!ed->looking)
        return;

    linalg::Vec2 d = input::cursor_delta(in);
    ed->view.yaw -= d.x * c->sensitivity;
    ed->view.pitch -= d.y * c->sensitivity;
    if (ed->view.pitch > PITCH_LIMIT)
        ed->view.pitch = PITCH_LIMIT;
    if (ed->view.pitch < -PITCH_LIMIT)
        ed->view.pitch = -PITCH_LIMIT;

    linalg::Vec3 wish{};
    if (input::down(in, ed->forward))
        wish = wish + FORWARD;
    if (input::down(in, ed->back))
        wish = wish - FORWARD;
    if (input::down(in, ed->right))
        wish = wish + RIGHT;
    if (input::down(in, ed->left))
        wish = wish - RIGHT;

    linalg::Quat q = orientation(ed->view);
    linalg::Vec3 move = linalg::quat_rotate(q, wish);
    if (input::down(in, ed->up))
        move = move + UP;
    if (input::down(in, ed->down))
        move = move - UP;
    move = linalg::vec3_normalize_or(move, linalg::Vec3{0.0f, 0.0f, 0.0f});

    f32 speed = c->speed * (input::down(in, ed->fast) ? c->fast_scale : 1.0f);
    ed->view.position = ed->view.position + move * (speed * dt);
}

// the free camera takes the pose of an entity: its -Z becomes the view direction
static void look_through(Editor *ed, entity::Entity *e) {
    linalg::Vec3 f = linalg::vec3_normalize_or(-1.0f * linalg::mat4_axis(e->world, 2), FORWARD);
    ed->view.position = linalg::mat4_translation(e->world);
    ed->view.pitch = RAD2DEG(asinf(f.y));
    ed->view.yaw = RAD2DEG(atan2f(-f.x, -f.z));
}

// a subtree too big for an entry: the journal cannot follow it, so it forgets
static void forget_history(Editor *ed) { undo::clear(ed->journal); }

// A row dropped on another, or on nothing: the child moves under the target,
// or to the top level, keeping its place in the world. Refused where the
// entity layer refuses, a parent inside the child's own subtree.
static void drop_reparent(Editor *ed, entity::World *w, asset::Manager *m, entity::Handle child,
                          entity::Handle parent) {
    entity::Entity *e = entity::get(w, child);
    if (!e || child.v == parent.v || e->parent.v == parent.v)
        return;

    u32 old_parent = entity::id_of(w, e->parent);
    char old_joint[ENTITY_NAME_CAP];
    memcpy(old_joint, e->attach_joint, sizeof(old_joint));
    entity::Transform before = e->local;
    linalg::Vec3 world = linalg::mat4_translation(e->world);

    if (!entity::set_parent(w, m, child, parent, NULL))
        return;
    entity::set_world_position(w, child, world);

    undo::record_reparent(ed->journal, e->id, old_parent, entity::id_of(w, parent),
                          old_joint[0] ? old_joint : NULL, &before, &e->local);
}

static void save_level(Editor *ed, entity::World *w, asset::Manager *m) {
    result::Error e = entity::save_level(w, m, ed->level);
    if (e != result::OK) {
        LOG_ERR("editor: saving level '%s' failed: %s", ed->level, result::str(e));
        snprintf(ed->level_status, sizeof(ed->level_status),
                 "%s could not be written; the log says why", ed->level);
        return;
    }
    undo::mark_clean(ed->journal);
    ed->discard_level[0] = 0;
    ed->level_status[0] = 0;
    LOG_INFO("editor: saved level '%s'", ed->level);
}

static void save_project(Editor *ed) {
    if (project::save(ed->scratch, ed->project) != result::OK)
        LOG_ERR("editor: %s could not be written", PROJECT_CFG);
}

static void draw_tree(Editor *ed, entity::World *w, entity::Handle h) {
    entity::Entity *e = entity::get(w, h);
    if (!e)
        return;

    // the handle, not the name: siblings may share one
    ui::push_id(h.v);

    b32 open = false;
    if (ui::tree_item(e->name, e->first_child.v != 0, ed->selected.v == h.v, &open, h.v))
        select(ed, w, h);

    if (open) {
        for (entity::Handle c = e->first_child; c.v; c = entity::get(w, c)->next_sibling)
            draw_tree(ed, w, c);
        ui::tree_pop();
    }

    ui::pop_id();
}

// a drag, in a panel or on the gizmo, or a run of typing is one edit
static b32 continuing(const Editor *ed) {
    return ui::dragging() || ui::wants_keyboard() || ed->drag.handle != gizmo::HANDLE_NONE;
}

static u32 field_size(const entity::FieldDesc *f) {
    switch (f->type) {
    case entity::FIELD_VEC3:
        return sizeof(linalg::Vec3);
    case entity::FIELD_STRING:
        return f->cap;
    default:
        return sizeof(u32); // F32, I32, B32 and ENUM are four bytes each
    }
}

// The OS picker, with the result written back as a path under the asset root.
// A file outside the root is refused: there is one assets folder, and the
// engine does not know where anything else lives.
static void browse_asset(asset::Manager *m, char *path, u32 cap) {
    char abs[ABS_PATH_CAP];
    if (!dialog::open_file("Choose an asset", ASSET_FILTER_LABEL, ASSET_EXTENSION, asset::root(m),
                           abs, sizeof(abs)))
        return;

    if (!path::relative_to(asset::root(m), abs, path, cap))
        LOG_WARN("editor: %s is not under the asset root %s; assets live there or nowhere", abs,
                 asset::root(m));
}

static b32 picker_open(const Editor *ed, u32 id, entity::Component c, const entity::FieldDesc *f) {
    return ed->picker_id == id && ed->picker_component == (i32)c && ed->picker_field == f->key;
}

static void close_picker(Editor *ed) { ed->picker_field = NULL; }

// what the project has imported of the field's kind, one row each, and the OS
// picker for a file the project does not hold yet; a choice writes the path
// and the caller's change detection does the rest
static void asset_picker_rows(Editor *ed, asset::Manager *m, const entity::FieldDesc *f,
                              char *path) {
    b32 model = f->asset == entity::ASSET_MODEL;
    const AssetPath *paths = model ? ed->models : ed->animations;
    u32 count = model ? ed->model_count : ed->animation_count;

    ui::push_id("picker");
    ui::columns(2, SPLIT_ROW);
    if (count == 0) {
        ui::spacer();
        ui::label_dim(model ? "(no models imported)" : "(no animations imported)");
    }
    for (u32 i = 0; i < count; i++) {
        ui::spacer();
        ui::push_id(i);
        if (ui::button(paths[i].v)) {
            snprintf(path, f->cap, "%s", paths[i].v);
            close_picker(ed);
        }
        ui::pop_id();
    }
    ui::spacer();
    if (ui::button("Other file...")) {
        browse_asset(m, path, f->cap);
        close_picker(ed);
    }
    ui::columns(1);
    ui::pop_id();
}

// one row per field, by type; the key is the row's label and the widget's id
static void field_row(Editor *ed, asset::Manager *m, u32 id, entity::Component c,
                      const entity::FieldDesc *f, void *comp) {
    u8 *at = (u8 *)comp + f->offset;
    switch (f->type) {
    case entity::FIELD_F32:
        ui::columns(2);
        ui::label(f->key);
        ui::float_field(f->key, (f32 *)at);
        break;
    case entity::FIELD_I32:
        ui::columns(2);
        ui::label(f->key);
        ui::int_field(f->key, (i32 *)at);
        break;
    case entity::FIELD_B32:
        ui::columns(1);
        ui::checkbox(f->key, (b32 *)at);
        break;
    case entity::FIELD_VEC3: {
        linalg::Vec3 *v = (linalg::Vec3 *)at;
        ui::columns(4);
        ui::label(f->key);
        ui::push_id(f->key);
        ui::float_field("x", &v->x);
        ui::float_field("y", &v->y);
        ui::float_field("z", &v->z);
        ui::pop_id();
        break;
    }
    case entity::FIELD_STRING: {
        if (f->asset == entity::ASSET_NONE) {
            ui::columns(2);
            ui::label(f->key);
            ui::text_field(f->key, (char *)at, f->cap);
            break;
        }
        ui::columns(3, ASSET_ROW_WEIGHTS);
        ui::label(f->key);
        ui::text_field(f->key, (char *)at, f->cap);
        ui::push_id(f->key);
        b32 open = picker_open(ed, id, c, f);
        if (ui::button(BROWSE_LABEL)) {
            open = !open;
            if (open) {
                ed->picker_id = id;
                ed->picker_component = (i32)c;
                ed->picker_field = f->key;
            } else {
                close_picker(ed);
            }
        }
        if (open)
            asset_picker_rows(ed, m, f, (char *)at);
        ui::pop_id();
        break;
    }
    case entity::FIELD_ENUM:
        ui::columns(2);
        ui::label(f->key);
        ui::enum_field(f->key, f->names, f->name_count, (i32 *)at);
        break;
    }
}

// an instance of a definition that restates a component writes it back on save
static void mark_overridden(entity::Entity *e, entity::Component c) {
    if (e->definition[0])
        e->overridden |= 1u << c;
}

// the file and sidecar behind a path, looked up when the path changes and
// forgotten after an undo, so the disk is not asked every frame
static AssetShown *shown_for(Editor *ed, asset::Manager *m, AssetKind kind, const char *path) {
    AssetShown *s = &ed->shown[kind];
    if (strcmp(s->path, path) == 0)
        return s;

    snprintf(s->path, sizeof(s->path), "%s", path);
    s->exists = asset::exists(m, path);
    if (!s->exists)
        return s;
    if (kind == ASSET_KIND_MODEL)
        s->model = asset::import_options(m, path);
    else
        s->clip = asset::clip_options(m, path);
    return s;
}

static void forget_shown(Editor *ed) {
    for (u32 i = 0; i < ASSET_KIND_COUNT; i++)
        ed->shown[i].path[0] = 0;
}

// what the assets panel shows the settings of
static void select_asset(Editor *ed, const char *path, AssetKind kind) {
    snprintf(ed->asset_selected, sizeof(ed->asset_selected), "%s", path);
    ed->asset_kind = kind;
}

// The asset's own settings, beside the field that names it. They belong to the
// file, so every entity showing this model or clip changes with them: a change
// is applied to what is loaded, written to the sidecar, and journaled.
static void model_import_rows(Editor *ed, asset::Manager *m, AssetShown *s) {
    asset::ImportOptions before = s->model;
    ui::columns(2);
    ui::label("scale");
    ui::float_field("scale", &s->model.scale);
    ui::columns(1);
    ui::checkbox("z_up", &s->model.z_up);

    if (memcmp(&before, &s->model, sizeof(before)) == 0)
        return;
    asset::apply_import_options(m, s->path, s->model);
    asset::write_import_options(m, s->path, s->model);
    undo::record_import_options(ed->journal, s->path, before, s->model, false);
}

static void clip_import_rows(Editor *ed, asset::Manager *m, AssetShown *s) {
    asset::ClipOptions before = s->clip;
    ui::columns(1);
    ui::checkbox("rotation_only", &s->clip.rotation_only);

    if (memcmp(&before, &s->clip, sizeof(before)) == 0)
        return;
    asset::apply_clip_options(m, s->path, s->clip);
    asset::write_clip_options(m, s->path, s->clip);
    undo::record_clip_options(ed->journal, s->path, before, s->clip);
}

// a way from a component's asset path to where that asset's settings are edited
static void asset_link_row(Editor *ed, const entity::FieldDesc *f, const void *comp) {
    if (f->type != entity::FIELD_STRING || f->asset == entity::ASSET_NONE)
        return;
    const char *path = (const char *)comp + f->offset;
    if (!path[0])
        return;

    ui::push_id(f->key);
    ui::columns(2, SPLIT_ROW);
    ui::spacer();
    if (ui::button("import settings..."))
        select_asset(ed, path,
                     f->asset == entity::ASSET_MODEL ? ASSET_KIND_MODEL : ASSET_KIND_CLIP);
    ui::pop_id();
}

// "" is refused, and so is a name that would leave the folder it belongs in
static b32 valid_name(const char *name, char *status) {
    if (!name[0]) {
        snprintf(status, STATUS_CAP, "%s", STATUS_NEEDS_NAME);
        return false;
    }
    for (const char *c = name; *c; c++) {
        if (*c == '\\' || *c == ':' || (c == name && *c == '/')) {
            snprintf(status, STATUS_CAP, "%s", STATUS_BAD_NAME);
            return false;
        }
    }
    return true;
}

static b32 ends_with_ext(const char *name, const char *ext) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (dot++; *dot && *ext; dot++, ext++)
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext))
            return false;
    return *dot == 0 && *ext == 0;
}

// the .glb files directly in <root>/<dir>, as paths under the root
static u32 scan_assets(Editor *ed, asset::Manager *m, const char *dir, AssetPath *out) {
    memory::ArenaTemp t = memory::temp_begin(ed->scratch);

    file::Entry *entries = push_array(ed->scratch, file::Entry, LIST_CAP);
    u32 found = file::list(FPATH(ed->scratch, asset::root(m), dir), entries, LIST_CAP);
    found = MIN(found, (u32)LIST_CAP);

    u32 n = 0;
    for (u32 i = 0; i < found; i++) {
        if (entries[i].is_directory || !ends_with_ext(entries[i].name, ASSET_EXTENSION))
            continue;
        snprintf(out[n++].v, ENTITY_PATH_CAP, "%s/%s", dir, entries[i].name);
    }

    memory::temp_end(t);
    return n;
}

// the project folder's listings, taken when first shown and after anything is written
static void rescan(Editor *ed, asset::Manager *m) {
    u32 levels = entity::list_levels(m, ed->levels, LIST_CAP);
    u32 templates = entity::list_definitions(m, ENTITY_TEMPLATE_SUBDIR, ed->templates, LIST_CAP);
    u32 definitions = entity::list_definitions(m, NULL, ed->definitions, LIST_CAP);
    ed->level_count = MIN(levels, (u32)LIST_CAP);
    ed->template_count = MIN(templates, (u32)LIST_CAP);
    ed->definition_count = MIN(definitions, (u32)LIST_CAP);
    ed->model_count = scan_assets(ed, m, PROJECT_MODEL_DIR, ed->models);
    ed->animation_count = scan_assets(ed, m, PROJECT_ANIMATION_DIR, ed->animations);
    ed->scanned = true;
}

static void project_section(Editor *ed) {
    if (!ui::tree_node("Project", true))
        return;

    ui::columns(2, SPLIT_ROW);
    ui::label("title");
    if (ui::text_field("title", ed->project->title, sizeof(ed->project->title)))
        ed->project_pending = true;
    ui::label("starts in");
    ui::label_dim(ed->project->level);
    ui::columns(1);
    ui::tree_pop();
}

// the world holds a level that was just read or written: nothing to undo, nothing pending
static void fresh_level(Editor *ed, entity::World *w) {
    select(ed, w, entity::NONE);
    undo::clear(ed->journal);
    undo::mark_clean(ed->journal);
    forget_shown(ed);
    ed->discard_level[0] = 0;
    ed->level_status[0] = 0;
}

// an empty level under the typed name, written at once so it is there to come back to
static void new_level(Editor *ed, entity::World *w, asset::Manager *m) {
    const char *name = ed->new_level_name;
    if (!valid_name(name, ed->level_status))
        return;
    if (dirty(ed)) {
        snprintf(ed->level_status, sizeof(ed->level_status), "unsaved changes in %s: save first",
                 ed->level);
        return;
    }
    for (u32 i = 0; i < ed->level_count; i++) {
        if (strcmp(ed->levels[i].v, name) == 0) {
            snprintf(ed->level_status, sizeof(ed->level_status),
                     "a level named %s exists; open it instead", name);
            return;
        }
    }

    entity::clear(w);
    snprintf(ed->level, sizeof(ed->level), "%s", name);
    fresh_level(ed, w);
    save_level(ed, w, m);
    ed->new_level_name[0] = 0;
    rescan(ed, m);
}

// Unsaved changes are not thrown away on one click: the first says so, and the
// same level clicked again is the answer.
static void open_level(Editor *ed, entity::World *w, asset::Manager *m, const char *name) {
    if (dirty(ed) && strcmp(ed->discard_level, name) != 0) {
        snprintf(ed->discard_level, sizeof(ed->discard_level), "%s", name);
        snprintf(ed->level_status, sizeof(ed->level_status), "%s", STATUS_SAVE_FIRST);
        return;
    }

    entity::clear(w);
    snprintf(ed->level, sizeof(ed->level), "%s", name);
    result::Error e = entity::load_level(w, m, name);
    fresh_level(ed, w);
    if (e != result::OK)
        snprintf(ed->level_status, sizeof(ed->level_status),
                 "%s could not be read; the log says why", name);
}

static void set_start_level(Editor *ed, const char *name) {
    snprintf(ed->project->level, sizeof(ed->project->level), "%s", name);
    save_project(ed);
}

static void levels_section(Editor *ed, entity::World *w, asset::Manager *m) {
    if (!ui::tree_node("Levels", true))
        return;

    // the level, and whether what is on screen is what is on disk
    char line[INSPECTOR_LINE_CAP];
    snprintf(line, sizeof(line), "editing %s%s", ed->level, dirty(ed) ? DIRTY_MARK : "");
    ui::label(line);

    ui::columns(2);
    if (ui::button("Save"))
        save_level(ed, w, m);
    if (ui::button("New level"))
        new_level(ed, w, m);
    ui::columns(2, SPLIT_ROW);
    ui::label("new");
    ui::text_field("new_level", ed->new_level_name, sizeof(ed->new_level_name));

    for (u32 i = 0; i < ed->level_count; i++) {
        const char *name = ed->levels[i].v;
        ui::push_id(i);
        ui::columns(2, LEVEL_ROW);
        if (ui::button(name))
            open_level(ed, w, m, name);
        if (strcmp(name, ed->project->level) == 0)
            ui::label_dim("start");
        else if (ui::button("start"))
            set_start_level(ed, name);
        ui::pop_id();
    }
    ui::columns(1);

    if (ed->level_status[0])
        ui::label_dim(ed->level_status);
    ui::tree_pop();
}

// a new entity is the selection, and the journal knows it came
static void created(Editor *ed, entity::World *w, entity::Handle h) {
    if (!h.v)
        return;
    select(ed, w, h);
    if (!undo::record_create(ed->journal, w, h))
        forget_history(ed);
}

// a copy keeps what the file gave it and forgets the file; a reference follows it
static void create_from_definition(Editor *ed, entity::World *w, asset::Manager *m,
                                   const char *name, b32 copy) {
    entity::HandleResult r = entity::spawn_definition(w, m, name);
    if (r.err != result::OK)
        return; // the loader said why
    if (copy)
        entity::detach_definition(w, r.val);
    created(ed, w, r.val);
}

static void new_entity_section(Editor *ed, entity::World *w, asset::Manager *m) {
    if (!ui::tree_node("New entity", false))
        return;

    if (ui::button("Empty"))
        created(ed, w, entity::spawn(w, NEW_ENTITY_NAME));

    if (ed->template_count) {
        ui::label_dim("a copy of a template");
        ui::push_id("templates");
        for (u32 i = 0; i < ed->template_count; i++) {
            ui::push_id(i);
            if (ui::button(path::basename(ed->templates[i].v)))
                create_from_definition(ed, w, m, ed->templates[i].v, true);
            ui::pop_id();
        }
        ui::pop_id();
    }

    if (ed->definition_count) {
        ui::label_dim("an instance of a definition");
        ui::push_id("definitions");
        for (u32 i = 0; i < ed->definition_count; i++) {
            ui::push_id(i);
            if (ui::button(ed->definitions[i].v))
                create_from_definition(ed, w, m, ed->definitions[i].v, false);
            ui::pop_id();
        }
        ui::pop_id();
    }

    ui::tree_pop();
}

static void hierarchy_panel(Editor *ed, entity::World *w, asset::Manager *m, f32 height) {
    ui::begin_panel("Hierarchy", canvas::Rect{0.0f, 0.0f, ed->cfg.hierarchy_width, height});

    project_section(ed);
    levels_section(ed, w, m);
    new_entity_section(ed, w, m);
    ui::separator();

    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h))
        if (entity::get(w, h)->parent.v == 0)
            draw_tree(ed, w, h);
    ui::end_panel();

    // a row dragged onto another row, or onto the panel's empty space
    u32 dragged = 0;
    u32 onto = 0;
    if (ui::dropped(&dragged, &onto))
        drop_reparent(ed, w, m, entity::Handle{dragged}, entity::Handle{onto});
}

static void component_section(Editor *ed, entity::World *w, asset::Manager *m, entity::Handle h,
                              entity::Component c) {
    const entity::ComponentDesc *d = entity::component_desc(c);
    void *comp = entity::component(w, h, c);
    entity::Entity *e = entity::get(w, h);

    ui::push_id(d->key);
    b32 remove = false;
    if (ui::tree_node(d->key, true)) {
        for (u32 i = 0; i < d->field_count; i++) {
            const entity::FieldDesc *f = &d->fields[i];
            u8 *at = (u8 *)comp + f->offset;
            u32 size = field_size(f);

            u8 before[UNDO_VALUE_CAP];
            memcpy(before, at, size);
            field_row(ed, m, e->id, c, f, comp);
            if (memcmp(before, at, size) == 0)
                continue;

            mark_overridden(e, c);
            undo::record_field(ed->journal, e->id, (i32)c, f->offset, size, before, at,
                               continuing(ed));

            // a path resolves when typing stops; anything else right away
            if (f->type == entity::FIELD_STRING) {
                ed->pending_entity = h;
                ed->pending_component = (i32)c;
            } else {
                d->after_read(w, m, h, comp);
            }
        }

        // the settings of the assets those paths name live in the assets panel
        for (u32 i = 0; i < d->field_count; i++)
            asset_link_row(ed, &d->fields[i], comp);

        ui::columns(1);
        remove = ui::button("Remove");
        ui::tree_pop();
    }
    ui::pop_id();

    if (remove) {
        undo::record_remove(ed->journal, e->id, c, comp);
        entity::remove_component(w, h, c);
    }
}

static void vec3_row(const char *label, linalg::Vec3 *v) {
    ui::label(label);
    ui::push_id(label);
    ui::float_field("x", &v->x);
    ui::float_field("y", &v->y);
    ui::float_field("z", &v->z);
    ui::pop_id();
}

static void transform_section(Editor *ed, entity::World *w, entity::Handle h) {
    entity::Entity *e = entity::get(w, h);
    if (!ui::tree_node("Transform", true))
        return;

    // rotation is edited as degrees; a quaternion-authored entity shows zeros
    // until it is touched, after which it is euler for good
    entity::Transform before = e->local;
    ui::columns(4);
    vec3_row("position", &e->local.position);
    vec3_row("rotation", &e->local.euler);
    vec3_row("scale", &e->local.scale);
    ui::columns(1);
    ui::tree_pop();

    if (memcmp(&before, &e->local, sizeof(before)) == 0)
        return;
    if (memcmp(&before.euler, &e->local.euler, sizeof(before.euler)) != 0)
        entity::set_euler(&e->local, e->local.euler);
    undo::record_field(ed->journal, e->id, UNDO_ENTITY, (u32)offsetof(entity::Entity, local),
                       sizeof(entity::Transform), &before, &e->local, false);
}

// The selection becomes a file and an instance of it, and every other instance
// of that file is rebuilt to match. The journal cannot follow a change of
// provenance, so it forgets; the level is dirty until it is saved with the reference.
static void save_as_definition(Editor *ed, entity::World *w, asset::Manager *m,
                               entity::Handle h) {
    const char *name = ed->definition_name;
    if (!valid_name(name, ed->entity_status))
        return;

    if (entity::save_definition(w, m, h, name) != result::OK) {
        snprintf(ed->entity_status, sizeof(ed->entity_status),
                 "%s could not be written; the log says why", name);
        return;
    }

    entity::attach_definition(w, h, name);
    u32 rebuilt = entity::reload_definition(w, m, name);
    forget_history(ed);
    forget_shown(ed);
    rescan(ed, m);
    snprintf(ed->entity_status, sizeof(ed->entity_status), "saved as %s; %u instance(s) rebuilt",
             name, rebuilt);
}

static void inspector_panel(Editor *ed, entity::World *w, asset::Manager *m, i32 fb_width,
                            i32 fb_height) {
    ui::begin_panel("Inspector",
                    canvas::Rect{(f32)fb_width - ed->cfg.inspector_width, 0.0f,
                                 ed->cfg.inspector_width, (f32)fb_height});

    entity::Entity *e = entity::get(w, ed->selected);
    if (!e) {
        ui::label("Nothing selected");
        ui::end_panel();
        return;
    }
    entity::Handle h = ed->selected;

    ui::columns(2);
    ui::label("name");
    char name_before[ENTITY_NAME_CAP];
    memcpy(name_before, e->name, sizeof(name_before));
    if (ui::text_field("name", e->name, ENTITY_NAME_CAP))
        undo::record_field(ed->journal, e->id, UNDO_ENTITY, (u32)offsetof(entity::Entity, name),
                           ENTITY_NAME_CAP, name_before, e->name, true);
    ui::columns(1);

    b32 active_before = e->active;
    if (ui::checkbox("active", &e->active))
        undo::record_field(ed->journal, e->id, UNDO_ENTITY, (u32)offsetof(entity::Entity, active),
                           sizeof(e->active), &active_before, &e->active, false);

    if (e->definition[0]) {
        char line[INSPECTOR_LINE_CAP];
        snprintf(line, sizeof(line), "definition: %s", e->definition);
        ui::label(line);
    }

    transform_section(ed, w, h);

    for (u32 c = 0; c < entity::COMPONENT_COUNT; c++)
        if (e->components[c])
            component_section(ed, w, m, h, (entity::Component)c);

    if (entity::camera(w, h) && ui::button("Look through"))
        look_through(ed, e);

    if (ui::tree_node("Add component", false)) {
        for (u32 c = 0; c < entity::COMPONENT_COUNT; c++) {
            if (e->components[c])
                continue;
            const entity::ComponentDesc *d = entity::component_desc((entity::Component)c);
            if (!ui::button(d->key))
                continue;

            void *comp = entity::add_component(w, h, (entity::Component)c);
            if (!comp)
                continue;
            d->after_read(w, m, h, comp);
            mark_overridden(e, (entity::Component)c);
            undo::record_add(ed->journal, e->id, (entity::Component)c, comp);
        }
        ui::tree_pop();
    }

    // last: saving rebuilds the entity, so nothing below may touch it this frame
    ui::separator();
    if (ed->definition_for_id != e->id) {
        snprintf(ed->definition_name, sizeof(ed->definition_name), "%s", e->name);
        ed->definition_for_id = e->id;
    }
    ui::columns(2, SPLIT_ROW);
    ui::label("definition");
    ui::text_field("definition_name", ed->definition_name, sizeof(ed->definition_name));
    ui::columns(1);
    if (ed->entity_status[0])
        ui::label_dim(ed->entity_status);
    if (ui::button("Save as definition"))
        save_as_definition(ed, w, m, h);

    ui::end_panel();
}

// The model's own definition: entities/<stem>.entity holding a renderable of
// it, written through a passing entity so the usual writer and loader do the
// work. An existing file of that name is somebody's and is kept.
static void define_model(Editor *ed, entity::World *w, asset::Manager *m, const char *rel) {
    memory::ArenaTemp t = memory::temp_begin(ed->scratch);
    const char *stem = path::swap_ext(ed->scratch, path::basename(rel), "");

    for (u32 i = 0; i < ed->definition_count; i++) {
        if (strcmp(ed->definitions[i].v, stem) == 0) {
            snprintf(ed->asset_status, sizeof(ed->asset_status),
                     "%s imported; the definition %s already exists and was kept", rel, stem);
            memory::temp_end(t);
            return;
        }
    }

    entity::Handle h = entity::spawn(w, stem);
    entity::Renderable *r = entity::add_renderable(w, h);
    result::Error e = result::ERR_EXHAUSTED;
    if (r) {
        snprintf(r->model_path, sizeof(r->model_path), "%s", rel);
        entity::component_desc(entity::COMPONENT_RENDERABLE)->after_read(w, m, h, r);
        e = entity::save_definition(w, m, h, stem);
    }
    entity::destroy(w, h);

    if (e == result::OK)
        snprintf(ed->asset_status, sizeof(ed->asset_status), "imported %s as the definition %s",
                 rel, stem);
    else
        snprintf(ed->asset_status, sizeof(ed->asset_status),
                 "%s imported; its definition could not be written, the log says why", rel);
    memory::temp_end(t);
}

// A file from anywhere is copied into the project by kind and given its
// sidecar; one already inside the project is used where it is. A model gets a
// definition to place it by.
static void import_asset(Editor *ed, entity::World *w, asset::Manager *m, AssetKind kind) {
    b32 model = kind == ASSET_KIND_MODEL;

    char abs[ABS_PATH_CAP];
    if (!dialog::open_file(model ? "Import a model" : "Import an animation", ASSET_FILTER_LABEL,
                           ASSET_EXTENSION, NULL, abs, sizeof(abs)))
        return;

    char rel[ENTITY_PATH_CAP];
    if (!path::relative_to(asset::root(m), abs, rel, sizeof(rel))) {
        const char *dir = model ? PROJECT_MODEL_DIR : PROJECT_ANIMATION_DIR;
        if (snprintf(rel, sizeof(rel), "%s/%s", dir, path::basename(abs)) >= (i32)sizeof(rel)) {
            snprintf(ed->asset_status, sizeof(ed->asset_status),
                     "the file's name is longer than the %d bytes a path may be", ENTITY_PATH_CAP);
            return;
        }

        memory::ArenaTemp t = memory::temp_begin(ed->scratch);
        const char *dst = FPATH(ed->scratch, asset::root(m), rel);
        b32 ok = true;
        if (!file::exists(dst))
            ok = file::make_directory(FPATH(ed->scratch, asset::root(m), dir)) &&
                 file::copy(abs, dst) == result::OK;
        memory::temp_end(t);

        if (!ok) {
            snprintf(ed->asset_status, sizeof(ed->asset_status),
                     "%s could not be copied in; the log says why", abs);
            return;
        }
    }

    // the sidecar with the defaults spelled out, so the settings have a file to live in
    if (!asset::has_import_sidecar(m, rel)) {
        if (model)
            asset::write_import_options(m, rel, asset::import_options(m, rel));
        else
            asset::write_clip_options(m, rel, asset::clip_options(m, rel));
    }

    select_asset(ed, rel, kind);
    forget_shown(ed);
    snprintf(ed->asset_status, sizeof(ed->asset_status), "imported %s", rel);
    rescan(ed, m);
    if (model) {
        define_model(ed, w, m, rel);
        rescan(ed, m);
    }
}

static void asset_list(Editor *ed, const char *title, const AssetPath *paths, u32 count,
                       AssetKind kind) {
    if (!ui::tree_node(title, true))
        return;
    if (count == 0)
        ui::label_dim("(none)");
    for (u32 i = 0; i < count; i++) {
        ui::push_id(i);
        if (ui::button(path::basename(paths[i].v)))
            select_asset(ed, paths[i].v, kind);
        ui::pop_id();
    }
    ui::tree_pop();
}

static void asset_settings(Editor *ed, asset::Manager *m) {
    ui::separator();
    ui::label(ed->asset_selected);

    AssetShown *s = shown_for(ed, m, ed->asset_kind, ed->asset_selected);
    if (!s->exists) {
        ui::label_dim("the file is missing");
        return;
    }
    if (ed->asset_kind == ASSET_KIND_MODEL)
        model_import_rows(ed, m, s);
    else
        clip_import_rows(ed, m, s);
}

static void assets_panel(Editor *ed, entity::World *w, asset::Manager *m, f32 top, f32 height) {
    ui::begin_panel("Assets", canvas::Rect{0.0f, top, ed->cfg.hierarchy_width, height});

    if (ui::button("Import model..."))
        import_asset(ed, w, m, ASSET_KIND_MODEL);
    if (ui::button("Import animation..."))
        import_asset(ed, w, m, ASSET_KIND_CLIP);
    ui::columns(2, SPLIT_ROW);
    if (ui::button("Refresh"))
        rescan(ed, m);
    ui::columns(1);

    asset_list(ed, "models", ed->models, ed->model_count, ASSET_KIND_MODEL);
    asset_list(ed, "animations", ed->animations, ed->animation_count, ASSET_KIND_CLIP);

    if (ed->asset_selected[0])
        asset_settings(ed, m);
    if (ed->asset_status[0])
        ui::label_dim(ed->asset_status);

    ui::end_panel();
}

// the selected camera's view volume, from its near plane to frustum_depth,
// at the framebuffer's aspect since that is what it would render
static void draw_frustum(const Editor *ed, entity::Entity *e, entity::Camera *c, f32 aspect) {
    linalg::Vec3 o = linalg::mat4_translation(e->world);
    linalg::Vec3 x = linalg::vec3_normalize_or(linalg::mat4_axis(e->world, 0), RIGHT);
    linalg::Vec3 y = linalg::vec3_normalize_or(linalg::mat4_axis(e->world, 1), UP);
    linalg::Vec3 z = linalg::vec3_normalize_or(linalg::mat4_axis(e->world, 2), -1.0f * FORWARD);

    f32 depths[2] = {c->z_near, ed->cfg.frustum_depth};
    f32 half = tanf(DEG2RAD(c->fov_y) * 0.5f);

    linalg::Vec3 corner[2][4];
    for (u32 d = 0; d < 2; d++) {
        f32 h = depths[d] * half;
        f32 wdt = h * aspect;
        linalg::Vec3 center = o - z * depths[d]; // forward is -Z
        corner[d][0] = center - x * wdt - y * h;
        corner[d][1] = center + x * wdt - y * h;
        corner[d][2] = center + x * wdt + y * h;
        corner[d][3] = center - x * wdt + y * h;
    }

    debugdraw::Style style{0.0f, true};
    for (u32 i = 0; i < 4; i++) {
        u32 j = (i + 1) % 4;
        debugdraw::line(corner[0][i], corner[0][j], ed->cfg.selection_color, style);
        debugdraw::line(corner[1][i], corner[1][j], ed->cfg.selection_color, style);
        debugdraw::line(corner[0][i], corner[1][i], ed->cfg.selection_color, style);
    }
}

static void draw_markers(Editor *ed, entity::World *w, asset::Manager *m, f32 aspect) {
    debugdraw::Style x_ray{0.0f, true};

    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h)) {
        entity::Entity *e = entity::get(w, h);
        if (!e || !e->active)
            continue;

        entity::Renderable *r = entity::renderable(w, h);
        b32 is_selected = ed->selected.v == h.v;

        // something to see draws itself, and a collider is drawn already;
        // anything else gets its axes
        if ((!r || !r->visible) && !entity::collider(w, h))
            debugdraw::axes(e->world, ed->cfg.marker_size);

        if (!is_selected)
            continue;

        if (r && r->visible) {
            linalg::Vec3 lo, hi;
            asset::model_bounds(m, r->model, &lo, &hi);
            debugdraw::box(e->world, lo, hi, ed->cfg.selection_color, x_ray);
        } else {
            // a cube the size of the axes, in the entity's own frame: a handle, not a halo
            f32 s = ed->cfg.marker_size;
            debugdraw::box(e->world, linalg::Vec3{-s, -s, -s}, linalg::Vec3{s, s, s},
                           ed->cfg.selection_color, x_ray);
        }

        entity::Camera *c = entity::camera(w, h);
        if (c)
            draw_frustum(ed, e, c, aspect);
    }
}

// The handles at the selection, sized so an axis is the same length on screen
// at any distance: the world length of one pixel at the gizmo's depth, times
// the size in pixels.
static b32 build_gizmo_frame(const Editor *ed, entity::World *w, i32 fb_height,
                             gizmo::Frame *out) {
    entity::Entity *e = entity::get(w, ed->selected);
    if (!e)
        return false;

    const GizmoConfig *g = &ed->cfg.gizmo;
    linalg::Vec3 origin = linalg::mat4_translation(e->world);

    linalg::Vec3 forward = linalg::quat_rotate(orientation(ed->view), FORWARD);
    f32 depth = MAX(linalg::vec3_dot(origin - ed->view.position, forward), MIN_GIZMO_DEPTH);
    f32 world_per_pixel = depth * 2.0f * tanf(DEG2RAD(ed->cfg.camera.fov) * 0.5f) / (f32)fb_height;

    out->origin = origin;
    out->length = g->size * world_per_pixel;
    out->plane_size = out->length * g->plane_size;
    out->plane_offset = out->length * g->plane_offset;

    const linalg::Vec3 world_axes[3] = {RIGHT, UP, -1.0f * FORWARD};
    for (u32 i = 0; i < 3; i++)
        out->axis[i] = ed->gizmo_local ? linalg::vec3_normalize_or(linalg::mat4_axis(e->world, i),
                                                                    world_axes[i])
                                       : world_axes[i];
    return true;
}

static linalg::Vec4 axis_color(u32 i) { return debugdraw::config()->axis_color[i]; }

// hover and drag; true while the pointer is on a handle or holding one, so a
// click there is not a pick
static b32 update_gizmo(Editor *ed, entity::World *w, input::State *in, intersect::Ray ray,
                        i32 fb_height, b32 pointer_free) {
    ed->gizmo_shown = build_gizmo_frame(ed, w, fb_height, &ed->gizmo_frame);
    if (!ed->gizmo_shown) {
        ed->hot_handle = gizmo::HANDLE_NONE;
        ed->drag.handle = gizmo::HANDLE_NONE;
        return false;
    }

    const gizmo::Frame f = ed->gizmo_frame;
    f32 tolerance = f.length / ed->cfg.gizmo.size * ed->cfg.gizmo.tolerance; // pixels to metres
    entity::Entity *e = entity::get(w, ed->selected);

    if (ed->drag.handle != gizmo::HANDLE_NONE) {
        if (!input::down(in, ed->select)) {
            ed->drag.handle = gizmo::HANDLE_NONE; // released: the journal closes at frame end
            return true;
        }

        f32 snap = input::down(in, ed->snap) ? ed->cfg.gizmo.snap : 0.0f;
        linalg::Vec3 moved = gizmo::delta(&ed->drag, ray, snap);
        entity::set_world_position(w, ed->selected, ed->drag_world_start + moved);
        undo::record_field(ed->journal, ed->selected_id, UNDO_ENTITY,
                           (u32)offsetof(entity::Entity, local), sizeof(entity::Transform),
                           &ed->drag_before, &e->local, true);
        return true;
    }

    ed->hot_handle = pointer_free ? gizmo::hit(&f, ray, tolerance) : gizmo::HANDLE_NONE;
    if (ed->hot_handle == gizmo::HANDLE_NONE)
        return false;

    if (input::pressed(in, ed->select)) {
        ed->drag = gizmo::begin(&f, ed->hot_handle, ray);
        ed->drag_world_start = f.origin;
        ed->drag_before = e->local;
        if (!ed->drag.valid)
            ed->drag.handle = gizmo::HANDLE_NONE; // edge-on: nothing to measure along
    }
    return true;
}

static void draw_gizmo(const Editor *ed) {
    if (!ed->gizmo_shown)
        return;

    const gizmo::Frame f = ed->gizmo_frame;
    gizmo::Handle live = ed->drag.handle != gizmo::HANDLE_NONE ? ed->drag.handle : ed->hot_handle;
    debugdraw::Style x_ray{0.0f, true};

    for (u32 i = 0; i < 3; i++) {
        linalg::Vec3 tip = f.origin + f.axis[i] * f.length;
        linalg::Vec4 color = (gizmo::Handle)i == live ? ed->cfg.selection_color : axis_color(i);
        debugdraw::line(f.origin, tip, color, x_ray);

        // an arrowhead: two bars across the tip, in the other two axes
        f32 bar = f.length * ARROWHEAD;
        linalg::Vec3 back = tip - f.axis[i] * bar;
        linalg::Vec3 u = f.axis[(i + 1) % 3] * bar;
        linalg::Vec3 v = f.axis[(i + 2) % 3] * bar;
        debugdraw::line(tip, back + u, color, x_ray);
        debugdraw::line(tip, back - u, color, x_ray);
        debugdraw::line(tip, back + v, color, x_ray);
        debugdraw::line(tip, back - v, color, x_ray);
    }

    for (u32 n = 0; n < 3; n++) {
        gizmo::Handle h = (gizmo::Handle)(gizmo::HANDLE_YZ + n);
        linalg::Vec3 u = f.axis[(n + 1) % 3];
        linalg::Vec3 v = f.axis[(n + 2) % 3];
        linalg::Vec3 c0 = f.origin + (u + v) * f.plane_offset;
        linalg::Vec3 c1 = c0 + u * f.plane_size;
        linalg::Vec3 c2 = c0 + (u + v) * f.plane_size;
        linalg::Vec3 c3 = c0 + v * f.plane_size;
        linalg::Vec4 color = h == live ? ed->cfg.selection_color : axis_color(n);
        debugdraw::line(c0, c1, color, x_ray);
        debugdraw::line(c1, c2, color, x_ray);
        debugdraw::line(c2, c3, color, x_ray);
        debugdraw::line(c3, c0, color, x_ray);
    }
}

// parks the camera looking at the selection from a distance that shows all of it
static void focus_selection(Editor *ed, entity::World *w, asset::Manager *m) {
    entity::Entity *e = entity::get(w, ed->selected);
    if (!e)
        return;

    linalg::Vec3 center = linalg::mat4_translation(e->world);
    f32 radius = ed->cfg.marker_size;

    entity::Renderable *r = entity::renderable(w, ed->selected);
    if (r && r->visible) {
        linalg::Vec3 lo, hi;
        asset::model_bounds(m, r->model, &lo, &hi);
        intersect::Aabb box = intersect::aabb_transform(intersect::Aabb{lo, hi}, e->world);
        center = intersect::aabb_center(box);
        radius = linalg::vec3_length(intersect::aabb_extents(box));
    }

    linalg::Vec3 forward = linalg::quat_rotate(orientation(ed->view), FORWARD);
    ed->view.position = center - forward * (radius * FOCUS_DISTANCE_SCALE);
}

// The first reference to an asset without a sidecar leaves one behind with the
// defaults spelled out, so the settings have a file to be edited in. Nothing
// is guessed: a clip that needs rotation_only still gets false, and the
// loader's warning says which to flip. Only for a file that exists, or a typo
// would litter the tree.
static void ensure_sidecars(asset::Manager *m, const entity::ComponentDesc *d, void *comp) {
    for (u32 i = 0; i < d->field_count; i++) {
        const entity::FieldDesc *f = &d->fields[i];
        if (f->type != entity::FIELD_STRING || f->asset == entity::ASSET_NONE)
            continue;

        const char *path = (const char *)comp + f->offset;
        if (!path[0] || !asset::exists(m, path) || asset::has_import_sidecar(m, path))
            continue;

        if (f->asset == entity::ASSET_MODEL)
            asset::write_import_options(m, path, asset::import_options(m, path));
        else
            asset::write_clip_options(m, path, asset::clip_options(m, path));
    }
}

// a typed path is resolved once the keyboard is released, so a model is not
// looked up on every keystroke of its name
static void resolve_pending(Editor *ed, entity::World *w, asset::Manager *m) {
    if (ed->pending_component == PENDING_NONE || ui::wants_keyboard())
        return;

    entity::Component c = (entity::Component)ed->pending_component;
    void *comp = entity::component(w, ed->pending_entity, c);
    if (comp) {
        const entity::ComponentDesc *d = entity::component_desc(c);
        ensure_sidecars(m, d, comp);
        d->after_read(w, m, ed->pending_entity, comp);
        forget_shown(ed); // a sidecar may just have appeared for the path on show
    }
    ed->pending_component = PENDING_NONE;
}

void frame(Editor *ed, entity::World *w, asset::Manager *m, input::State *in, window::Window *win,
           f32 dt, i32 fb_width, i32 fb_height) {
    ASSERT(ed && w && m && in && win);
    ASSERT_MSG(ed->active, "editor: frame while not editing");

    // the id is the truth; the handle is this frame's way to it
    ed->selected = entity::find_by_id(w, ed->selected_id);

    f32 aspect = (f32)fb_width / (f32)fb_height;

    if (!ed->scanned)
        rescan(ed, m);

    fly(ed, in, win, dt);
    f32 hierarchy_height = MAX((f32)fb_height - ed->cfg.assets_height, 0.0f);
    hierarchy_panel(ed, w, m, hierarchy_height);
    assets_panel(ed, w, m, hierarchy_height, ed->cfg.assets_height);
    inspector_panel(ed, w, m, fb_width, fb_height);

    // after the panels, so a click on one is not also a click in the world;
    // the gizmo gets the pointer first, then whatever is under it
    intersect::Ray ray =
        pick_ray(ed->view, ed->cfg.camera.fov, aspect, input::cursor(in), fb_width, fb_height);
    b32 pointer_free = !ed->looking && !ui::wants_mouse();
    b32 on_gizmo = update_gizmo(ed, w, in, ray, fb_height, pointer_free);

    if (input::pressed(in, ed->select) && pointer_free && !on_gizmo)
        select(ed, w, pick(w, m, ray, ed->cfg.marker_size, ed->cfg.camera.z_far));

    // keys go to a focused field before they mean anything here
    if (!ui::wants_keyboard()) {
        if (input::pressed(in, ed->focus))
            focus_selection(ed, w, m);

        if (input::pressed(in, ed->toggle_space))
            ed->gizmo_local = !ed->gizmo_local;

        if (input::pressed(in, ed->delete_entity) && ed->selected.v) {
            if (!undo::record_delete(ed->journal, w, ed->selected))
                forget_history(ed);
            entity::destroy(w, ed->selected);
            select(ed, w, entity::NONE);
        }

        // an undone setting must show its restored value, so what is shown is looked up again
        if (input::pressed(in, ed->undo_action) && undo::undo(ed->journal, w, m))
            forget_shown(ed);
        if (input::pressed(in, ed->redo_action) && undo::redo(ed->journal, w, m))
            forget_shown(ed);
        if (input::pressed(in, ed->save))
            save_level(ed, w, m);
    }

    resolve_pending(ed, w, m);
    if (!continuing(ed))
        undo::close(ed->journal);

    // the title typed into the project section, once typing has stopped
    if (ed->project_pending && !ui::wants_keyboard()) {
        save_project(ed);
        window::set_title(win, ed->project->title);
        ed->project_pending = false;
    }

    entity::draw_colliders(w, ed->cfg.collider_color);
    draw_markers(ed, w, m, aspect);
    draw_gizmo(ed);
}

} // namespace editor
