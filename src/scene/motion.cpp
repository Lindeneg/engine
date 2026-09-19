#include "scene/motion.h"

#include "core/logger.h"
#include "math/intersect.h"

// a lift or a landing shorter than this is no step, only rounding
#define STEP_MIN_HEIGHT 1e-3f

namespace motion {

static b32 under(entity::World *w, entity::Handle h, entity::Handle root) {
    if (!root.v)
        return false;

    for (entity::Handle p = h; p.v;) {
        if (p.v == root.v)
            return true;

        entity::Entity *e = entity::get(w, p);
        if (!e)
            break;

        p = e->parent;
    }

    return false;
}

// the capsule's axis from its position to the top sphere's centre; zero for a sphere
static linalg::Vec3 axis_of(const Body *b) {
    f32 rise = b->height - 2.0f * b->radius;
    return linalg::Vec3{0.0f, rise > 0.0f ? rise : 0.0f, 0.0f};
}

// The body against one collider, in the collider's own frame: the frame is
// rigid, so the capsule stays a capsule there and a box is axis-aligned, and
// the answer comes back through the same frame. In that frame the moving
// capsule is a moving sphere at `center` against the collider extruded along
// -axis, which is what sweep and depenetrate build.
struct Contact {
    entity::Collider *collider;
    linalg::Mat4 frame;
    linalg::Mat4 inverse;
    linalg::Vec3 center; // the bottom sphere, in the collider's frame
    linalg::Vec3 axis;   // the capsule's axis there
};

static b32 contact_of(entity::World *w, entity::Collider *c, const Body *body,
                      linalg::Vec3 position, entity::Handle ignore, Contact *out) {
    entity::Entity *e = entity::get(w, c->owner);
    if (!e || !e->active || under(w, c->owner, ignore))
        return false;

    out->collider = c;
    out->frame = entity::collider_frame(w, c);
    out->inverse = linalg::mat4_inverse_affine(out->frame);
    out->center = linalg::mat4_mul_point(out->inverse, position);
    out->axis = linalg::mat4_mul_dir(out->inverse, axis_of(body));
    return true;
}

static intersect::Aabb box_of(const entity::Collider *c) {
    linalg::Vec3 half = c->size * 0.5f;
    return intersect::Aabb{-1.0f * half, half};
}

static b32 sweep(const Contact *k, f32 radius, linalg::Vec3 motion, intersect::Hit *out) {
    linalg::Vec3 local_motion = linalg::mat4_mul_dir(k->inverse, motion);
    linalg::Vec3 origin{0.0f, 0.0f, 0.0f};

    intersect::Hit h;
    b32 hit;
    if (k->collider->shape == entity::COLLIDER_SPHERE) {
        hit = intersect::sweep_sphere_capsule(k->center, radius, local_motion, origin,
                                              -1.0f * k->axis, k->collider->radius, &h);
    } else {
        intersect::Polytope p;
        intersect::polytope_extrude_box(box_of(k->collider), -1.0f * k->axis, &p);
        hit = intersect::sweep_sphere_polytope(k->center, radius, local_motion, &p, &h);
    }
    if (!hit)
        return false;

    // t is a fraction either way; the point and normal go back to the world
    out->t = h.t;
    out->point = linalg::mat4_mul_point(k->frame, h.point);
    out->normal = linalg::mat4_mul_dir(k->frame, h.normal);
    return true;
}

static b32 depenetrate(const Contact *k, f32 radius, linalg::Vec3 *out_normal, f32 *out_depth) {
    linalg::Vec3 origin{0.0f, 0.0f, 0.0f};
    linalg::Vec3 n;
    b32 overlapping;
    if (k->collider->shape == entity::COLLIDER_SPHERE) {
        overlapping = intersect::depenetrate_sphere_capsule(
            k->center, radius, origin, -1.0f * k->axis, k->collider->radius, &n, out_depth);
    } else {
        intersect::Polytope p;
        intersect::polytope_extrude_box(box_of(k->collider), -1.0f * k->axis, &p);
        overlapping = intersect::depenetrate_sphere_polytope(k->center, radius, &p, &n, out_depth);
    }
    if (!overlapping)
        return false;

    *out_normal = linalg::mat4_mul_dir(k->frame, n);
    return true;
}

// the nearest collider along the motion, from position; false when none
static b32 sweep_world(entity::World *w, const Body *body, linalg::Vec3 position,
                       linalg::Vec3 motion, entity::Handle ignore, intersect::Hit *out) {
    b32 found = false;
    for (u32 s = entity::component_first(w, entity::COMPONENT_COLLIDER); s;
         s = entity::component_next(w, entity::COMPONENT_COLLIDER, s)) {
        entity::Collider *c =
            (entity::Collider *)entity::component_at(w, entity::COMPONENT_COLLIDER, s);

        Contact k;
        if (!contact_of(w, c, body, position, ignore, &k))
            continue;

        intersect::Hit h;
        if (sweep(&k, body->radius, motion, &h) && (!found || h.t < out->t)) {
            *out = h;
            found = true;
        }
    }
    return found;
}

// Everything a sweep cannot answer, because a body that starts inside geometry
// has no surface to have hit. Deepest overlap first: it is the one most likely
// to be the real problem, and leaving it often clears the shallower ones too.
static linalg::Vec3 push_clear(entity::World *w, const Body *body, linalg::Vec3 position,
                               entity::Handle ignore, Result *res) {
    for (u32 i = 0; i < MOTION_MAX_DEPENETRATIONS; i++) {
        f32 worst_depth = 0.0f;
        linalg::Vec3 worst_normal{};
        b32 found = false;

        for (u32 s = entity::component_first(w, entity::COMPONENT_COLLIDER); s;
             s = entity::component_next(w, entity::COMPONENT_COLLIDER, s)) {
            entity::Collider *c =
                (entity::Collider *)entity::component_at(w, entity::COMPONENT_COLLIDER, s);

            Contact k;
            if (!contact_of(w, c, body, position, ignore, &k))
                continue;

            linalg::Vec3 n;
            f32 depth;
            if (!depenetrate(&k, body->radius, &n, &depth))
                continue;

            if (!found || depth > worst_depth) {
                worst_depth = depth;
                worst_normal = n;
                found = true;
            }
        }

        if (!found)
            return position;

        position = position + worst_normal * (worst_depth + MOTION_SKIN);
        res->depenetrated = true;

        // pushed up out of something is standing on it
        if (worst_normal.y > MOTION_GROUND_NORMAL_Y)
            res->grounded = true;
    }

    res->stuck = true;
    return position;
}

// The slide itself: motion from res->position, along what it meets, the
// result written over res. hit and normal are the last surface met.
static void slide(entity::World *w, const Body *body, linalg::Vec3 motion, entity::Handle ignore,
                  Result *res) {
    linalg::Vec3 left = motion;

    for (u32 i = 0; i < MOTION_MAX_SLIDES; i++) {
        if (linalg::vec3_length_sq(left) < LINALG_ZERO_EPSILON)
            break;

        intersect::Hit best{};
        if (!sweep_world(w, body, res->position, left, ignore, &best)) {
            res->position = res->position + left;
            break;
        }

        // A zero normal is the sweep saying it started inside. push_clear should
        // have made that impossible, so reaching it means a slide put us inside
        // something the push could not resolve.
        if (linalg::vec3_length_sq(best.normal) < LINALG_ZERO_EPSILON) {
            res->stuck = true;
            break;
        }

        res->hit = true;
        res->normal = best.normal;

        if (best.normal.y > MOTION_GROUND_NORMAL_Y)
            res->grounded = true;

        // up to the contact, then a skin clear of it
        res->position = res->position + left * best.t + best.normal * MOTION_SKIN;

        // and on with what is left, flattened against what was hit
        linalg::Vec3 rest = left * (1.0f - best.t);
        left = rest - best.normal * linalg::vec3_dot(rest, best.normal);
    }
}

// one sweep with no slide: how far the body gets, a skin short of whatever stops it
static linalg::Vec3 nudge(entity::World *w, const Body *body, linalg::Vec3 position,
                          linalg::Vec3 motion, entity::Handle ignore, intersect::Hit *out_hit) {
    intersect::Hit h{};
    if (!sweep_world(w, body, position, motion, ignore, &h)) {
        out_hit->t = 1.0f;
        out_hit->normal = linalg::Vec3{0.0f, 0.0f, 0.0f};
        return position + motion;
    }
    *out_hit = h;
    return position + motion * h.t + h.normal * MOTION_SKIN;
}

// The step: from where the move began, up by what the step height allows,
// the same horizontal move as far as it goes, and down onto whatever is
// there. Kept only if it got further along the move and landed on something
// to stand on. The forward move is one sweep, not a slide: sliding there
// would let a body roll up over an edge taller than its step.
static void try_step(entity::World *w, const Body *body, linalg::Vec3 start, linalg::Vec3 motion,
                     entity::Handle ignore, Result *res) {
    linalg::Vec3 flat{motion.x, 0.0f, motion.z};
    f32 flat_len = linalg::vec3_length(flat);
    if (flat_len < LINALG_ZERO_EPSILON)
        return;
    linalg::Vec3 along = flat * (1.0f / flat_len);
    f32 got = linalg::vec3_dot(res->position - start, along);

    intersect::Hit lift_hit{};
    linalg::Vec3 lifted =
        nudge(w, body, start, linalg::Vec3{0.0f, body->step_height, 0.0f}, ignore, &lift_hit);
    f32 rise = lifted.y - start.y;
    if (rise < STEP_MIN_HEIGHT) // a ceiling right there
        return;

    intersect::Hit forward_hit{};
    linalg::Vec3 forward = nudge(w, body, lifted, flat, ignore, &forward_hit);

    intersect::Hit land{};
    linalg::Vec3 landed = nudge(w, body, forward, linalg::Vec3{0.0f, -rise, 0.0f}, ignore, &land);
    if (land.t >= 1.0f || land.normal.y <= MOTION_GROUND_NORMAL_Y)
        return; // nothing to stand on up there: a wall, or a ledge gravity will handle

    if (linalg::vec3_dot(landed - start, along) <= got + STEP_MIN_HEIGHT)
        return;

    res->position = landed;
    res->grounded = true;
    res->hit = true;
    res->normal = forward_hit.t < 1.0f ? forward_hit.normal : land.normal;
}

Result move_and_slide(entity::World *w, asset::Manager *m, Body body, linalg::Vec3 motion,
                      entity::Handle ignore) {
    ASSERT(w && m);

    Result res{};
    res.position = body.position;

    if (body.radius <= 0.0f) {
        LOG_WARN_ONCE("motion: a body needs a positive radius, not %.3f", (f64)body.radius);
        return res;
    }
    if (body.height < 2.0f * body.radius) {
        LOG_WARN_ONCE("motion: a body of height %.3f is shorter than its radius allows; it is a "
                      "sphere",
                      (f64)body.height);
        body.height = 2.0f * body.radius;
    }

    // Before anything else, get out of whatever we are already inside. Without
    // this the first sweep reports "started inside", there is no surface to
    // slide along, and the body never moves again.
    res.position = push_clear(w, &body, res.position, ignore, &res);
    linalg::Vec3 start = res.position;

    slide(w, &body, motion, ignore, &res);

    // stopped by a wall, with a step to try
    if (body.step_height > 0.0f && res.hit && res.normal.y <= MOTION_GROUND_NORMAL_Y)
        try_step(w, &body, start, motion, ignore, &res);

    return res;
}

RayResult ray_cast(entity::World *w, linalg::Vec3 origin, linalg::Vec3 dir, f32 max_distance,
                   entity::Handle ignore) {
    ASSERT(w);

    RayResult res{};
    if (linalg::vec3_length_sq(dir) < LINALG_ZERO_EPSILON) {
        LOG_WARN_ONCE("motion: a ray needs a direction");
        return res;
    }
    dir = linalg::vec3_normalize(dir);

    // a ray is a body of nothing: no radius, no height
    Body point{origin, 0.0f, 0.0f, 0.0f};

    for (u32 s = entity::component_first(w, entity::COMPONENT_COLLIDER); s;
         s = entity::component_next(w, entity::COMPONENT_COLLIDER, s)) {
        entity::Collider *c =
            (entity::Collider *)entity::component_at(w, entity::COMPONENT_COLLIDER, s);

        Contact k;
        if (!contact_of(w, c, &point, origin, ignore, &k))
            continue;

        // the frame is rigid, so the direction stays unit and t stays metres
        intersect::Ray ray{k.center, linalg::mat4_mul_dir(k.inverse, dir)};
        intersect::Hit h;
        b32 hit;
        if (c->shape == entity::COLLIDER_SPHERE)
            hit = intersect::ray_sphere(ray, linalg::Vec3{0.0f, 0.0f, 0.0f}, c->radius,
                                        max_distance, &h);
        else
            hit = intersect::ray_aabb(ray, box_of(c), max_distance, &h);
        if (!hit || (res.hit && h.t >= res.distance))
            continue;

        res.hit = true;
        res.entity = c->owner;
        res.distance = h.t;
        res.point = linalg::mat4_mul_point(k.frame, h.point);
        res.normal = linalg::mat4_mul_dir(k.frame, h.normal);
    }

    return res;
}

} // namespace motion
