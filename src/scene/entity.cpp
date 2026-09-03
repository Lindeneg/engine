#include "scene/entity.h"

#include <math.h>
#include <string.h>

#include "asset/anim.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/pool.h"
#include "core/str.h"
#include "gfx/debugdraw.h"
#include "gfx/render.h"
#include "gfx/shader.h"
#include "platform/file.h"
#include "platform/path.h"

#define ENTITY_DIR "entities"
#define ENTITY_EXT ".entity"
#define LEVEL_DIR "levels"
#define LEVEL_EXT ".level"
#define FILE_EXT_CAP 16 // longest extension above, plus NUL

namespace entity {

#define DEFAULT_FIXED_HZ 120.0f
#define DEFAULT_MAX_FIXED_STEPS 8
#define DEFAULT_MAX_ENTITIES 4096
#define DEFAULT_MAX_COLLIDERS 256

// 0 means no id, so the counter starts above it
#define FIRST_ID 1u
#define DEFAULT_MAX_RENDERABLES 2048
#define DEFAULT_MAX_ANIMATORS 64
#define DEFAULT_MAX_CAMERAS 8
#define DEFAULT_MAX_LIGHTS 16

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .max_entities = (u32)config::get_i32(body, "max_entities", DEFAULT_MAX_ENTITIES),
        .max_renderables = (u32)config::get_i32(body, "max_renderables", DEFAULT_MAX_RENDERABLES),
        .max_animators = (u32)config::get_i32(body, "max_animators", DEFAULT_MAX_ANIMATORS),
        .max_cameras = (u32)config::get_i32(body, "max_cameras", DEFAULT_MAX_CAMERAS),
        .max_lights = (u32)config::get_i32(body, "max_lights", DEFAULT_MAX_LIGHTS),
        .max_colliders = (u32)config::get_i32(body, "max_colliders", DEFAULT_MAX_COLLIDERS),
        .fixed_hz = config::get_f32(body, "fixed_hz", DEFAULT_FIXED_HZ),
        .max_fixed_steps = (u32)config::get_i32(body, "max_fixed_steps", DEFAULT_MAX_FIXED_STEPS),
    };
}

// past this a hand-typed quaternion is reported as non-unit; it is normalized either way
#define UNIT_QUAT_TOLERANCE 1e-3f

// enough for anything a person types; a computed value may lose its last bits on a round trip
#define F32_WRITE_FMT "%.6g"

#define FIELD(T, member) (u32) offsetof(T, member)

static const Renderable RENDERABLE_DEFAULTS = {.visible = true};
static const FieldDesc RENDERABLE_FIELDS[] = {
    {.key = "model",
     .type = FIELD_STRING,
     .offset = FIELD(Renderable, model_path),
     .cap = ENTITY_PATH_CAP,
     .asset = ASSET_MODEL},
    {.key = "visible", .type = FIELD_B32, .offset = FIELD(Renderable, visible)},
};

static const Animator ANIMATOR_DEFAULTS = {.speed = 1.0f, .playing = true};
static const FieldDesc ANIMATOR_FIELDS[] = {
    {.key = "clip",
     .type = FIELD_STRING,
     .offset = FIELD(Animator, clip_path),
     .cap = ENTITY_PATH_CAP,
     .asset = ASSET_CLIP},
    {.key = "speed", .type = FIELD_F32, .offset = FIELD(Animator, speed)},
    {.key = "playing", .type = FIELD_B32, .offset = FIELD(Animator, playing)},
};

static const Camera CAMERA_DEFAULTS = {
    .fov_y = 60.0f, .z_near = 0.05f, .z_far = 500.0f, .active = true};
static const FieldDesc CAMERA_FIELDS[] = {
    {.key = "fov", .type = FIELD_F32, .offset = FIELD(Camera, fov_y)},
    {.key = "near", .type = FIELD_F32, .offset = FIELD(Camera, z_near)},
    {.key = "far", .type = FIELD_F32, .offset = FIELD(Camera, z_far)},
    {.key = "active", .type = FIELD_B32, .offset = FIELD(Camera, active)},
};

static const char *LIGHT_KIND_NAMES[LIGHT_KIND_COUNT] = {"directional"};
static_assert(sizeof(LightKind) == sizeof(i32), "FIELD_ENUM reads enums as i32");

static const Light LIGHT_DEFAULTS = {
    .kind = LIGHT_DIRECTIONAL,
    .color = {1.0f, 1.0f, 1.0f},
    .intensity = 0.72f,
    .ambient = 0.28f,
    .active = true,
};
static const FieldDesc LIGHT_FIELDS[] = {
    {.key = "kind",
     .type = FIELD_ENUM,
     .offset = FIELD(Light, kind),
     .names = LIGHT_KIND_NAMES,
     .name_count = LIGHT_KIND_COUNT},
    {.key = "color", .type = FIELD_VEC3, .offset = FIELD(Light, color)},
    {.key = "intensity", .type = FIELD_F32, .offset = FIELD(Light, intensity)},
    {.key = "ambient", .type = FIELD_F32, .offset = FIELD(Light, ambient)},
    {.key = "active", .type = FIELD_B32, .offset = FIELD(Light, active)},
};

static const char *COLLIDER_SHAPE_NAMES[COLLIDER_SHAPE_COUNT] = {"box", "sphere"};
static_assert(sizeof(ColliderShape) == sizeof(i32), "FIELD_ENUM reads enums as i32");

static const Collider COLLIDER_DEFAULTS = {
    .shape = COLLIDER_BOX,
    .size = {1.0f, 1.0f, 1.0f},
    .radius = 0.5f,
};
static const FieldDesc COLLIDER_FIELDS[] = {
    {.key = "shape",
     .type = FIELD_ENUM,
     .offset = FIELD(Collider, shape),
     .names = COLLIDER_SHAPE_NAMES,
     .name_count = COLLIDER_SHAPE_COUNT},
    {.key = "size", .type = FIELD_VEC3, .offset = FIELD(Collider, size)},
    {.key = "radius", .type = FIELD_F32, .offset = FIELD(Collider, radius)},
    {.key = "offset", .type = FIELD_VEC3, .offset = FIELD(Collider, offset)},
};

static void after_read_RENDERABLE(World *w, asset::Manager *m, Handle h, void *c);
static void after_read_ANIMATOR(World *w, asset::Manager *m, Handle h, void *c);
static void after_read_CAMERA(World *w, asset::Manager *m, Handle h, void *c);
static void after_read_LIGHT(World *w, asset::Manager *m, Handle h, void *c);
static void after_read_COLLIDER(World *w, asset::Manager *m, Handle h, void *c);

