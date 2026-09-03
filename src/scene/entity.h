#pragma once

#include <stddef.h>

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"
#include "gfx/shader.h"
#include "math/linalg.h"

// An entity is a fat struct holding one slot index per component type. Adding
// a component type is engine code: a line in COMPONENT_LIST, a struct whose
// first field is `Handle owner`, and a field table with defaults in entity.cpp.
// Which entity has which components is data, in three layers:
//
//   assets/<path>.import.cfg   how an asset enters the world       (importer)
//   entities/<name>.entity     the template: components, children  (entity editor)
//   levels/<name>.level        instances: which template, placed   (level editor)
//
// Unlike asset handles, handle 0 is nothing, not a fallback: a dead entity is a
// normal runtime condition, and get() returns NULL.

#define ENTITY_NAME_CAP 64
#define ENTITY_PATH_CAP 128

#define COMPONENT_LIST(X)                                                                          \
    X(RENDERABLE, Renderable, "renderable")                                                        \
    X(ANIMATOR, Animator, "animator")                                                              \
    X(CAMERA, Camera, "camera")                                                                    \
    X(LIGHT, Light, "light")

namespace entity {

struct Handle {
    u32 v;
};

constexpr Handle NONE = {0};

enum Component {
#define X(ID, Type, key) COMPONENT_##ID,
    COMPONENT_LIST(X)
#undef X
        COMPONENT_COUNT,
};

// intrinsic, not a component: every entity has a place in the world
struct Transform {
    linalg::Vec3 position;
    linalg::Quat rotation;
    linalg::Vec3 scale;

    // what was authored, so degrees are written back as degrees
    linalg::Vec3 euler;
    b32 use_euler;
};

inline Transform transform_identity() {
    return Transform{
        linalg::Vec3{0.0f, 0.0f, 0.0f},
        linalg::quat_identity(),
        linalg::Vec3{1.0f, 1.0f, 1.0f},
        linalg::Vec3{0.0f, 0.0f, 0.0f},
        true,
    };
}

inline void set_euler(Transform *t, linalg::Vec3 degrees) {
    t->euler = degrees;
    t->use_euler = true;
    t->rotation = linalg::quat_from_euler(degrees);
}

inline void set_rotation(Transform *t, linalg::Quat q) {
    t->rotation = q;
    t->use_euler = false;
}

// every component starts with its owner, so a system can walk a dense pool and
// get back to a transform; add_component fills it
struct Renderable {
    Handle owner;
    asset::ModelHandle model;
    char model_path[ENTITY_PATH_CAP]; // what the file said; model is resolved from it
    b32 visible;
};

struct Animator {
    Handle owner;
    asset::ClipHandle clip;
    char clip_path[ENTITY_PATH_CAP];
    asset::SkeletonHandle skeleton;
    f32 time;
    f32 speed;
    b32 playing;
    u32 joint_count;
};

struct Camera {
    Handle owner;
    f32 fov_y; // vertical, degrees
    f32 z_near;
    f32 z_far;
    b32 active; // the first active one renders
};

enum LightKind { LIGHT_DIRECTIONAL, LIGHT_KIND_COUNT };

// a directional light has no position; it shines along the entity's forward (-Z)
struct Light {
    Handle owner;
    LightKind kind;
    linalg::Vec3 color;
    f32 intensity;
    f32 ambient; // fraction of color applied regardless of facing
    b32 active;
};

struct Entity {
    char name[ENTITY_NAME_CAP];

    Transform local;
    linalg::Mat4 world; // recomputed every update from the hierarchy

    Handle parent;
    Handle first_child;
    Handle next_sibling;

    // joint of the parent's skeleton, or empty for plain parenting; resolved in set_parent
    char attach_joint[ENTITY_NAME_CAP];
    i32 attach_joint_index;

    u32 components[COMPONENT_COUNT]; // slot index per type, 0 for absent
    b32 active;

    // provenance, for writing back
    char definition[ENTITY_NAME_CAP]; // the file this was instantiated from; "" when inline
    u32 overridden;                   // a bit per component the referencing block restated
    b32 owned_by_definition;          // spawned by a definition below its root; not written again
};

// reflection for the parser, the writer and the editor; unlisted fields never touch a file
enum FieldType { FIELD_F32, FIELD_I32, FIELD_B32, FIELD_VEC3, FIELD_STRING, FIELD_ENUM };

struct FieldDesc {
    const char *key;
    FieldType type;
    u32 offset;
    u32 cap;                  // FIELD_STRING: bytes at offset
    const char *const *names; // FIELD_ENUM: index -> name
    u32 name_count;
};

struct World;

struct ComponentDesc {
    const char *key;
    u64 size;
    const void *defaults; // an instance of the struct with the engine defaults
    const FieldDesc *fields;
    u32 field_count;

