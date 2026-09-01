#include "entity.h"

#include <math.h>
#include <string.h>

#include "anim.h"
#include "config.h"
#include "logger.h"
#include "path.h"
#include "pool.h"
#include "render.h"
#include "shader.h"

#define ENTITY_DIR "entities"
#define ENTITY_EXT ".entity"
#define LEVEL_DIR "levels"
#define LEVEL_EXT ".level"

namespace entity {

struct World {
    memory::Arena *arena;
    Config cfg;

    pool::Pool entities;
    pool::Pool components[COMPONENT_COUNT];

    // One palette per animator slot, indexed by slot rather than stored on the
    // component. Fixed at init so nothing allocates while the game runs.
    linalg::Mat4 *skin;   // slot * SHADER_MAX_JOINTS
    linalg::Mat4 *global; // joint model-space transforms, for attachments
};

static const u64 COMPONENT_STRIDE[COMPONENT_COUNT] = {
    sizeof(Renderable),
    sizeof(Animator),
};

WorldResult init(memory::Arena *a, Config cfg) {
    ASSERT(a);

    if (!cfg.max_entities) {
        LOG_ERR("entity: init needs a nonzero entity capacity");
        return WorldResult{.err = result::ERR_INVALID};
    }

    World *w = push_struct(a, World);
    w->arena = a;
    w->cfg = cfg;

    w->entities = pool::create(a, cfg.max_entities, sizeof(Entity));

    u32 caps[COMPONENT_COUNT] = {
        MAX(cfg.max_renderables, 1u),
        MAX(cfg.max_animators, 1u),
    };
    for (u32 c = 0; c < COMPONENT_COUNT; c++)
        w->components[c] = pool::create(a, caps[c], COMPONENT_STRIDE[c]);

    // slot 0 is reserved in every pool and never animates, but sizing for it
    // keeps the indexing arithmetic free of an off-by-one
    u64 palette_slots = (u64)caps[COMPONENT_ANIMATOR] * SHADER_MAX_JOINTS;
    w->skin = push_array(a, linalg::Mat4, palette_slots);
    w->global = push_array(a, linalg::Mat4, palette_slots);

    LOG_INFO("entity: world with %u entities, %u renderables, %u animators (%.2f MiB palettes)",
             cfg.max_entities, caps[COMPONENT_RENDERABLE], caps[COMPONENT_ANIMATOR],
             (f64)(palette_slots * sizeof(linalg::Mat4) * 2) / (1024.0 * 1024.0));

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
    e->local = transform_identity();
    e->world = linalg::mat4_identity(); // zeroed would be degenerate
    e->attach_joint_index = -1;
    e->active = true;

    return Handle{pool::make_handle(&w->entities, idx)};
}

// Unhooks a child from its parent's sibling list. Linear in sibling count,
// which is what an intrusive list costs and why it needs no allocation.
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

    // children first, and snapshot each next pointer before recursing: the
    // recursive call unlinks the child and clears it
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