static const ComponentDesc DESCS[COMPONENT_COUNT] = {
#define X(ID, Type, key)                                                                           \
    {key, sizeof(Type), &ID##_DEFAULTS, ID##_FIELDS, ARRAY_COUNT(ID##_FIELDS), after_read_##ID},
    COMPONENT_LIST(X)
#undef X
};

const ComponentDesc *component_desc(Component c) {
    ASSERT(c < COMPONENT_COUNT);
    return &DESCS[c];
}

struct World {
    memory::Arena *arena;
    Config cfg;

    f32 fixed_dt;
    u32 max_fixed_steps;
    f32 accumulator; // unsimulated time carried between frames
    f32 alpha;       // where this frame sits between two fixed steps

    pool::Pool entities;
    pool::Pool components[COMPONENT_COUNT];

    u32 next_id; // ids only ever go up; a load moves this past what the file holds

    linalg::Mat4 *skin;   // per animator slot, slot * SHADER_MAX_JOINTS; sized once at init
    linalg::Mat4 *global; // same, joint model-space transforms for attachments
};

// every component's first field; add_component writes owner through this instead of a per-type
// switch
struct ComponentHeader {
    Handle owner;
};

#define X(ID, Type, key) static_assert(offsetof(Type, owner) == 0, #Type " must start with owner");
COMPONENT_LIST(X)
#undef X

WorldResult init(memory::Arena *a, Config cfg) {
    ASSERT(a);

    if (!cfg.max_entities) {
        LOG_ERR("entity: init needs a nonzero entity capacity");
        return WorldResult{.err = result::ERR_INVALID};
    }

    World *w = push_struct(a, World);
    w->arena = a;
    w->cfg = cfg;
    w->fixed_dt = 1.0f / (cfg.fixed_hz > 0.0f ? cfg.fixed_hz : DEFAULT_FIXED_HZ);
    w->max_fixed_steps = cfg.max_fixed_steps ? cfg.max_fixed_steps : DEFAULT_MAX_FIXED_STEPS;
    w->next_id = FIRST_ID;

    // +1: a pool reserves index 0 and there is no fallback entity to fill it,
    // so the configured count is the usable one
    w->entities = pool::create(a, cfg.max_entities + 1, sizeof(Entity));

    u32 caps[COMPONENT_COUNT] = {
        cfg.max_renderables + 1,
        cfg.max_animators + 1,
        cfg.max_cameras + 1,
        cfg.max_lights + 1,
        cfg.max_colliders + 1,
    };
    for (u32 c = 0; c < COMPONENT_COUNT; c++)
        w->components[c] = pool::create(a, caps[c], DESCS[c].size);

    // sized for the reserved slot 0 too, so a slot indexes the palette directly
    u64 palette_slots = (u64)caps[COMPONENT_ANIMATOR] * SHADER_MAX_JOINTS;
    w->skin = push_array(a, linalg::Mat4, palette_slots);
    w->global = push_array(a, linalg::Mat4, palette_slots);

    LOG_INFO("entity: world with %u entities, %u renderables, %u animators (%.2f MiB palettes)",
             cfg.max_entities, caps[COMPONENT_RENDERABLE], caps[COMPONENT_ANIMATOR],
             (f64)(palette_slots * sizeof(linalg::Mat4) * 2) / (1024.0 * 1024.0));
    LOG_INFO("entity: simulating at %.1f Hz, at most %u step(s) per frame",
             (f64)(1.0f / w->fixed_dt), w->max_fixed_steps);

    return WorldResult{.val = w};
}

Entity *get(World *w, Handle h) {
    ASSERT(w);
    return (Entity *)pool::resolve(&w->entities, h.v);
}

b32 alive(World *w, Handle h) {
    ASSERT(w);
    return pool::alive(&w->entities, h.v);
}

Handle spawn(World *w, const char *name) {
    ASSERT(w);

    u32 idx = pool::alloc(&w->entities, 0);
    if (!idx) {
        LOG_WARN("entity: world is full at %u entities, \"%s\" not spawned", w->cfg.max_entities,
                 name ? name : "<unnamed>");
        return NONE;
    }

    Entity *e = (Entity *)pool::at(&w->entities, idx);
    snprintf(e->name, ENTITY_NAME_CAP, "%s", name ? name : "<unnamed>");
    e->id = w->next_id++;
    e->local = transform_identity();
    e->world = linalg::mat4_identity(); // zeroed would be degenerate
    e->attach_joint_index = -1;
    e->active = true;

    ASSERT_MSG(w->next_id != 0, "entity: the id counter wrapped");
    return Handle{pool::make_handle(&w->entities, idx)};
}

// takes the id the file or the journal asks for, unless something alive has it
static void assign_id(World *w, Handle h, u32 id) {
    Entity *e = get(w, h);
    if (!id || !e)
        return;

    Handle holder = find_by_id(w, id);
    if (holder.v && holder.v != h.v) {
        LOG_WARN("entity: id %u is already taken by \"%s\"; \"%s\" keeps a fresh one", id,
                 get(w, holder)->name, e->name);
        return;
    }

    e->id = id;
    if (id >= w->next_id)
        w->next_id = id + 1;
}

Handle spawn_with_id(World *w, const char *name, u32 id) {
    Handle h = spawn(w, name);
    assign_id(w, h, id);
    return h;
}

u32 id_of(World *w, Handle h) {
    Entity *e = get(w, h);
    return e ? e->id : 0;
}

Handle find_by_id(World *w, u32 id) {
    ASSERT(w);
    if (!id)
        return NONE;

    for (Handle h = first(w); h.v; h = next(w, h))
        if (get(w, h)->id == id)
            return h;
    return NONE;
}

static void unlink(World *w, Handle h) {
    Entity *e = get(w, h);
    if (!e || e->parent.v == 0)
        return;

    Entity *p = get(w, e->parent);
    if (p) {
        if (p->first_child.v == h.v) {
            p->first_child = e->next_sibling;
        } else {
            for (Handle c = p->first_child; c.v;) {
                Entity *ce = get(w, c);
                if (!ce)
                    break;
                if (ce->next_sibling.v == h.v) {
                    ce->next_sibling = e->next_sibling;
                    break;
                }
                c = ce->next_sibling;
            }
        }
    }

    e->parent = NONE;
    e->next_sibling = NONE;
}

void destroy(World *w, Handle h) {
    ASSERT(w);

    Entity *e = get(w, h);
    if (!e)
        return;

    // snapshot next before recursing: destroy unlinks the child and clears it
    for (Handle c = e->first_child; c.v;) {
        Entity *ce = get(w, c);
        Handle nxt = ce ? ce->next_sibling : NONE;
        destroy(w, c);
        c = nxt;
    }

    unlink(w, h);

    for (u32 c = 0; c < COMPONENT_COUNT; c++)
        if (e->components[c])
            pool::release(&w->components[c], e->components[c]);

    pool::release(&w->entities, pool::handle_index(h.v));
}

static asset::SkeletonHandle skeleton_of(World *w, asset::Manager *m, Handle h) {
    Animator *a = animator(w, h);
    if (a && a->skeleton.v)
        return a->skeleton;

    Renderable *r = renderable(w, h);
    if (r)
        return asset::get(m, r->model)->skeleton;

    return asset::SKELETON_FALLBACK;
}

static b32 descends_from(World *w, Handle h, Handle ancestor) {
    for (Entity *e = get(w, h); e && e->parent.v; e = get(w, e->parent))
        if (e->parent.v == ancestor.v)
            return true;
    return false;
}

b32 set_parent(World *w, asset::Manager *m, Handle child, Handle parent, const char *joint) {
    ASSERT(w && m);

    Entity *ce = get(w, child);
    if (!ce) {
        LOG_WARN("entity: set_parent on a dead child handle %u", child.v);
        return false;
    }

    // every refusal precedes the unlink, so a rejected reparent leaves the child where it was
    Entity *pe = NULL;
    if (parent.v) {
        pe = get(w, parent);
        if (!pe) {
            LOG_WARN("entity: \"%s\" parented to a dead handle %u", ce->name, parent.v);
            return false;
        }
        // a cycle never terminates in resolve_world or destroy
        if (parent.v == child.v || descends_from(w, parent, child)) {
            LOG_WARN("entity: \"%s\" cannot be parented under its own subtree", ce->name);
            return false;
        }
    }

    unlink(w, child);

    ce->attach_joint[0] = 0;
    ce->attach_joint_index = -1;

    if (!pe)
        return true;

    // appended, not prepended: sibling order is file order and a save must reproduce it
    ce->parent = parent;
    ce->next_sibling = NONE;
    if (!pe->first_child.v) {
        pe->first_child = child;
    } else {
        Entity *last = get(w, pe->first_child);
        while (last && last->next_sibling.v)
            last = get(w, last->next_sibling);
        if (last)
            last->next_sibling = child;
    }

    if (!joint || !joint[0])
        return true;

    // skeletons store joint names canonicalized
    str::canonical_name(ce->attach_joint, ENTITY_NAME_CAP, joint);

    // resolved once; an unknown name stays -1 and degrades to plain parenting
    asset::Skeleton *skel = asset::get(m, skeleton_of(w, m, parent));
    u32 *found = strmap::find(&skel->by_name, ce->attach_joint);
    if (!found) {
        LOG_WARN("entity: \"%s\" attaches to joint \"%s\", which \"%s\" does not have", ce->name,
                 ce->attach_joint, pe->name);
        return true;
    }

    ce->attach_joint_index = (i32)*found;

    if (!animator(w, parent))
        LOG_WARN("entity: \"%s\" attaches to joint \"%s\" of \"%s\", which has no animator to "
                 "pose it",
                 ce->name, ce->attach_joint, pe->name);
    return true;
}

void set_world_position(World *w, Handle h, linalg::Vec3 world) {
    ASSERT(w);

    Entity *e = get(w, h);
    if (!e)
        return;

    Entity *p = get(w, e->parent);
    if (!p) {
        e->local.position = world;
        return;
    }

    // a joint attachment sits under the joint, not the parent's origin, and
    // that matrix is rebuilt per frame; the parent's own frame is the honest answer
    e->local.position = linalg::mat4_mul_point(linalg::mat4_inverse_affine(p->world), world);
}

void *add_component(World *w, Handle h, Component type) {
    ASSERT(w && type < COMPONENT_COUNT);

    Entity *e = get(w, h);
    if (!e) {
        LOG_WARN("entity: add_component on a dead handle %u", h.v);
        return NULL;
    }

    if (e->components[type])
        return pool::at(&w->components[type], e->components[type]);

    u32 idx = pool::alloc(&w->components[type], 0);
    if (!idx) {
        LOG_WARN("entity: component pool %u is full, \"%s\" goes without", type, e->name);
        return NULL;
    }

    e->components[type] = idx;

    void *c = pool::at(&w->components[type], idx);
    memcpy(c, DESCS[type].defaults, DESCS[type].size);
    ((ComponentHeader *)c)->owner = h;
    return c;
}

void remove_component(World *w, Handle h, Component type) {
    ASSERT(w && type < COMPONENT_COUNT);

    Entity *e = get(w, h);
    if (!e || !e->components[type])
        return;

    pool::release(&w->components[type], e->components[type]);
    e->components[type] = 0;
    e->overridden &= ~(1u << type);
}

static void *component_of(World *w, Handle h, u32 type) {
    Entity *e = get(w, h);
    if (!e || !e->components[type])
        return NULL;
    return pool::at(&w->components[type], e->components[type]);
}

Renderable *add_renderable(World *w, Handle h) {
    return (Renderable *)add_component(w, h, COMPONENT_RENDERABLE);
}

Animator *add_animator(World *w, Handle h) {
    return (Animator *)add_component(w, h, COMPONENT_ANIMATOR);
}

Camera *add_camera(World *w, Handle h) { return (Camera *)add_component(w, h, COMPONENT_CAMERA); }

Light *add_light(World *w, Handle h) { return (Light *)add_component(w, h, COMPONENT_LIGHT); }

Collider *add_collider(World *w, Handle h) {
    return (Collider *)add_component(w, h, COMPONENT_COLLIDER);
}

u32 component_first(World *w, Component c) {
    ASSERT(w && c < COMPONENT_COUNT);
    return pool::first(&w->components[c]);
}

u32 component_next(World *w, Component c, u32 slot) {
    ASSERT(w && c < COMPONENT_COUNT);
    return pool::next(&w->components[c], slot);
}

void *component_at(World *w, Component c, u32 slot) {
    ASSERT(w && c < COMPONENT_COUNT);
    return slot ? pool::at(&w->components[c], slot) : NULL;
}

void *component(World *w, Handle h, Component c) {
    ASSERT(w && c < COMPONENT_COUNT);
    return component_of(w, h, c);
}

Renderable *renderable(World *w, Handle h) {
    return (Renderable *)component_of(w, h, COMPONENT_RENDERABLE);
}

Animator *animator(World *w, Handle h) {
    return (Animator *)component_of(w, h, COMPONENT_ANIMATOR);
}

Camera *camera(World *w, Handle h) { return (Camera *)component_of(w, h, COMPONENT_CAMERA); }

Light *light(World *w, Handle h) { return (Light *)component_of(w, h, COMPONENT_LIGHT); }

Collider *collider(World *w, Handle h) {
    return (Collider *)component_of(w, h, COMPONENT_COLLIDER);
}

linalg::Mat4 collider_frame(World *w, const Collider *c) {
    ASSERT(w && c);

    Entity *e = get(w, c->owner);
    if (!e)
        return linalg::mat4_from_translation(c->offset);

    // the owner's basis with the scale divided out; a flattened axis keeps the world's
    static const linalg::Vec3 WORLD_AXES[3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    linalg::Mat4 frame = linalg::mat4_identity();
    for (u32 i = 0; i < 3; i++) {
        linalg::Vec3 axis =
            linalg::vec3_normalize_or(linalg::mat4_axis(e->world, i), WORLD_AXES[i]);
        frame.m[i * 4 + 0] = axis.x;
        frame.m[i * 4 + 1] = axis.y;
        frame.m[i * 4 + 2] = axis.z;
    }

    // the offset is in the owner's frame, so it turns with it but does not scale
    linalg::Vec3 origin =
        linalg::mat4_translation(e->world) + linalg::mat4_mul_dir(frame, c->offset);
    frame.m[12] = origin.x;
    frame.m[13] = origin.y;
    frame.m[14] = origin.z;
    return frame;
}

void draw_colliders(World *w, linalg::Vec4 color) {
    ASSERT(w);

    pool::Pool *p = &w->components[COMPONENT_COLLIDER];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Collider *c = (Collider *)pool::at(p, i);
        Entity *e = get(w, c->owner);
        if (!e || !e->active)
            continue;

        linalg::Mat4 frame = collider_frame(w, c);
        if (c->shape == COLLIDER_SPHERE) {
            debugdraw::sphere(linalg::mat4_translation(frame), c->radius, color);
        } else {
            linalg::Vec3 half = c->size * 0.5f;
            debugdraw::box(frame, -1.0f * half, half, color);
        }
    }
}

Handle active_camera(World *w) {
    ASSERT(w);

    pool::Pool *p = &w->components[COMPONENT_CAMERA];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Camera *c = (Camera *)pool::at(p, i);
        if (c->active && alive(w, c->owner))
            return c->owner;
    }
    return NONE;
}

