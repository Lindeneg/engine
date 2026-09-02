#pragma once

#include <stddef.h>

#include "asset.h"
#include "common.h"
#include "config.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"
#include "shader.h"

// Entities and their components.
//
// An entity is a fat struct holding one slot index per component type. Adding
// a component *type* is engine code: a line in COMPONENT_LIST, a struct whose
// first field is `Handle owner`, and a field table with defaults in entity.cpp.
// The parser, the writer, add_component and an editor's inspector all read the
// table, so none of them changes. Which entity has which components is data.
//
// Three layers of data feed this, each written by a different tool:
//
//   assets/<path>.import.cfg   how an asset enters the world       (importer)
//   entities/<name>.entity     the template: components, children  (entity editor)
//   levels/<name>.level        instances: which template, placed   (level editor)
//
// Unlike asset handles, entity handle 0 is nothing, not a fallback: "the thing
// I was tracking died" is a normal runtime condition. get() returns NULL.

#define ENTITY_NAME_CAP 64
#define ENTITY_PATH_CAP 128

#define COMPONENT_LIST(X)                                                                          \
    X(RENDERABLE, Renderable, "renderable")                                                       \
    X(ANIMATOR, Animator, "animator")                                                             \
    X(CAMERA, Camera, "camera")                                                                   \
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

// Transform is intrinsic rather than a component: an entity without a place in
// the world is meaningless, and this keeps a lookup off the hottest path.
struct Transform {
    linalg::Vec3 position;
    linalg::Quat rotation;
    linalg::Vec3 scale;

    // What was authored. The engine uses the quaternion; a person reads degrees
    // back, so a rotation typed as degrees is written out as degrees.
    linalg::Vec3 euler;
    b32 use_euler;
};

inline Transform transform_identity() {
    return Transform{
        linalg::Vec3{0.0f, 0.0f, 0.0f}, linalg::quat_identity(), linalg::Vec3{1.0f, 1.0f, 1.0f},
        linalg::Vec3{0.0f, 0.0f, 0.0f}, true,
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

// Every component begins with the entity that owns it, so a system can walk one
// dense component pool and still get back to a transform. add_component fills
// it; nothing else should write it.
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
    f32 fov_y;  // vertical, degrees
    f32 z_near; // too small a near plane is where depth precision goes to die
    f32 z_far;
    b32 active; // the world renders through the first active one it finds
};

enum LightKind { LIGHT_DIRECTIONAL, LIGHT_KIND_COUNT };

// A directional light has no position: its direction is the entity's forward,
// which is -Z, so rotating the entity aims it.
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

    // Attachment to a joint of the parent's skeleton. An empty name is ordinary
    // parenting. The index is resolved once when the parent is set.
    char attach_joint[ENTITY_NAME_CAP];
    i32 attach_joint_index;

    u32 components[COMPONENT_COUNT]; // slot index per type, 0 for absent
    b32 active;

    // Provenance, for writing back. definition is the file this entity was
    // instantiated from ("" when declared inline). overridden has a bit per
    // component the referencing block restated. owned_by_definition marks what
    // a definition spawned below its root, which the referencing file does not
    // write out again.
    char definition[ENTITY_NAME_CAP];
    u32 overridden;
    b32 owned_by_definition;
};

// Reflection: what a component's fields are, for the parser, the writer and
// the editor. Fields not listed are transient and never touch a file.
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

    // Runs after the fields are read: resolves paths to handles, validates.
    void (*after_read)(World *w, asset::Manager *m, Handle h, void *component);
};

const ComponentDesc *component_desc(Component c);

struct Config {
    u32 max_entities;
    u32 max_renderables;
    u32 max_animators;
    u32 max_cameras;
    u32 max_lights;

    // Simulation rate. Bullet physics has to be exact, which means it can
    // never see a timestep that varies with framerate. 0 takes the default.
    f32 fixed_hz;

    // How many fixed steps one frame may run before the backlog is dropped,
    // which stops a slow frame from making the next one slower. 0 takes the
    // default.
    u32 max_fixed_steps;
};

// The `world` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

RESULT_TYPE(World *, WorldResult);
RESULT_TYPE(Handle, HandleResult);

// The world arena holds the entity and component pools plus one joint palette
// per animator slot, all pushed once. Nothing here allocates per frame.
WorldResult init(memory::Arena *a, Config cfg);

// NULL for a handle that is not live, including NONE. Check it.
Entity *get(World *w, Handle h);
b32 alive(World *w, Handle h);

Handle spawn(World *w, const char *name);

// Destroys the entity and, recursively, its children.
void destroy(World *w, Handle h);

// Reparents, resolving the joint name against the parent's skeleton. Pass NONE
// to detach. joint may be NULL or empty for ordinary parenting.
void set_parent(World *w, asset::Manager *m, Handle child, Handle parent, const char *joint);

// Adds with the engine defaults, or returns the existing component unchanged.
Renderable *add_renderable(World *w, Handle h);
Animator *add_animator(World *w, Handle h);
Camera *add_camera(World *w, Handle h);
Light *add_light(World *w, Handle h);

Renderable *renderable(World *w, Handle h); // NULL when absent
Animator *animator(World *w, Handle h);
Camera *camera(World *w, Handle h);
Light *light(World *w, Handle h);

// The first active camera, or NONE. Walks the camera pool, not every entity.
Handle active_camera(World *w);

// Fills the per-frame constants every shader reads, from the active camera and
// the first active light. False when there is no camera, leaving out untouched.
b32 build_frame(World *w, f32 aspect, shader::FrameData *out);

// The skinning palette for an entity's animator, or NULL when it has none.
const linalg::Mat4 *palette(World *w, Handle h, u32 *out_count);

// One frame: 0..max_fixed_steps fixed steps at fixed_hz, then one variable step
// at the real dt, then world transforms.
void update(World *w, asset::Manager *m, memory::Arena *scratch, f32 dt);

// How far the frame sits between the last fixed step and the next, in 0..1.
f32 fixed_alpha(World *w);
f32 fixed_dt(World *w);

// Queues every visible renderable. The one place this module reaches into the
// renderer.
void submit(World *w, asset::Manager *m);

// Iteration for the editor and for systems. Returns NONE at the end.
Handle first(World *w);
Handle next(World *w, Handle h);

// One "entity", "child" or "instance" block, built in place. A block either
// references a definition and overrides parts of it, or declares its
// components inline. Returns NONE when nothing could be spawned.
Handle spawn_from_config(World *w, asset::Manager *m, config::Node *decl, Handle parent);

// The inverse: the entity and its subtree as a block under parent (NULL for
// top level), using `key` for this node and "child" below it. A definition
// reference is written as one unless expand is set, which spells it out --
// what save_definition needs and save_level must not do.
void write_entity(memory::Arena *a, config::Doc *doc, config::Node *parent, World *w, Handle h,
                  const char *key, b32 expand);

// Instantiates entities/<name>.entity, including its children. Read fresh each
// time: an editor rewrites these files while the game is running.
HandleResult spawn_definition(World *w, asset::Manager *m, const char *name);
result::Error save_definition(World *w, asset::Manager *m, Handle h, const char *name);

// levels/<name>.level: one instance block per root entity.
result::Error load_level(World *w, asset::Manager *m, const char *name);
result::Error save_level(World *w, asset::Manager *m, const char *name);

} // namespace entity
