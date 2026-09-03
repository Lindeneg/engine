#include "editor/gizmo.h"

#include <math.h>

// below this the ray runs along the handle and its position on it means nothing
#define PARALLEL_EPSILON 1e-6f

// beyond this many lengths behind the eye a hit is not on screen
#define BEHIND_EYE 0.0f

namespace gizmo {

u32 handle_axis(Handle h) {
    ASSERT(h != HANDLE_NONE && h < HANDLE_COUNT);
    return h < HANDLE_YZ ? (u32)h : (u32)(h - HANDLE_YZ);
}

b32 handle_is_plane(Handle h) { return h >= HANDLE_YZ && h < HANDLE_COUNT; }

// the two axes a plane handle spans, given its normal
static void plane_axes(u32 normal, u32 *out_u, u32 *out_v) {
    *out_u = (normal + 1) % 3;
    *out_v = (normal + 2) % 3;
}

// The parameter along the axis line nearest the ray, or false when parallel.
// Closest points of two lines: solve for s on the axis with both directions unit.
static b32 axis_param(linalg::Vec3 origin, linalg::Vec3 axis, intersect::Ray ray, f32 *out_s) {
    linalg::Vec3 w = origin - ray.origin;
    f32 b = linalg::vec3_dot(axis, ray.dir);
    f32 d = linalg::vec3_dot(axis, w);
    f32 e = linalg::vec3_dot(ray.dir, w);

    f32 denom = 1.0f - b * b;
    if (denom < PARALLEL_EPSILON)
        return false;

    *out_s = (b * e - d) / denom;
    return true;
}

// where the ray meets the plane through origin with the given normal
static b32 plane_point(linalg::Vec3 origin, linalg::Vec3 normal, intersect::Ray ray,
                       linalg::Vec3 *out) {
    f32 facing = linalg::vec3_dot(ray.dir, normal);
    if (fabsf(facing) < PARALLEL_EPSILON)
        return false;

    f32 t = linalg::vec3_dot(origin - ray.origin, normal) / facing;
    if (t < BEHIND_EYE)
        return false;

    *out = ray.origin + ray.dir * t;
    return true;
}

static f32 distance_to_ray(linalg::Vec3 p, intersect::Ray ray) {
    f32 t = linalg::vec3_dot(p - ray.origin, ray.dir);
    if (t < 0.0f)
        t = 0.0f;
    return linalg::vec3_length(p - (ray.origin + ray.dir * t));
}

Handle hit(const Frame *f, intersect::Ray ray, f32 tolerance) {
    ASSERT(f);

    // planes first: a square is a region, and the axis lines run past its corner
    for (u32 n = 0; n < 3; n++) {
        linalg::Vec3 p;
        if (!plane_point(f->origin, f->axis[n], ray, &p))
            continue;

        u32 u, v;
        plane_axes(n, &u, &v);
        f32 a = linalg::vec3_dot(p - f->origin, f->axis[u]);
        f32 b = linalg::vec3_dot(p - f->origin, f->axis[v]);
        f32 lo = f->plane_offset;
        f32 hi = f->plane_offset + f->plane_size;
        if (a >= lo && a <= hi && b >= lo && b <= hi)
            return (Handle)(HANDLE_YZ + n);
    }

    Handle best = HANDLE_NONE;
    f32 best_distance = tolerance;
    for (u32 i = 0; i < 3; i++) {
        f32 s;
        if (!axis_param(f->origin, f->axis[i], ray, &s))
            continue;
        if (s < 0.0f)
            s = 0.0f;
        if (s > f->length)
            s = f->length;

        f32 distance = distance_to_ray(f->origin + f->axis[i] * s, ray);
        if (distance < best_distance) {
            best_distance = distance;
            best = (Handle)i;
        }
    }
    return best;
}

// the point on the handle the ray is pointing at, false when parallel
static b32 reference(const Frame *f, Handle h, intersect::Ray ray, linalg::Vec3 *out) {
    u32 axis = handle_axis(h);
    if (handle_is_plane(h))
        return plane_point(f->origin, f->axis[axis], ray, out);

    f32 s;
    if (!axis_param(f->origin, f->axis[axis], ray, &s))
        return false;
    *out = f->origin + f->axis[axis] * s;
    return true;
}

Drag begin(const Frame *f, Handle h, intersect::Ray ray) {
    ASSERT(f && h != HANDLE_NONE && h < HANDLE_COUNT);

    Drag d{};
    d.frame = *f;
    d.handle = h;
    d.valid = reference(f, h, ray, &d.grab);
    return d;
}

static f32 snapped(f32 v, f32 snap) { return snap > 0.0f ? roundf(v / snap) * snap : v; }

linalg::Vec3 delta(const Drag *d, intersect::Ray ray, f32 snap) {
    ASSERT(d);

    linalg::Vec3 none{0.0f, 0.0f, 0.0f};
    if (!d->valid)
        return none;

    linalg::Vec3 now;
    if (!reference(&d->frame, d->handle, ray, &now))
        return none;

    linalg::Vec3 moved = now - d->grab;
    u32 axis = handle_axis(d->handle);

    if (!handle_is_plane(d->handle)) {
        f32 along = snapped(linalg::vec3_dot(moved, d->frame.axis[axis]), snap);
        return d->frame.axis[axis] * along;
    }

    u32 u, v;
    plane_axes(axis, &u, &v);
    f32 a = snapped(linalg::vec3_dot(moved, d->frame.axis[u]), snap);
    f32 b = snapped(linalg::vec3_dot(moved, d->frame.axis[v]), snap);
    return d->frame.axis[u] * a + d->frame.axis[v] * b;
}

} // namespace gizmo