b32 build_frame(World *w, f32 aspect, shader::FrameData *out) {
    ASSERT(w && out);

    Handle h = active_camera(w);
    Entity *e = get(w, h);
    if (!e)
        return false;

    return build_frame(w, e->world, camera(w, h), aspect, out);
}

b32 build_frame(World *w, linalg::Mat4 camera_world, const Camera *c, f32 aspect,
                shader::FrameData *out) {
    ASSERT(w && c && out);

    out->view_proj = linalg::mat4_perspective(DEG2RAD(c->fov_y), aspect, c->z_near, c->z_far) *
                     linalg::mat4_view_from_world(camera_world);
    out->camera_pos = linalg::vec4_from(linalg::mat4_translation(camera_world), 0.0f);

    // no light is legitimate (a level being built) and reads as black, not as a shader failure
    out->light_dir = linalg::Vec4{0.0f, 1.0f, 0.0f, 0.0f};
    out->light_color = linalg::Vec4{0.0f, 0.0f, 0.0f, 0.0f};

    pool::Pool *p = &w->components[COMPONENT_LIGHT];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Light *l = (Light *)pool::at(p, i);
        Entity *le = get(w, l->owner);
        if (!l->active || !le)
            continue;

        // forward is -Z, so "toward the light" is the entity's +Z axis; a
        // zero-scaled entity has no forward and falls back to overhead
        linalg::Vec3 toward = linalg::vec3_normalize_or(linalg::mat4_axis(le->world, 2),
                                                        linalg::Vec3{0.0f, 1.0f, 0.0f});
        out->light_dir = linalg::Vec4{toward.x, toward.y, toward.z, 0.0f};
        out->light_color = linalg::Vec4{l->color.x * l->intensity, l->color.y * l->intensity,
                                        l->color.z * l->intensity, l->ambient};
        break; // one directional light until the shader can loop over more
    }

    return true;
}

