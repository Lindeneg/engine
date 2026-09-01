#pragma once

#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"
#include "shader.h"

// Entities and their components.
//
// An entity is a fat struct holding one slot index per component type. Adding a
// component *type* is engine code -- a type is data plus a system that
// understands it, so a new one always means new code anyway. Which entity has
// which components is data, and comes from a definition file.
//
// Three layers of data feed this, each written by a different tool:
//
//   assets/<path>.import.cfg   how an asset enters the world       (importer)
//   entities/<name>.entity     the template: components, children  (entity editor)
//   levels/<name>.level        instances: which template, placed   (level editor)
//
// Unlike asset handles, entity handle 0 is nothing, not a fallback. A magenta
// cube is a sensible answer to a missing model; a fallback entity is not, and
// "the thing I was tracking died" is a normal runtime condition rather than a
// broken deployment. So get() returns NULL and callers check.

#define ENTITY_NAME_CAP 64

namespace entity {

struct Handle {
    u32 v;
};

constexpr Handle NONE = {0};

enum Component {
    COMPONENT_RENDERABLE,
    COMPONENT_ANIMATOR,
    COMPONENT_CAMERA,
    COMPONENT_LIGHT,
    COMPONENT_COUNT,
};

// Transform is intrinsic rather than a component: an entity without a place in
// the world is meaningless, and this keeps a lookup off the hottest path.
struct Transform {
    linalg::Vec3 position;
    linalg::Quat rotation;
    linalg::Vec3 scale;
};

inline Transform transform_identity() {
    return Transform{
        linalg::Vec3{0.0f, 0.0f, 0.0f},
        linalg::quat_identity(),
        linalg::Vec3{1.0f, 1.0f, 1.0f},
    };
}

// Every component begins with the entity that owns it, so a system can walk one
// dense component pool -- eight cameras rather than four thousand entities --
// and still get back to a transform. add_component fills it; nothing else
// should write it.
struct Renderable {
    Handle owner;
    asset::ModelHandle model;
    b32 visible;
};

struct Animator {
    Handle owner;
    asset::ClipHandle clip;
    asset::SkeletonHandle skeleton;
    f32 time;
    f32 speed;
    b32 playing;
    u32 joint_count;

    // The palettes live in a fixed block indexed by this component's slot, not
    // in the struct: pool::alloc zeroes the payload, so a pointer stored here
    // would be cleared out from under it on every reuse.
};

struct Camera {
    Handle owner;
    f32 fov_y;  // vertical, degrees -- what a person types, converted at use
    f32 z_near; // too small a near plane is where depth precision goes to die
    f32 z_far;
    b32 active; // the world renders through the first active one it finds
};

enum LightKind { LIGHT_DIRECTIONAL, LIGHT_KIND_COUNT };

struct Light {
    Handle owner;
    LightKind kind;
    linalg::Vec3 color;
    f32 intensity;
    f32 ambient; // fraction of color applied regardless of facing
    b32 active;

    // A directional light has no position: its direction is the entity's
    // forward, which is -Z, so rotating the entity aims it.
};

struct Entity {
    char name[ENTITY_NAME_CAP];

    Transform local;
    linalg::Mat4 world; // recomputed every update from the hierarchy

    Handle parent;
    Handle first_child;
    Handle next_sibling;

    // Attachment to a joint of the parent's skeleton. An empty name is ordinary
    // parenting, against the parent's own transform. The index is resolved once
    // when the parent is set, because resolving a string per frame per entity
    // is exactly the kind of cost that never gets found later.
    char attach_joint[ENTITY_NAME_CAP];
    i32 attach_joint_index;

    u32 components[COMPONENT_COUNT]; // slot index per type, 0 for absent
    b32 active;
};

struct Config {
    u32 max_entities;
    u32 max_renderables;
    u32 max_animators;
    u32 max_cameras;
    u32 max_lights;

    // Simulation rate. Bullet physics has to be exact, and exact means it can
    // never see a timestep that varies with framerate: the same shot fired on a
    // 60 Hz and a 144 Hz machine has to land in the same place, and a solver
    // fed a variable dt cannot promise that. 0 takes the default.
    f32 fixed_hz;

    // How many fixed steps one frame may run before the backlog is dropped.
    // Without a cap, a frame that runs long queues more steps, which makes the
    // next frame longer, which queues more -- the machine walks itself to a
    // halt. Dropping simulated time is the lesser evil, and it is reported.
    u32 max_fixed_steps;
};

struct World;

RESULT_TYPE(World *, WorldResult);
RESULT_TYPE(Handle, HandleResult);

// The world arena holds the entity and component pools plus one joint palette
// per animator slot, all pushed once. Nothing here allocates per frame.
WorldResult init(memory::Arena *a, Config cfg);

// NULL for a handle that is not live, including NONE. Check it.
Entity *get(World *w, Handle h);
b32 alive(World *w, Handle h);

Handle spawn(World *w, const char *name);

// Destroys the entity and, recursively, its children -- an attached weapon has
// no meaning once its holder is gone.
void destroy(World *w, Handle h);

// Reparents, resolving the joint name against the parent's skeleton. Pass NONE
// to detach. joint may be NULL or empty for ordinary parenting.
void set_parent(World *w, asset::Manager *m, Handle child, Handle parent, const char *joint);

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
// the first active light. False when there is no camera, leaving out untouched
// -- a frame with no camera is a level that forgot one, not a crash.
//
// This is the one place the world speaks to the renderer besides submit(); it
// lives here because the camera and the lights are entities, and it moves to a
// systems module when there are more systems than these.
b32 build_frame(World *w, f32 aspect, shader::FrameData *out);

// The skinning palette for an entity's animator, or NULL when it has none.
const linalg::Mat4 *palette(World *w, Handle h, u32 *out_count);

// One frame. dt is real elapsed time and is used two different ways:
//
//   fixed steps    run 0..max_fixed_steps times at exactly fixed_hz. Anything
//                  whose correctness depends on the timestep goes here --
//                  physics, and eventually the animation that physics reads.
//                  Empty today; it exists so the split is not retrofitted
//                  through every system later.
//
//   variable step  runs once, at the real frame time. Anything purely visual:
//                  animation sampling now, particles and cameras later.
//
// World transforms resolve after both, since either can move an entity.
void update(World *w, asset::Manager *m, memory::Arena *scratch, f32 dt);

// How far the frame sits between the last fixed step and the next, in 0..1.
// What rendering interpolates on once there is simulated state to interpolate:
// a body stepped at 120 Hz and drawn at 144 would otherwise judder.
f32 fixed_alpha(World *w);
f32 fixed_dt(World *w);

// Queues every visible renderable. The one place this module reaches into the
// renderer; it moves to a systems module once there is more than one system.
void submit(World *w, asset::Manager *m);

// Iteration for the editor and for systems. Returns NONE at the end.
Handle first(World *w);
Handle next(World *w, Handle h);

// Instantiates entities/<name>.entity, including its children. The definition
// is read fresh each time rather than cached: an editor rewrites these files
// while the game is running.
HandleResult spawn_definition(World *w, asset::Manager *m, const char *name);

// Loads levels/<name>.level, spawning one entity per instance block.
result::Error load_level(World *w, asset::Manager *m, const char *name);

} // namespace entity
