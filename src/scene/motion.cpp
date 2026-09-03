#include "scene/motion.h"

#include "core/logger.h"
#include "math/intersect.h"

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

// A sphere against one collider, in the collider's own frame: the frame is
// rigid, so the sphere stays a sphere there and a box is axis-aligned, and the
// answer comes back through the same frame. `motion` may be zero for a
// depenetration query.
struct Contact {
    entity::Collider *collider;
    linalg::Mat4 frame;
    linalg::Mat4 inverse;
    linalg::Vec3 center; // the body, in the collider's frame
};

static b32 contact_of(entity::World *w, entity::Collider *c, linalg::Vec3 body,
                      entity::Handle ignore, Contact *out) {
    entity::Entity *e = entity::get(w, c->owner);
    if (!e || !e->active || under(w, c->owner, ignore))
        return false;

    out->collider = c;
    out->frame = entity::collider_frame(w, c);
    out->inverse = linalg::mat4_inverse_affine(out->frame);
    out->center = linalg::mat4_mul_point(out->inverse, body);
    return true;
}

static intersect::Aabb box_of(const entity::Collider *c) {
    linalg::Vec3 half = c->size * 0.5f;
    return intersect::Aabb{-1.0f * half, half};
}

static b32 sweep(const Contact *k, f32 radius, linalg::Vec3 motion, intersect::Hit *out) {
    linalg::Vec3 local_motion = linalg::mat4_mul_dir(k->inverse, motion);

    intersect::Hit h;
    b32 hit;
    if (k->collider->shape == entity::COLLIDER_SPHERE)
        hit = intersect::sweep_sphere_sphere(k->center, radius, local_motion,
                                             linalg::Vec3{0.0f, 0.0f, 0.0f},
                                             k->collider->radius, &h);
    else
        hit = intersect::sweep_sphere_aabb(k->center, radius, local_motion, box_of(k->collider),
                                           &h);
    if (!hit)
        return false;

    // t is a fraction either way; the point and normal go back to the world
    out->t = h.t;
    out->point = linalg::mat4_mul_point(k->frame, h.point);
    out->normal = linalg::mat4_mul_dir(k->frame, h.normal);
    return true;
}

static b32 depenetrate(const Contact *k, f32 radius, linalg::Vec3 *out_normal, f32 *out_depth) {
    linalg::Vec3 n;
    b32 overlapping;
    if (k->collider->shape == entity::COLLIDER_SPHERE)
        overlapping = intersect::depenetrate_sphere_sphere(
            k->center, radius, linalg::Vec3{0.0f, 0.0f, 0.0f}, k->collider->radius, &n, out_depth);
    else
        overlapping =
            intersect::depenetrate_sphere_aabb(k->center, radius, box_of(k->collider), &n, out_depth);
    if (!overlapping)
        return false;

    *out_normal = linalg::mat4_mul_dir(k->frame, n);
    return true;
}

// Everything a sweep cannot answer, because a body that starts inside geometry
// has no surface to have hit. Deepest overlap first: it is the one most likely
// to be the real problem, and leaving it often clears the shallower ones too.
static linalg::Vec3 push_clear(entity::World *w, linalg::Vec3 position, f32 radius,
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
            if (!contact_of(w, c, position, ignore, &k))
                continue;

            linalg::Vec3 n;
            f32 depth;
            if (!depenetrate(&k, radius, &n, &depth))
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

Result move_and_slide(entity::World *w, asset::Manager *m, linalg::Vec3 position, f32 radius,
                      linalg::Vec3 motion, entity::Handle ignore) {
    ASSERT(w && m);

    Result res{};
    res.position = position;

    if (radius <= 0.0f) {
        LOG_WARN_ONCE("motion: a body needs a positive radius, not %.3f", (f64)radius);
        return res;
    }

    // Before anything else, get out of whatever we are already inside. Without
    // this the first sweep reports "started inside", there is no surface to
    // slide along, and the body never moves again.
    res.position = push_clear(w, res.position, radius, ignore, &res);

    linalg::Vec3 left = motion;

    for (u32 i = 0; i < MOTION_MAX_SLIDES; i++) {
        if (linalg::vec3_length_sq(left) < LINALG_ZERO_EPSILON)
            break;

        intersect::Hit best{};
        b32 found = false;

        for (u32 s = entity::component_first(w, entity::COMPONENT_COLLIDER); s;
             s = entity::component_next(w, entity::COMPONENT_COLLIDER, s)) {
            entity::Collider *c =
                (entity::Collider *)entity::component_at(w, entity::COMPONENT_COLLIDER, s);

            Contact k;
            if (!contact_of(w, c, res.position, ignore, &k))
                continue;

            intersect::Hit h;
            if (sweep(&k, radius, left, &h) && (!found || h.t < best.t)) {
                best = h;
                found = true;
            }
        }

        if (!found) {
            res.position = res.position + left;
            break;
        }

        // A zero normal is the sweep saying it started inside. push_clear should
        // have made that impossible, so reaching it means a slide put us inside
        // something the push could not resolve.
        if (linalg::vec3_length_sq(best.normal) < LINALG_ZERO_EPSILON) {
            res.stuck = true;
            break;
        }

        res.hit = true;
        res.normal = best.normal;

        if (best.normal.y > MOTION_GROUND_NORMAL_Y)
            res.grounded = true;

        // up to the contact, then a skin clear of it
        res.position = res.position + left * best.t + best.normal * MOTION_SKIN;

        // and on with what is left, flattened against what was hit
        linalg::Vec3 rest = left * (1.0f - best.t);
        left = rest - best.normal * linalg::vec3_dot(rest, best.normal);
    }

    return res;
}

} // namespace motion