static linalg::Mat4 *skin_at(World *w, u32 slot) { return w->skin + (u64)slot * SHADER_MAX_JOINTS; }
static linalg::Mat4 *global_at(World *w, u32 slot) {
    return w->global + (u64)slot * SHADER_MAX_JOINTS;
}

const linalg::Mat4 *palette(World *w, Handle h, u32 *out_count) {
    ASSERT(w);

    Entity *e = get(w, h);
    if (!e || !e->components[COMPONENT_ANIMATOR])
        return NULL;

    u32 slot = e->components[COMPONENT_ANIMATOR];
    Animator *a = (Animator *)pool::at(&w->components[COMPONENT_ANIMATOR], slot);
    if (a->joint_count == 0)
        return NULL;

    if (out_count)
        *out_count = a->joint_count;
    return skin_at(w, slot);
}

Handle first(World *w) {
    ASSERT(w);
    u32 idx = pool::first(&w->entities);
    return idx ? Handle{pool::make_handle(&w->entities, idx)} : NONE;
}

Handle next(World *w, Handle h) {
    ASSERT(w);
    u32 idx = pool::next(&w->entities, pool::handle_index(h.v));
    return idx ? Handle{pool::make_handle(&w->entities, idx)} : NONE;
}

static void advance_animator(World *w, asset::Manager *m, memory::Arena *scratch, Animator *a,
                             u32 slot, f32 dt) {
    asset::Skeleton *skel = asset::get(m, a->skeleton);
    a->joint_count = MIN(skel->joint_count, (u32)SHADER_MAX_JOINTS);
    if (a->joint_count == 0)
        return;

    if (a->playing)
        a->time += dt * a->speed;

    asset::Clip *clip = a->clip.v ? asset::get(m, a->clip) : NULL;
    anim::sample(scratch, skel, clip, a->time, skin_at(w, slot), global_at(w, slot));
}

// recursive from a root, so a child reads a parent already resolved this frame
static void resolve_world(World *w, asset::Manager *m, Handle h) {
    Entity *e = get(w, h);
    if (!e)
        return;

    linalg::Mat4 local =
        linalg::mat4_from_trs(e->local.position, e->local.rotation, e->local.scale);
    Entity *pe = get(w, e->parent);

    if (!pe) {
        e->world = local;
    } else if (e->attach_joint_index >= 0) {
        u32 slot = pe->components[COMPONENT_ANIMATOR];
        Animator *pa = slot ? (Animator *)pool::at(&w->components[COMPONENT_ANIMATOR], slot) : NULL;

        // the parent may have an animator but no renderable to take an import transform from
        Renderable *pr = renderable(w, e->parent);

        if (pa && pr && (u32)e->attach_joint_index < pa->joint_count) {
            // model-space joint, not the skinning matrix, which carries the inverse bind
            linalg::Mat4 joint = global_at(w, slot)[e->attach_joint_index];

            // the joint is in the parent's native space, so it passes through the parent's import
            // too
            asset::Model *pm = asset::get(m, pr->model);
            e->world = pe->world * pm->import * joint * local;
        } else {
            e->world = pe->world * local;
        }
    } else {
        e->world = pe->world * local;
    }

    for (Handle c = e->first_child; c.v;) {
        Entity *ce = get(w, c);
        if (!ce)
            break;
        resolve_world(w, m, c);
        c = ce->next_sibling;
    }
}

u32 begin_frame(World *w, f32 dt) {
    ASSERT(w);

    // a clock that stepped backwards must not strand the accumulator
    if (dt < 0.0f)
        dt = 0.0f;

    w->accumulator += dt;

    u32 steps = 0;
    while (w->accumulator >= w->fixed_dt && steps < w->max_fixed_steps) {
        w->accumulator -= w->fixed_dt;
        steps++;
    }

    // over the cap the backlog is dropped, not carried: a hitch is survivable, the spiral is not
    if (w->accumulator >= w->fixed_dt) {
        LOG_WARN_ONCE("entity: could not keep up at %.1f Hz, dropping %.1f ms of simulation",
                      (f64)(1.0f / w->fixed_dt), (f64)(w->accumulator * 1000.0f));
        w->accumulator = 0.0f;
    }

    w->alpha = w->accumulator / w->fixed_dt;
    return steps;
}

// everything of the engine's own whose correctness depends on the timestep;
// empty until there is engine-side physics to run here
void step_fixed(World *w, asset::Manager *m) { ASSERT(w && m); }

