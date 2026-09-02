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

static b32 collider_box(entity::World *w, asset::Manager *m, entity::Renderable *r,
                        intersect::Aabb *out) {
    entity::Entity *e = entity::get(w, r->owner);
    if (!e)
        return false;

    linalg::Vec3 lo;
    linalg::Vec3 hi;
    asset::model_bounds(m, r->model, &lo, &hi);

    intersect::Aabb local{lo, hi};
    if (!intersect::aabb_valid(local))
        return false;

    *out = intersect::aabb_transform(local, e->world);
    return true;
}

// Everything a sweep cannot answer, because a body that starts inside geometry
// has no surface to have hit. Deepest overlap first: it is the one most likely
// to be the real problem, and leaving it often clears the shallower ones too.
static linalg::Vec3 push_clear(entity::World *w, asset::Manager *m, linalg::Vec3 position,
                               f32 radius, entity::Handle ignore, Result *res) {
    for (u32 i = 0; i < MOTION_MAX_DEPENETRATIONS; i++) {
        f32 worst_depth = 0.0f;
        linalg::Vec3 worst_normal{};
        b32 found = false;

        for (u32 s = entity::component_first(w, entity::COMPONENT_RENDERABLE); s;
             s = entity::component_next(w, entity::COMPONENT_RENDERABLE, s)) {
            entity::Renderable *r =
                (entity::Renderable *)entity::component_at(w, entity::COMPONENT_RENDERABLE, s);

            if (under(w, r->owner, ignore))
                continue;

            intersect::Aabb box;
            if (!collider_box(w, m, r, &box))
                continue;

            linalg::Vec3 n;
            f32 depth;
            if (!intersect::depenetrate_sphere_aabb(position, radius, box, &n, &depth))
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
    res.position = push_clear(w, m, res.position, radius, ignore, &res);

    linalg::Vec3 left = motion;

    for (u32 i = 0; i < MOTION_MAX_SLIDES; i++) {
        if (linalg::vec3_length_sq(left) < LINALG_ZERO_EPSILON)
            break;

        intersect::Hit best{};
        b32 found = false;

        for (u32 s = entity::component_first(w, entity::COMPONENT_RENDERABLE); s;
             s = entity::component_next(w, entity::COMPONENT_RENDERABLE, s)) {
            entity::Renderable *r =
                (entity::Renderable *)entity::component_at(w, entity::COMPONENT_RENDERABLE, s);

            if (under(w, r->owner, ignore))
                continue;

            intersect::Aabb box;
            if (!collider_box(w, m, r, &box))
                continue;

            intersect::Hit h;
            if (intersect::sweep_sphere_aabb(res.position, radius, left, box, &h) &&
                (!found || h.t < best.t)) {
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