    pool::release(&w->entities, pool_handle_index(h.v));
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

void set_parent(World *w, asset::Manager *m, Handle child, Handle parent, const char *joint) {
    ASSERT(w && m);

    Entity *ce = get(w, child);
    if (!ce) {
        LOG_WARN("entity: set_parent on a dead child handle %u", child.v);
        return;
    }

    unlink(w, child);

    ce->attach_joint[0] = 0;
    ce->attach_joint_index = -1;

    if (parent.v == 0)
        return;

    Entity *pe = get(w, parent);
    if (!pe) {
        LOG_WARN("entity: \"%s\" parented to a dead handle %u", ce->name, parent.v);
        return;
    }

    ce->parent = parent;
    ce->next_sibling = pe->first_child;
    pe->first_child = child;

    if (!joint || !joint[0])
        return;

    snprintf(ce->attach_joint, ENTITY_NAME_CAP, "%s", joint);

    // Resolved once here rather than per frame. A name that does not match is a
    // content error and stays at -1, which falls back to plain parenting --
    // visible as a weapon floating at the holder's origin.
    asset::Skeleton *skel = asset::get(m, skeleton_of(w, m, parent));
    u32 *found = strmap::find(&skel->by_name, ce->attach_joint);
    if (found) {
        ce->attach_joint_index = (i32)*found;
    } else {
        LOG_WARN("entity: \"%s\" attaches to joint \"%s\", which \"%s\" does not have", ce->name,
                 ce->attach_joint, pe->name);
    }
}

static void *add_component(World *w, Handle h, u32 type) {
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
    return pool::at(&w->components[type], idx);
}

static void *component_of(World *w, Handle h, u32 type) {
    Entity *e = get(w, h);
    if (!e || !e->components[type])
        return NULL;
    return pool::at(&w->components[type], e->components[type]);
}

Renderable *add_renderable(World *w, Handle h) {
    Renderable *r = (Renderable *)add_component(w, h, COMPONENT_RENDERABLE);
    if (r)
        r->visible = true; // zeroed would mean invisible, which reads as a bug
    return r;
}

Animator *add_animator(World *w, Handle h) {
    Animator *a = (Animator *)add_component(w, h, COMPONENT_ANIMATOR);
    if (a) {
        a->speed = 1.0f;
        a->playing = true;
    }
    return a;
}

Renderable *renderable(World *w, Handle h) {
    return (Renderable *)component_of(w, h, COMPONENT_RENDERABLE);
}

Animator *animator(World *w, Handle h) {
    return (Animator *)component_of(w, h, COMPONENT_ANIMATOR);
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
    u32 idx = pool::next(&w->entities, pool_handle_index(h.v));
    return idx ? Handle{pool::make_handle(&w->entities, idx)} : NONE;
}

static void advance_animator(World *w, asset::Manager *m, memory::Arena *scratch, Handle h,
                             f32 dt) {
    Entity *e = get(w, h);
    u32 slot = e->components[COMPONENT_ANIMATOR];
    if (!slot)
        return;

    Animator *a = (Animator *)pool::at(&w->components[COMPONENT_ANIMATOR], slot);

    asset::Skeleton *skel = asset::get(m, a->skeleton);
    a->joint_count = MIN(skel->joint_count, (u32)SHADER_MAX_JOINTS);
    if (a->joint_count == 0)
        return;

    if (a->playing)
        a->time += dt * a->speed;

    asset::Clip *clip = a->clip.v ? asset::get(m, a->clip) : NULL;
    anim::sample(scratch, skel, clip, a->time, skin_at(w, slot), global_at(w, slot));
}

// Parent before child, following the hierarchy rather than pool order: a child
// must read a world matrix that is already current this frame.
static void resolve_world(World *w, asset::Manager *m, Handle h) {
    Entity *e = get(w, h);
    if (!e)
        return;

    linalg::Mat4 local = linalg::mat4_from_trs(e->local.position, e->local.rotation, e->local.scale);
    Entity *pe = get(w, e->parent);

    if (!pe) {
        e->world = local;
    } else if (e->attach_joint_index >= 0) {
        u32 slot = pe->components[COMPONENT_ANIMATOR];
        Animator *pa =
            slot ? (Animator *)pool::at(&w->components[COMPONENT_ANIMATOR], slot) : NULL;

        // a skeleton can come from an animator alone, so the parent is not
        // guaranteed to have a renderable to take an import transform from
        Renderable *pr = renderable(w, e->parent);

        if (pa && pr && (u32)e->attach_joint_index < pa->joint_count) {
            // The joint's model-space transform, not its skinning matrix: the
            // palette entry carries the inverse bind, which would send the
            // attachment back into the mesh's pre-skin space.
            linalg::Mat4 joint = global_at(w, slot)[e->attach_joint_index];

            // The parent's import transform sits between its model space and
            // the world, so the joint has to pass through it too -- otherwise a
            // gun on a 0.857-scaled soldier lands 14x out.
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

void update(World *w, asset::Manager *m, memory::Arena *scratch, f32 dt) {
    ASSERT(w && m && scratch);

    // Animation first: an attachment reads the joint transforms this produces.
    for (Handle h = first(w); h.v; h = next(w, h))
        advance_animator(w, m, scratch, h, dt);

    for (Handle h = first(w); h.v; h = next(w, h))
        if (get(w, h)->parent.v == 0)
            resolve_world(w, m, h);
}

void submit(World *w, asset::Manager *m) {
    ASSERT(w && m);

    for (Handle h = first(w); h.v; h = next(w, h)) {
        Entity *e = get(w, h);
        if (!e->active)
            continue;

        Renderable *r = renderable(w, h);
        if (!r || !r->visible)
            continue;

        u32 joint_count = 0;
        const linalg::Mat4 *pal = palette(w, h, &joint_count);
        render::submit_model(m, r->model, e->world, pal, joint_count);
    }
}

static void read_transform(config::Node *n, Transform *out) {
    if (!n)
        return;

    config::Node *body = n->first_child;

    config::Node *p = config::find(body, "position");
    if (p)
        out->position = linalg::Vec3{
            config::value_f32(p, 0, out->position.x),
            config::value_f32(p, 1, out->position.y),
            config::value_f32(p, 2, out->position.z),
        };

    // Three values are Euler degrees about X, Y, Z -- what a person types.
    // Four are a raw quaternion, for a tool that wants to round-trip exactly.
    config::Node *r = config::find(body, "rotation");
    if (r && r->value_count == 3) {
        out->rotation = linalg::quat_from_euler(linalg::Vec3{
            config::value_f32(r, 0, 0.0f),
            config::value_f32(r, 1, 0.0f),
            config::value_f32(r, 2, 0.0f),
        });
    } else if (r) {
        linalg::Quat q{
            config::value_f32(r, 0, out->rotation.x),
            config::value_f32(r, 1, out->rotation.y),
            config::value_f32(r, 2, out->rotation.z),
            config::value_f32(r, 3, out->rotation.w),
        };

        // A non-unit quaternion does not produce a scaled rotation, it produces
        // a broken one: mat4_from_quat simplifies its terms using
        // w^2+x^2+y^2+z^2 == 1, so feeding it anything else yields a
        // non-orthogonal matrix that stretches each axis by a different amount
        // and shears. "rotation 2 0 0 1" leaves X alone and blows Y and Z out
        // to 8x, which reads as a mangled model rather than a bad angle.
        // Normalising here is the difference between a typo and an afternoon.
        f32 len = sqrtf(linalg::quat_dot(q, q));
        if (len < 1e-8f) {
            LOG_WARN("entity: rotation is a zero quaternion, ignored");
        } else {
            if (len < 0.999f || len > 1.001f)
                LOG_WARN("entity: rotation is not a unit quaternion (length %.3f); normalized. "
                         "Three values would be read as Euler degrees instead.",
                         (f64)len);
            out->rotation = linalg::quat_normalize(q);
        }
    }

    config::Node *s = config::find(body, "scale");
    if (s) {
        // one value is uniform scale, which is what almost every instance wants
        f32 sx = config::value_f32(s, 0, out->scale.x);
        out->scale = linalg::Vec3{
            sx,
            config::value_f32(s, 1, s->value_count > 1 ? out->scale.y : sx),
            config::value_f32(s, 2, s->value_count > 2 ? out->scale.z : sx),
        };
    }
}

// A definition that references itself, directly or through a chain, would spawn
// until the pool ran dry. Depth is bounded instead, and reported.
#define MAX_DEFINITION_DEPTH 8

static Handle build_entity(World *w, asset::Manager *m, config::Node *decl, Handle parent,
                           u32 depth);

static void build_children(World *w, asset::Manager *m, config::Node *body, Handle self,
                           u32 depth) {
    for (config::Node *c = config::find(body, "child"); c; c = config::find_next(c, "child"))
        build_entity(w, m, c, self, depth);
}

// The components a block declares inline. Skipped entirely when the block
// instead references a definition, which brings its own.
static void build_components(World *w, asset::Manager *m, Handle h, config::Node *body,
                             const char *name) {
    config::Node *rn = config::find(body, "renderable");
    if (rn) {
        const char *model = config::value(config::find(rn->first_child, "model"), 0, NULL);
        if (!model) {
            LOG_ERR("entity: \"%s\" has a renderable with no model", name);
        } else {
            Renderable *r = add_renderable(w, h);
            if (r) {
                r->model = OR_ELSE(asset::load_model(m, asset::SCOPE_PERSISTENT, model),
                                   asset::MODEL_FALLBACK);
                r->visible = config::value_b32(config::find(rn->first_child, "visible"), 0, true);
                render::upload_model(m, r->model);
            }
        }
    }

    config::Node *an = config::find(body, "animator");
    if (!an)
        return;

    Renderable *r = renderable(w, h);
    if (!r) {
        LOG_ERR("entity: \"%s\" has an animator but no renderable to take a skeleton from", name);
        return;
    }

    Animator *a = add_animator(w, h);
    if (!a)
        return;

    a->skeleton = asset::get(m, r->model)->skeleton;
    a->speed = config::value_f32(config::find(an->first_child, "speed"), 0, 1.0f);
    a->playing = config::value_b32(config::find(an->first_child, "playing"), 0, true);

    const char *clip = config::value(config::find(an->first_child, "clip"), 0, NULL);
    if (clip)
        a->clip = OR_ELSE(asset::load_clip(m, asset::SCOPE_PERSISTENT, clip, a->skeleton),
                          asset::CLIP_FALLBACK);
}

static HandleResult instantiate(World *w, asset::Manager *m, const char *name, u32 depth) {
    if (depth >= MAX_DEFINITION_DEPTH) {
        LOG_ERR("entity: definition '%s' nested deeper than %d -- a cycle?", name,
                MAX_DEFINITION_DEPTH);
        return HandleResult{.err = result::ERR_INVALID};
    }

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    char file[ENTITY_NAME_CAP + 16];
    snprintf(file, sizeof(file), "%s%s", name, ENTITY_EXT);
    const char *abs = FPATH(scratch, asset::root(m), ENTITY_DIR, file);

    config::DocResult doc = config::load(scratch, abs);
    if (doc.err != result::OK) {
        memory::temp_end(t);
        return HandleResult{.err = doc.err};
    }

    config::Node *decl = config::find(doc.val->first, "entity");
    if (!decl) {
        LOG_ERR("entity: %s has no 'entity' block", file);
        memory::temp_end(t);
        return HandleResult{.err = result::ERR_PARSE};
    }

    // Parented by the caller, not here: the joint name to attach by lives in
    // the referencing block, not in the definition.
    Handle h = build_entity(w, m, decl, NONE, depth);
    memory::temp_end(t);

    if (!h.v)
        return HandleResult{.err = result::ERR_EXHAUSTED};
    return HandleResult{.val = h};
}

// One "entity", "child" or "instance" block -- they are the same thing, which
// is why they share this. A block either references another definition and
// overrides parts of it, or declares its components inline. The three files
// then compose: a level instances a soldier, the soldier's weapon slot
// instances a pistol, and each layer overrides only what it cares about.
static Handle build_entity(World *w, asset::Manager *m, config::Node *decl, Handle parent,
                           u32 depth) {
    config::Node *body = decl->first_child;
    const char *label = config::value(decl, 0, NULL);
    const char *def = config::value(config::find(body, "definition"), 0, NULL);

    Handle h = NONE;
    if (def) {
        // Read fresh per reference rather than cached. An editor rewrites these
        // files while the game runs, and ten crates is ten small parses, not a
        // reason for a cache to go stale.
        HandleResult r = instantiate(w, m, def, depth + 1);
        if (r.err != result::OK)
            return NONE;
        h = r.val;
    } else {
        h = spawn(w, label ? label : "<unnamed>");
        if (!h.v)
            return NONE;
        build_components(w, m, h, body, label ? label : "<unnamed>");
    }

    // Overrides, applied the same way whether the components came from a
    // definition or from this block. An absent key leaves what is already there.
    Entity *e = get(w, h);
    if (def && label)
        snprintf(e->name, ENTITY_NAME_CAP, "%s", label);
    read_transform(config::find(body, "transform"), &e->local);

    // after the components exist: attaching resolves a joint against the
    // parent's skeleton, and children read this entity's own
    if (parent.v)
        set_parent(w, m, h, parent, config::value(config::find(body, "attach_joint"), 0, NULL));

    build_children(w, m, body, h, depth);
    return h;
}

HandleResult spawn_definition(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);
    return instantiate(w, m, name, 0);
}

result::Error load_level(World *w, asset::Manager *m, const char *name) {
    ASSERT(w && m && name);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    char file[ENTITY_NAME_CAP + 16];
    snprintf(file, sizeof(file), "%s%s", name, LEVEL_EXT);
    const char *abs = FPATH(scratch, asset::root(m), LEVEL_DIR, file);

    config::DocResult doc = config::load(scratch, abs);
    if (doc.err != result::OK) {
        memory::temp_end(t);
        return doc.err;
    }

    u32 spawned = 0;
    for (config::Node *n = config::find(doc.val->first, "instance"); n;
         n = config::find_next(n, "instance")) {
        // An instance is a root-level reference to a definition, which is
        // exactly what a child block is, so it takes the same path and gets
        // inline children and component overrides for free.
        if (build_entity(w, m, n, NONE, 0).v)
            spawned++;
    }

    memory::temp_end(t);
    LOG_INFO("entity: %s spawned %u instance(s)", file, spawned);
    return result::OK;
}

} // namespace entity