void end_frame(World *w, asset::Manager *m, memory::Arena *scratch, f32 dt) {
    ASSERT(w && m && scratch);

    // visual only, at frame time. Animation samples here for smoothness; hit
    // tests against bones will need it sampled on the fixed clock instead.
    pool::Pool *p = &w->components[COMPONENT_ANIMATOR];
    for (u32 i = pool::first(p); i; i = pool::next(p, i))
        advance_animator(w, m, scratch, (Animator *)pool::at(p, i), i, dt);

    // after both clocks, since either can move an entity
    for (Handle h = first(w); h.v; h = next(w, h))
        if (get(w, h)->parent.v == 0)
            resolve_world(w, m, h);
}

f32 fixed_alpha(World *w) {
    ASSERT(w);
    return w->alpha;
}

f32 fixed_dt(World *w) {
    ASSERT(w);
    return w->fixed_dt;
}

void submit(World *w, asset::Manager *m) {
    ASSERT(w && m);

    pool::Pool *p = &w->components[COMPONENT_RENDERABLE];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Renderable *r = (Renderable *)pool::at(p, i);
        if (!r->visible)
            continue;

        Entity *e = get(w, r->owner);
        if (!e || !e->active)
            continue;

        u32 joint_count = 0;
        const linalg::Mat4 *pal = palette(w, r->owner, &joint_count);
        render::submit_model(m, r->model, e->world, pal, joint_count);
    }
}

void draw_bounds(World *w, asset::Manager *m, linalg::Vec4 color) {
    ASSERT(w && m);

    pool::Pool *p = &w->components[COMPONENT_RENDERABLE];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Renderable *r = (Renderable *)pool::at(p, i);
        if (!r->visible)
            continue;

        Entity *e = get(w, r->owner);
        if (!e || !e->active)
            continue;

        // the same space submit draws in: bounds carry the import, world the placement
        linalg::Vec3 lo, hi;
        asset::model_bounds(m, r->model, &lo, &hi);
        debugdraw::box(e->world, lo, hi, color);
    }
}

static void after_read_RENDERABLE(World *w, asset::Manager *m, Handle h, void *c) {
    Renderable *r = (Renderable *)c;

    if (!r->model_path[0]) {
        LOG_WARN("entity: a renderable has no model, drawing the fallback");
        r->model = asset::MODEL_FALLBACK;
    } else {
        r->model = OR_ELSE(asset::load_model(m, asset::SCOPE_PERSISTENT, r->model_path),
                           asset::MODEL_FALLBACK);
    }

    // the skeleton is the model's, so an animator already here binds again;
    // from a file the animator is read after this and does not exist yet
    Animator *a = animator(w, h);
    if (a)
        after_read_ANIMATOR(w, m, h, a);
}

static void after_read_ANIMATOR(World *w, asset::Manager *m, Handle h, void *c) {
    Animator *a = (Animator *)c;

    Renderable *r = renderable(w, h);
    if (!r) {
        LOG_WARN("entity: \"%s\" has an animator but no renderable to take a skeleton from",
                 get(w, h)->name);
        return;
    }

    a->skeleton = asset::get(m, r->model)->skeleton;
    if (a->clip_path[0])
        a->clip = OR_ELSE(asset::load_clip(m, asset::SCOPE_PERSISTENT, a->clip_path, a->skeleton),
                          asset::CLIP_FALLBACK);
}

static void after_read_CAMERA(World *, asset::Manager *, Handle, void *c) {
    Camera *cam = (Camera *)c;
    if (cam->z_near > 0.0f && cam->z_far > cam->z_near)
        return;

    LOG_WARN("entity: camera near/far of %.3f/%.3f is not a valid range, using %.3f/%.1f",
             (f64)cam->z_near, (f64)cam->z_far, (f64)CAMERA_DEFAULTS.z_near,
             (f64)CAMERA_DEFAULTS.z_far);
    cam->z_near = CAMERA_DEFAULTS.z_near;
    cam->z_far = CAMERA_DEFAULTS.z_far;
}

static void after_read_LIGHT(World *, asset::Manager *, Handle, void *) {}

// a shape with no extent is a hole in the world, not a wall; it is put back to the default
static void after_read_COLLIDER(World *w, asset::Manager *, Handle h, void *c) {
    Collider *col = (Collider *)c;
    const char *name = get(w, h)->name;

    if (col->size.x <= 0.0f || col->size.y <= 0.0f || col->size.z <= 0.0f) {
        LOG_WARN("entity: \"%s\" has a collider of size %.3f %.3f %.3f; using the default", name,
                 (f64)col->size.x, (f64)col->size.y, (f64)col->size.z);
        col->size = COLLIDER_DEFAULTS.size;
    }
    if (col->radius <= 0.0f) {
        LOG_WARN("entity: \"%s\" has a collider of radius %.3f; using the default", name,
                 (f64)col->radius);
        col->radius = COLLIDER_DEFAULTS.radius;
    }
}

// absent keys keep what is there: the default for a fresh component, the definition's value for an
// override
static void read_fields(const ComponentDesc *d, config::Node *body, void *c) {
    for (u32 i = 0; i < d->field_count; i++) {
        const FieldDesc *f = &d->fields[i];
        config::Node *n = config::find(body, f->key);
        if (!n)
            continue;

        u8 *at = (u8 *)c + f->offset;
        switch (f->type) {
        case FIELD_F32:
            *(f32 *)at = config::value_f32(n, 0, *(f32 *)at);
            break;
        case FIELD_I32:
            *(i32 *)at = config::value_i32(n, 0, *(i32 *)at);
            break;
        case FIELD_B32:
            *(b32 *)at = config::value_b32(n, 0, *(b32 *)at);
            break;
        case FIELD_VEC3:
            *(linalg::Vec3 *)at = config::value_vec3(n, *(linalg::Vec3 *)at);
            break;
        case FIELD_STRING: {
            const char *v = config::value(n, 0, "");
            i32 len = snprintf((char *)at, f->cap, "%s", v);
            if (len < 0 || (u32)len >= f->cap)
                LOG_WARN("entity: %u: '%s' is longer than the %u bytes %s allows", n->line, v,
                         f->cap, f->key);
            break;
        }
        case FIELD_ENUM: {
            const char *v = config::value(n, 0, "");
            i32 idx = str::name_index(f->names, f->name_count, v);
            if (idx < 0)
                LOG_WARN("entity: %u: '%s' is not a known %s", n->line, v, f->key);
            else
                *(i32 *)at = idx;
            break;
        }
        }
    }
}

static const char *fmt_f32(memory::Arena *a, f32 v) {
    return memory::push_fmt(a, F32_WRITE_FMT, (f64)v);
}

