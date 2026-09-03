#include "editor/editor.h"

#include <math.h>
#include <stddef.h> // offsetof
#include <string.h>

#include "core/logger.h"
#include "editor/undo.h"
#include "gfx/canvas.h"
#include "gfx/debugdraw.h"
#include "platform/dialog.h"
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
#define DEFAULT_MARKER_SIZE 0.3f
#define DEFAULT_FRUSTUM_DEPTH 10.0f
#define DEFAULT_UNDO_DEPTH 256

// straight up or down flips the basis; stop short of it
#define PITCH_LIMIT 89.0f

// focus_selection parks the camera this many bounding radii from the target
#define FOCUS_DISTANCE_SCALE 3.0f

#define NEW_ENTITY_NAME "entity"
#define INSPECTOR_LINE_CAP 160

// the picker's filter: every asset is a .glb
#define ASSET_EXTENSION "glb"
#define ASSET_FILTER_LABEL "glTF binary"
#define BROWSE_LABEL "..."
#define ABS_PATH_CAP 1024

// no component: nothing waits for the keyboard to be released
#define PENDING_NONE (-1)

namespace editor {

static const linalg::Vec3 DEFAULT_START_POSITION = {0.0f, 2.0f, 6.0f};
static const linalg::Vec4 DEFAULT_SELECTION_COLOR = {1.0f, 0.5f, 0.1f, 1.0f};

// one asset of each kind can be on show at a time: the inspector shows one entity
enum AssetShownSlot { SHOWN_MODEL, SHOWN_CLIP, ASSET_SHOWN_COUNT };

// an asset's file and sidecar, looked up when its path first appears
struct AssetShown {
    char path[ENTITY_PATH_CAP];
    b32 exists;
    asset::ImportOptions model;
    asset::ClipOptions clip;
};

static const linalg::Vec3 FORWARD = {0.0f, 0.0f, -1.0f};
static const linalg::Vec3 RIGHT = {1.0f, 0.0f, 0.0f};
static const linalg::Vec3 UP = {0.0f, 1.0f, 0.0f};

// label, field, browse button: the field gets most of the row
static const f32 ASSET_ROW_WEIGHTS[3] = {0.3f, 0.55f, 0.15f};

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

    c.hierarchy_width = config::get_f32(panels, "hierarchy_width", DEFAULT_HIERARCHY_WIDTH);
    c.inspector_width = config::get_f32(panels, "inspector_width", DEFAULT_INSPECTOR_WIDTH);

    c.selection_color =
        config::value_vec4(config::find(first, "selection_color"), DEFAULT_SELECTION_COLOR);
    c.marker_size = config::get_f32(first, "marker_size", DEFAULT_MARKER_SIZE);
    c.frustum_depth = config::get_f32(first, "frustum_depth", DEFAULT_FRUSTUM_DEPTH);
    c.undo_depth = (u32)config::get_i32(first, "undo_depth", DEFAULT_UNDO_DEPTH);