    // after the fields are read: resolves paths to handles, validates
    void (*after_read)(World *w, asset::Manager *m, Handle h, void *component);
};

const ComponentDesc *component_desc(Component c);

struct Config {
    u32 max_entities;
    u32 max_renderables;
    u32 max_animators;
    u32 max_cameras;
    u32 max_lights;

    // simulation rate; physics must never see a framerate-dependent step. 0 takes the default
    f32 fixed_hz;

    // fixed steps one frame may run before the backlog is dropped. 0 takes the default
    u32 max_fixed_steps;
};

// The `world` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

RESULT_TYPE(World *, WorldResult);
RESULT_TYPE(Handle, HandleResult);

// Pushes the pools and one palette per animator slot once; nothing allocates per frame.
WorldResult init(memory::Arena *a, Config cfg);

// NULL for a handle that is not live, including NONE.
Entity *get(World *w, Handle h);
b32 alive(World *w, Handle h);

Handle spawn(World *w, const char *name);

// Recursive over children.
void destroy(World *w, Handle h);

// NONE detaches; joint may be NULL or empty for plain parenting.
void set_parent(World *w, asset::Manager *m, Handle child, Handle parent, const char *joint);

// With the engine defaults, or the existing component unchanged.
Renderable *add_renderable(World *w, Handle h);
Animator *add_animator(World *w, Handle h);
Camera *add_camera(World *w, Handle h);
Light *add_light(World *w, Handle h);

Renderable *renderable(World *w, Handle h); // NULL when absent
Animator *animator(World *w, Handle h);
Camera *camera(World *w, Handle h);
Light *light(World *w, Handle h);

// Walking one component's live slots, for the systems and tools that live
// outside this file. 0 ends the walk, as with a pool index.
u32 component_first(World *w, Component c);
u32 component_next(World *w, Component c, u32 slot);
void *component_at(World *w, Component c, u32 slot);

// The first active camera, or NONE.
Handle active_camera(World *w);

// Per-frame constants from the active camera and the first active light; false
// when there is no camera.
b32 build_frame(World *w, f32 aspect, shader::FrameData *out);

// The skinning palette for an entity's animator, or NULL when it has none.
const linalg::Mat4 *palette(World *w, Handle h, u32 *out_count);

// A frame in three parts, because the fixed clock is the engine's but what runs
// on it is not. begin_frame banks dt and answers how many fixed steps are owed;
// the caller runs that many, doing its own simulation alongside step_fixed;
// end_frame does the frame-rate work and rebuilds world transforms.
//
//     u32 steps = entity::begin_frame(world, dt);
//     for (u32 i = 0; i < steps; i++) {
//         game_step(entity::fixed_dt(world));
//         entity::step_fixed(world, assets);
//     }
//     entity::end_frame(world, assets, &scratch, dt);
//
// Running zero steps is normal and running several is normal; what is not
// allowed is running game code at frame rate and calling it simulation. A
// caller that wants no simulation at all -- an editor holding a level still --
// simply skips the loop and calls end_frame, and the world still draws.
u32 begin_frame(World *w, f32 dt);
void step_fixed(World *w, asset::Manager *m);
void end_frame(World *w, asset::Manager *m, memory::Arena *scratch, f32 dt);

// How far the frame sits between the last fixed step and the next, in 0..1.
f32 fixed_alpha(World *w);
f32 fixed_dt(World *w);

// Queues every visible renderable.
void submit(World *w, asset::Manager *m);

// Every visible renderable's bounds as a wire box, through gfx/debugdraw. For
// the debug toggle now and the editor's selection later.
void draw_bounds(World *w, asset::Manager *m, linalg::Vec4 color);

// NONE at the end.
Handle first(World *w);
Handle next(World *w, Handle h);

// One "entity", "child" or "instance" block; NONE when nothing could be spawned.
Handle spawn_from_config(World *w, asset::Manager *m, config::Node *decl, Handle parent);

// The inverse: the subtree as a block under parent (NULL for top level), `key`
// for this node and "child" below it. A definition reference is written as one
// unless expand spells it out.
void write_entity(memory::Arena *a, config::Doc *doc, config::Node *parent, World *w, Handle h,
                  const char *key, b32 expand);

// entities/<name>.entity, read fresh each time.
HandleResult spawn_definition(World *w, asset::Manager *m, const char *name);
result::Error save_definition(World *w, asset::Manager *m, Handle h, const char *name);

// levels/<name>.level: one instance block per root entity.
result::Error load_level(World *w, asset::Manager *m, const char *name);
result::Error save_level(World *w, asset::Manager *m, const char *name);

} // namespace entity