static void write_fields(memory::Arena *a, config::Doc *doc, config::Node *block,
                         const ComponentDesc *d, const void *c) {
    for (u32 i = 0; i < d->field_count; i++) {
        const FieldDesc *f = &d->fields[i];
        const u8 *at = (const u8 *)c + f->offset;
        const char *vals[3];
        u32 n = 1;

        switch (f->type) {
        case FIELD_F32:
            vals[0] = fmt_f32(a, *(const f32 *)at);
            break;
        case FIELD_I32:
            vals[0] = memory::push_fmt(a, "%d", *(const i32 *)at);
            break;
        case FIELD_B32:
            vals[0] = *(const b32 *)at ? "true" : "false";
            break;
        case FIELD_VEC3: {
            const linalg::Vec3 *v = (const linalg::Vec3 *)at;
            vals[0] = fmt_f32(a, v->x);
            vals[1] = fmt_f32(a, v->y);
            vals[2] = fmt_f32(a, v->z);
            n = 3;
            break;
        }
        case FIELD_STRING:
            if (!*(const char *)at)
                continue; // never set
            vals[0] = (const char *)at;
            break;
        case FIELD_ENUM: {
            u32 idx = (u32) * (const i32 *)at;
            vals[0] = idx < f->name_count ? f->names[idx] : f->names[0];
            break;
        }
        }

        config::append(a, doc, block, f->key, vals, n);
    }
}

static void read_transform(config::Node *n, Transform *out) {
    if (!n)
        return;

    config::Node *body = n->first_child;

    out->position = config::value_vec3(config::find(body, "position"), out->position);

    // 3 values: Euler degrees. 4: a raw quaternion, for exact round-trips.
    config::Node *r = config::find(body, "rotation");
    if (r && r->value_count == 3) {
        set_euler(out, config::value_vec3(r, linalg::Vec3{}));
    } else if (r) {
        linalg::Quat q{
            config::value_f32(r, 0, out->rotation.x),
            config::value_f32(r, 1, out->rotation.y),
            config::value_f32(r, 2, out->rotation.z),
            config::value_f32(r, 3, out->rotation.w),
        };

        // mat4_from_quat assumes unit length; a non-unit quaternion yields a
        // sheared, non-orthogonal matrix, not a scaled rotation
        if (r->value_count != 4)
            LOG_WARN("entity: rotation has %u value(s); 3 reads as Euler degrees, 4 as a "
                     "quaternion, anything else is a typo",
                     r->value_count);

        f32 len = sqrtf(linalg::quat_dot(q, q));
        if (len < LINALG_ZERO_EPSILON) {
            LOG_WARN("entity: rotation is a zero quaternion, ignored");
        } else {
            if (len < 1.0f - UNIT_QUAT_TOLERANCE || len > 1.0f + UNIT_QUAT_TOLERANCE)
                LOG_WARN("entity: rotation is not a unit quaternion (length %.3f); normalized. "
                         "Three values would be read as Euler degrees instead.",
                         (f64)len);
            set_rotation(out, linalg::quat_normalize(q));
        }
    }

    config::Node *s = config::find(body, "scale");
    if (s) {
        // one value is uniform scale
        f32 sx = config::value_f32(s, 0, out->scale.x);
        out->scale = linalg::Vec3{
            sx,
            config::value_f32(s, 1, s->value_count > 1 ? out->scale.y : sx),
            config::value_f32(s, 2, s->value_count > 2 ? out->scale.z : sx),
        };
    }
}

static void write_transform(memory::Arena *a, config::Doc *doc, config::Node *parent,
                            const Transform *t) {
    config::Node *block = config::append(a, doc, parent, "transform", NULL, 0);
    block->is_block = true;

    const char *pos[] = {fmt_f32(a, t->position.x), fmt_f32(a, t->position.y),
                         fmt_f32(a, t->position.z)};
    config::append(a, doc, block, "position", pos, 3);

    if (t->use_euler) {
        const char *rot[] = {fmt_f32(a, t->euler.x), fmt_f32(a, t->euler.y),
                             fmt_f32(a, t->euler.z)};
        config::append(a, doc, block, "rotation", rot, 3);
    } else {
        const char *rot[] = {fmt_f32(a, t->rotation.x), fmt_f32(a, t->rotation.y),
                             fmt_f32(a, t->rotation.z), fmt_f32(a, t->rotation.w)};
        config::append(a, doc, block, "rotation", rot, 4);
    }

    // one value when uniform
    b32 uniform = t->scale.x == t->scale.y && t->scale.y == t->scale.z;
    const char *scl[] = {fmt_f32(a, t->scale.x), fmt_f32(a, t->scale.y), fmt_f32(a, t->scale.z)};
    config::append(a, doc, block, "scale", scl, uniform ? 1 : 3);
}

// bounds a definition that references itself, directly or through a chain
#define MAX_DEFINITION_DEPTH 8

static Handle build_entity(World *w, asset::Manager *m, config::Node *decl, Handle parent,
                           u32 depth);
static u32 assign_owned_ids(World *w, Handle h, config::Node *ids, u32 n);

static void build_children(World *w, asset::Manager *m, config::Node *body, Handle self,
                           u32 depth) {
    for (config::Node *c = config::find(body, "child"); c; c = config::find_next(c, "child"))
        build_entity(w, m, c, self, depth);
}

// returns a bit per component block present: the override mask for a definition reference
static u32 read_components(World *w, asset::Manager *m, Handle h, config::Node *body) {
    u32 present = 0;
    for (u32 c = 0; c < COMPONENT_COUNT; c++) {
        config::Node *block = config::find(body, DESCS[c].key);
        if (!block)
            continue;

        void *comp = add_component(w, h, (Component)c);
        if (!comp)
            continue;

        read_fields(&DESCS[c], block->first_child, comp);
        if (DESCS[c].after_read)
            DESCS[c].after_read(w, m, h, comp);
        present |= 1u << c;
    }
    return present;
}

static void mark_owned(World *w, Handle h, b32 owned) {
    for (Handle c = get(w, h)->first_child; c.v; c = get(w, c)->next_sibling) {
        get(w, c)->owned_by_definition = owned;
        mark_owned(w, c, owned);
    }
}

// refuses to truncate: a clipped name would report NOT_FOUND against a file nobody asked for
static b32 data_file_name(char *out, u64 cap, const char *name, const char *ext) {
    i32 n = snprintf(out, cap, "%s%s", name, ext);
    if (n < 0 || (u64)n >= cap) {
        LOG_WARN("entity: name '%s' is longer than the %d bytes allowed", name, ENTITY_NAME_CAP);
        return false;
    }
    return true;
}

// parses <root>/<dir>/<name><ext> into scratch; the caller owns the temp scope
static config::DocResult load_data_file(asset::Manager *m, memory::Arena *scratch, const char *dir,
                                        const char *name, const char *ext) {
    char file[ENTITY_NAME_CAP + FILE_EXT_CAP];
    if (!data_file_name(file, sizeof(file), name, ext))
        return config::DocResult{.err = result::ERR_INVALID};
    return config::load(scratch, FPATH(scratch, asset::root(m), dir, file));
}