    if (c.camera.z_near <= 0.0f || c.camera.z_far <= c.camera.z_near) {
        LOG_WARN("editor: camera near/far of %.3f/%.3f is not a valid range, using %.3f/%.1f",
                 (f64)c.camera.z_near, (f64)c.camera.z_far, (f64)DEFAULT_Z_NEAR,
                 (f64)DEFAULT_Z_FAR);
        c.camera.z_near = DEFAULT_Z_NEAR;
        c.camera.z_far = DEFAULT_Z_FAR;
    }
    if (c.undo_depth == 0) {
        LOG_WARN("editor: undo_depth 0 remembers nothing, using %d", DEFAULT_UNDO_DEPTH);
        c.undo_depth = DEFAULT_UNDO_DEPTH;
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

    entity::Handle selected;
    undo::Journal *journal;
    char level[ENTITY_NAME_CAP];

    // a path typed into a component resolves once the keyboard lets go, not per keystroke
    entity::Handle pending_entity;
    i32 pending_component;

    // the assets whose settings are on show, read once per path rather than per frame
    AssetShown shown[ASSET_SHOWN_COUNT];

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
};

Editor *init(memory::Arena *a, Config cfg, input::State *in, const char *level) {
    ASSERT(a && in && level);

    Editor *ed = push_struct(a, Editor);
    ed->cfg = cfg;
    ed->view = cfg.camera.start;
    ed->journal = undo::create(a, cfg.undo_depth);
    ed->pending_component = PENDING_NONE;
    snprintf(ed->level, sizeof(ed->level), "%s", level);

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

    // maps start enabled; the editor's waits for edit mode
    input::set_map_enabled(in, ed->editor_map, false);
    return ed;
}

void set_active(Editor *ed, input::State *in, window::Window *win, b32 on) {
    ASSERT(ed && in && win);

    if ((ed->active != 0) == (on != 0))
        return;
    ed->active = on ? 1 : 0;
    ed->looking = false;

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

entity::Handle selected(const Editor *ed) {
    ASSERT(ed);
    return ed->selected;
}

void select(Editor *ed, entity::Handle h) {
    ASSERT(ed);
    ed->selected = h;
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

// a structural change: the journal cannot follow a recycled handle, so it forgets
static void forget_history(Editor *ed) { undo::clear(ed->journal); }

static void save_level(Editor *ed, entity::World *w, asset::Manager *m) {
    result::Error e = entity::save_level(w, m, ed->level);
    if (e == result::OK)
        LOG_INFO("editor: saved level '%s'", ed->level);
    else
        LOG_ERR("editor: saving level '%s' failed: %s", ed->level, result::str(e));
}

static void draw_tree(Editor *ed, entity::World *w, entity::Handle h) {
    entity::Entity *e = entity::get(w, h);
    if (!e)
        return;

    // the handle, not the name: siblings may share one
    ui::push_id(h.v);

    b32 open = false;
    if (ui::tree_item(e->name, e->first_child.v != 0, ed->selected.v == h.v, &open))
        ed->selected = h;

    if (open) {
        for (entity::Handle c = e->first_child; c.v; c = entity::get(w, c)->next_sibling)
            draw_tree(ed, w, c);
        ui::tree_pop();
    }

    ui::pop_id();
}

static void hierarchy_panel(Editor *ed, entity::World *w, asset::Manager *m, i32 fb_height) {
    ui::begin_panel("Hierarchy", canvas::Rect{0.0f, 0.0f, ed->cfg.hierarchy_width, (f32)fb_height});

    ui::columns(2);
    if (ui::button("New entity")) {
        entity::Handle h = entity::spawn(w, NEW_ENTITY_NAME);
        if (h.v) {
            ed->selected = h;
            forget_history(ed);
        }
    }
    if (ui::button("Save"))
        save_level(ed, w, m);
    ui::columns(1);
    ui::separator();

    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h))
        if (entity::get(w, h)->parent.v == 0)
            draw_tree(ed, w, h);
    ui::end_panel();
}

// a drag or a run of typing is one edit
static b32 continuing() { return ui::dragging() || ui::wants_keyboard(); }

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

// one row per field, by type; the key is the row's label and the widget's id
static void field_row(asset::Manager *m, const entity::FieldDesc *f, void *comp) {
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
    case entity::FIELD_STRING:
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
        if (ui::button(BROWSE_LABEL))
            browse_asset(m, (char *)at, f->cap);
        ui::pop_id();
        break;
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
static AssetShown *shown_for(Editor *ed, asset::Manager *m, AssetShownSlot slot,
                             const char *path) {
    AssetShown *s = &ed->shown[slot];
    if (strcmp(s->path, path) == 0)
        return s;

    snprintf(s->path, sizeof(s->path), "%s", path);
    s->exists = asset::exists(m, path);
    if (!s->exists)
        return s;
    if (slot == SHOWN_MODEL)
        s->model = asset::import_options(m, path);
    else
        s->clip = asset::clip_options(m, path);
    return s;
}

static void forget_shown(Editor *ed) {
    for (u32 i = 0; i < ASSET_SHOWN_COUNT; i++)
        ed->shown[i].path[0] = 0;
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

static void import_section(Editor *ed, asset::Manager *m, const entity::FieldDesc *f,
                           const void *comp) {
    if (f->type != entity::FIELD_STRING || f->asset == entity::ASSET_NONE)
        return;
    const char *path = (const char *)comp + f->offset;
    if (!path[0])
        return;

    AssetShownSlot slot = f->asset == entity::ASSET_MODEL ? SHOWN_MODEL : SHOWN_CLIP;
    AssetShown *s = shown_for(ed, m, slot, path);
    if (!s->exists)
        return;

    ui::push_id(f->key);
    if (ui::tree_node("import settings", true)) {
        if (slot == SHOWN_MODEL)
            model_import_rows(ed, m, s);
        else
            clip_import_rows(ed, m, s);
        ui::tree_pop();
    }
    ui::pop_id();
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
            field_row(m, f, comp);
            if (memcmp(before, at, size) == 0)
                continue;

            mark_overridden(e, c);
            undo::record_field(ed->journal, h, (i32)c, f->offset, size, before, at, continuing());

            // a path resolves when typing stops; anything else right away
            if (f->type == entity::FIELD_STRING) {
                ed->pending_entity = h;
                ed->pending_component = (i32)c;
            } else {
                d->after_read(w, m, h, comp);
            }
        }

        // the settings of the assets those paths name
        for (u32 i = 0; i < d->field_count; i++)
            import_section(ed, m, &d->fields[i], comp);

        ui::columns(1);
        remove = ui::button("Remove");
        ui::tree_pop();
    }
    ui::pop_id();

    if (remove) {
        undo::record_remove(ed->journal, h, c, comp);
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
    undo::record_field(ed->journal, h, UNDO_ENTITY, (u32)offsetof(entity::Entity, local),
                       sizeof(entity::Transform), &before, &e->local, false);
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
        undo::record_field(ed->journal, h, UNDO_ENTITY, (u32)offsetof(entity::Entity, name),
                           ENTITY_NAME_CAP, name_before, e->name, true);
    ui::columns(1);

    b32 active_before = e->active;
    if (ui::checkbox("active", &e->active))
        undo::record_field(ed->journal, h, UNDO_ENTITY, (u32)offsetof(entity::Entity, active),
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
            undo::record_add(ed->journal, h, (entity::Component)c, comp);
        }
        ui::tree_pop();
    }

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

        // something to see draws itself; anything else gets its axes
        if (!r || !r->visible)
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

    if (ed->selected.v && !entity::alive(w, ed->selected))
        ed->selected = entity::NONE;

    f32 aspect = (f32)fb_width / (f32)fb_height;

    fly(ed, in, win, dt);
    hierarchy_panel(ed, w, m, fb_height);
    inspector_panel(ed, w, m, fb_width, fb_height);

    // after the panels, so a click on one is not also a click in the world
    if (input::pressed(in, ed->select) && !ed->looking && !ui::wants_mouse()) {
        intersect::Ray ray =
            pick_ray(ed->view, ed->cfg.camera.fov, aspect, input::cursor(in), fb_width, fb_height);
        ed->selected = pick(w, m, ray, ed->cfg.marker_size, ed->cfg.camera.z_far);
    }

    // keys go to a focused field before they mean anything here
    if (!ui::wants_keyboard()) {
        if (input::pressed(in, ed->focus))
            focus_selection(ed, w, m);

        if (input::pressed(in, ed->delete_entity) && ed->selected.v) {
            entity::destroy(w, ed->selected);
            ed->selected = entity::NONE;
            forget_history(ed);
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
    if (!continuing())
        undo::close(ed->journal);

    draw_markers(ed, w, m, aspect);
}

} // namespace editor