static HandleResult instantiate(World *w, asset::Manager *m, const char *name, u32 depth) {
    if (depth >= MAX_DEFINITION_DEPTH) {
        LOG_WARN("entity: definition '%s' nested deeper than %d -- a cycle?", name,
                 MAX_DEFINITION_DEPTH);
        return HandleResult{.err = result::ERR_INVALID};
    }

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    config::DocResult doc = load_data_file(m, scratch, ENTITY_DIR, name, ENTITY_EXT);
    if (doc.err != result::OK) {
        memory::temp_end(t);
        return HandleResult{.err = doc.err};
    }

    config::Node *decl = config::find(doc.val->first, "entity");
    if (!decl) {
        LOG_WARN("entity: %s%s has no 'entity' block", name, ENTITY_EXT);
        memory::temp_end(t);
        return HandleResult{.err = result::ERR_PARSE};
    }

    // parented by the caller: the attach joint lives in the referencing block
    Handle h = build_entity(w, m, decl, NONE, depth);
    memory::temp_end(t);

    if (!h.v)
        return HandleResult{.err = result::ERR_EXHAUSTED};

    attach_definition(w, h, name);
    return HandleResult{.val = h};
}

void attach_definition(World *w, Handle h, const char *name) {
    ASSERT(w && name);
    Entity *e = get(w, h);
    if (!e)
        return;
    snprintf(e->definition, ENTITY_NAME_CAP, "%s", name);
    e->overridden = 0;
    mark_owned(w, h, true);
}

void detach_definition(World *w, Handle h) {
    ASSERT(w);
    Entity *e = get(w, h);
    if (!e)
        return;
    e->definition[0] = 0;
    e->overridden = 0;
    mark_owned(w, h, false);
}

// one "entity", "child" or "instance" block: either a definition reference with
// overrides, or inline components
static Handle build_entity(World *w, asset::Manager *m, config::Node *decl, Handle parent,
                           u32 depth) {
    config::Node *body = decl->first_child;
    const char *label = config::value(decl, 0, NULL);
    const char *def = config::get(body, "definition", NULL);

    Handle h = NONE;
    if (def) {
        // parsed fresh per reference: an editor rewrites these files while the game runs
        HandleResult r = instantiate(w, m, def, depth + 1);
        if (r.err != result::OK)
            return NONE;
        h = r.val;
    } else {
        h = spawn(w, label ? label : "<unnamed>");
        if (!h.v)
            return NONE;
    }

    // ids first: a level names its own, and for a reference the ones its definition spawned
    assign_id(w, h, (u32)config::get_i32(body, "id", 0));
    config::Node *owned = def ? config::find(body, "owned_ids") : NULL;
    if (owned)
        assign_owned_ids(w, h, owned, 0);

    // for a reference, the components present are overrides; remembered so a save writes only them
    Entity *e = get(w, h);
    u32 present = read_components(w, m, h, body);
    if (def)
        e->overridden |= present;

    if (def && label)
        snprintf(e->name, ENTITY_NAME_CAP, "%s", label);
    e->active = config::get_b32(body, "active", e->active);
    read_transform(config::find(body, "transform"), &e->local);

    // after the components: attaching resolves a joint against the parent's skeleton
    if (parent.v)
        set_parent(w, m, h, parent, config::get(body, "attach_joint", NULL));

    build_children(w, m, body, h, depth);
    return h;
}

Handle spawn_from_config(World *w, asset::Manager *m, config::Node *decl, Handle parent) {
    ASSERT(w && m && decl);
    return build_entity(w, m, decl, parent, 0);
}

// how many definition-spawned descendants one instance can name; a warning past it
#define OWNED_IDS_CAP CONFIG_MAX_TOKENS

// depth first in sibling order, the order instantiate spawned them
static u32 collect_owned_ids(memory::Arena *a, World *w, Handle h, const char **out, u32 n) {
    for (Handle c = get(w, h)->first_child; c.v; c = get(w, c)->next_sibling) {
        Entity *ce = get(w, c);
        if (!ce->owned_by_definition)
            continue;
        if (n == OWNED_IDS_CAP) {
            LOG_WARN_ONCE("entity: \"%s\" has more than %d definition children; the rest get "
                          "fresh ids on load",
                          get(w, h)->name, OWNED_IDS_CAP);
            return n;
        }
        out[n++] = memory::push_fmt(a, "%u", ce->id);
        n = collect_owned_ids(a, w, c, out, n);
    }
    return n;
}

// the same walk, handing the ids back
static u32 assign_owned_ids(World *w, Handle h, config::Node *ids, u32 n) {
    for (Handle c = get(w, h)->first_child; c.v; c = get(w, c)->next_sibling) {
        if (!get(w, c)->owned_by_definition)
            continue;
        if (n < ids->value_count)
            assign_id(w, c, (u32)config::value_i32(ids, n, 0));
        n++;
        n = assign_owned_ids(w, c, ids, n);
    }
    return n;
}

void write_entity(memory::Arena *a, config::Doc *doc, config::Node *parent, World *w, Handle h,
                  const char *key, b32 as_definition) {
    ASSERT(a && doc && w && key);

    Entity *e = get(w, h);
    if (!e)
        return;

    const char *label[] = {e->name};
    config::Node *node = config::append(a, doc, parent, key, label, 1);
    node->is_block = true;

    // identity is an instance's, never a definition's: a template is shared
    if (!as_definition) {
        const char *id[] = {memory::push_fmt(a, "%u", e->id)};
        config::append(a, doc, node, "id", id, 1);
    }

    b32 by_ref = e->definition[0] != 0;
    if (by_ref) {
        const char *def[] = {e->definition};
        config::append(a, doc, node, "definition", def, 1);

        // the children the definition spawned are not written, so their ids
        // are, in spawn order, and the reader hands them back the same way
        const char *ids[OWNED_IDS_CAP];
        u32 n = as_definition ? 0 : collect_owned_ids(a, w, h, ids, 0);
        if (n)
            config::append(a, doc, node, "owned_ids", ids, n);
    }
    if (parent && e->attach_joint[0]) {
        const char *joint[] = {e->attach_joint};
        config::append(a, doc, node, "attach_joint", joint, 1);
    }
    if (!e->active) {
        const char *off[] = {"false"};
        config::append(a, doc, node, "active", off, 1);
    }

    write_transform(a, doc, node, &e->local);

    for (u32 c = 0; c < COMPONENT_COUNT; c++) {
        if (!e->components[c])
            continue;
        if (by_ref && !(e->overridden & (1u << c)))
            continue;

        config::Node *block = config::append(a, doc, node, DESCS[c].key, NULL, 0);
        block->is_block = true;
        write_fields(a, doc, block, &DESCS[c], pool::at(&w->components[c], e->components[c]));
    }

    for (Handle c = e->first_child; c.v; c = get(w, c)->next_sibling) {
        if (by_ref && get(w, c)->owned_by_definition)
            continue;
        write_entity(a, doc, node, w, c, "child", false);
    }
}

HandleResult spawn_definition(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);
    return instantiate(w, m, name, 0);
}

// NULL when the name does not fit
static const char *data_file_path(asset::Manager *m, memory::Arena *scratch, const char *dir,
                                  const char *name, const char *ext) {
    char file[ENTITY_NAME_CAP + FILE_EXT_CAP];
    if (!data_file_name(file, sizeof(file), name, ext))
        return NULL;
    return FPATH(scratch, asset::root(m), dir, file);
}

result::Error save_definition(World *w, asset::Manager *m, Handle h, const char *name) {
    ASSERT(w && m && name);

    Entity *e = get(w, h);
    if (!e)
        return result::ERR_INVALID;

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    const char *abs = data_file_path(m, scratch, ENTITY_DIR, name, ENTITY_EXT);
    if (!abs) {
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    // written detached while it would otherwise reference the file being written
    b32 self = strcmp(e->definition, name) == 0;
    u32 overridden = e->overridden;
    if (self)
        detach_definition(w, h);

    config::Doc *doc = config::create(scratch, abs);
    write_entity(scratch, doc, NULL, w, h, "entity", true);
    result::Error err = config::save(scratch, doc, abs);

    if (self) {
        attach_definition(w, h, name);
        e->overridden = overridden;
    }
    if (err == result::OK)
        LOG_INFO("entity: wrote %s", abs);

    memory::temp_end(t);
    return err;
}

// the instance as its block, gone, and back from its definition under the same parent
static b32 rebuild_instance(World *w, asset::Manager *m, Handle h) {
    Entity *e = get(w, h);
    Handle parent = e->parent;

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    // the writer leaves the joint to the referencing block, which this is
    config::Doc *doc = config::create(scratch, "<reload>");
    write_entity(scratch, doc, NULL, w, h, "instance", false);
    if (parent.v && e->attach_joint[0]) {
        const char *joint[] = {e->attach_joint};
        config::append(scratch, doc, doc->first, "attach_joint", joint, 1);
    }

    // the block holds copies of every string, so the entity can go before it is read
    destroy(w, h);
    Handle back = build_entity(w, m, doc->first, parent, 0);

    memory::temp_end(t);
    return back.v != 0;
}

u32 reload_definition(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    // by id, gathered first: a rebuild moves entities through the pool, which
    // the walk could not survive
    u32 count = 0;
    for (Handle h = first(w); h.v; h = next(w, h))
        if (strcmp(get(w, h)->definition, name) == 0)
            count++;

    u32 *ids = push_array(scratch, u32, count);
    u32 n = 0;
    for (Handle h = first(w); h.v && n < count; h = next(w, h))
        if (strcmp(get(w, h)->definition, name) == 0)
            ids[n++] = get(w, h)->id;

    u32 rebuilt = 0;
    for (u32 i = 0; i < n; i++) {
        Handle h = find_by_id(w, ids[i]);
        if (h.v && rebuild_instance(w, m, h))
            rebuilt++;
    }

    memory::temp_end(t);
    LOG_INFO("entity: definition '%s' rebuilt %u instance(s)", name, rebuilt);
    return rebuilt;
}

// the names in <root>/<dir> ending in ext, without it, each behind prefix
static u32 list_names(asset::Manager *m, const char *dir, const char *ext, const char *prefix,
                      Name *out, u32 cap) {
    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    file::Entry *entries = push_array(scratch, file::Entry, FILE_LIST_CAP);
    u32 found = file::list(FPATH(scratch, asset::root(m), dir), entries, FILE_LIST_CAP);
    if (found > FILE_LIST_CAP) {
        LOG_WARN("entity: %s holds %u entries; only the first %d are listed", dir, found,
                 FILE_LIST_CAP);
        found = FILE_LIST_CAP;
    }

    u64 ext_len = strlen(ext);
    u32 n = 0;
    for (u32 i = 0; i < found; i++) {
        u64 len = strlen(entries[i].name);
        if (entries[i].is_directory || len <= ext_len ||
            strcmp(entries[i].name + len - ext_len, ext) != 0)
            continue;
        if (n < cap)
            snprintf(out[n].v, ENTITY_NAME_CAP, "%s%.*s", prefix, (int)(len - ext_len),
                     entries[i].name);
        n++;
    }

    memory::temp_end(t);
    return n;
}

u32 list_levels(asset::Manager *m, Name *out, u32 cap) {
    ASSERT(m && (out || cap == 0));
    return list_names(m, LEVEL_DIR, LEVEL_EXT, "", out, cap);
}

u32 list_definitions(asset::Manager *m, const char *subdir, Name *out, u32 cap) {
    ASSERT(m && (out || cap == 0));

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    const char *dir = subdir ? FPATH(scratch, ENTITY_DIR, subdir) : ENTITY_DIR;
    const char *prefix = subdir ? FPATH(scratch, subdir, "") : ""; // "templates/"
    u32 n = list_names(m, dir, ENTITY_EXT, prefix, out, cap);

    memory::temp_end(t);
    return n;
}

static u32 spawn_instances(World *w, asset::Manager *m, config::Doc *doc) {
    u32 spawned = 0;
    for (config::Node *n = config::find(doc->first, "instance"); n;
         n = config::find_next(n, "instance")) {
        if (build_entity(w, m, n, NONE, 0).v)
            spawned++;
    }
    return spawned;
}

result::Error load_level(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    config::DocResult doc = load_data_file(m, scratch, LEVEL_DIR, name, LEVEL_EXT);
    if (doc.err != result::OK) {
        memory::temp_end(t);
        return doc.err;
    }

    u32 spawned = spawn_instances(w, m, doc.val);

    memory::temp_end(t);
    LOG_INFO("entity: %s%s spawned %u instance(s)", name, LEVEL_EXT, spawned);
    return result::OK;
}

u32 load_level_text(World *w, asset::Manager *m, char *text, const char *name) {
    ASSERT(w && m && text && name);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    config::DocResult doc = config::parse(scratch, text, name);
    u32 spawned = doc.err == result::OK ? spawn_instances(w, m, doc.val) : 0;
    if (doc.err != result::OK)
        LOG_ERR("entity: the level text '%s' would not parse", name);

    memory::temp_end(t);
    return spawned;
}

static config::Doc *level_doc(memory::Arena *a, World *w, const char *name) {
    config::Doc *doc = config::create(a, name);
    for (Handle h = first(w); h.v; h = next(w, h))
        if (get(w, h)->parent.v == 0)
            write_entity(a, doc, NULL, w, h, "instance", false);
    return doc;
}

char *write_level(memory::Arena *a, World *w) {
    ASSERT(a && w);
    // the document stays in the arena beside its text as scaffolding; the
    // caller owns the arena and resets it, as the editor's snapshot does
    return config::write(a, level_doc(a, w, "<level>"));
}

result::Error save_level(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    const char *abs = data_file_path(m, scratch, LEVEL_DIR, name, LEVEL_EXT);
    if (!abs) {
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    result::Error e = config::save(scratch, level_doc(scratch, w, abs), abs);

    memory::temp_end(t);
    return e;
}

void clear(World *w) {
    ASSERT(w);

    // destroy takes a subtree with it, so walking the list while destroying
    // would step onto children already gone; instead, take the first live
    // entity's root until nothing is left
    for (Handle h = first(w); h.v; h = first(w)) {
        Entity *e = get(w, h);
        while (e->parent.v) {
            h = e->parent;
            e = get(w, h);
        }
        destroy(w, h);
    }

    w->accumulator = 0.0f;
    w->alpha = 0.0f;
}

} // namespace entity
