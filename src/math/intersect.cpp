#include "math/intersect.h"

namespace intersect {

static inline void to_array(linalg::Vec3 v, f32 out[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

static inline linalg::Vec3 from_array(const f32 v[3]) { return linalg::Vec3{v[0], v[1], v[2]}; }

b32 ray_aabb(Ray r, Aabb box, f32 max_t, Hit *out) {
    f32 o[3];
    f32 d[3];
    f32 lo[3];
    f32 hi[3];
    to_array(r.origin, o);
    to_array(r.dir, d);
    to_array(box.min, lo);
    to_array(box.max, hi);

    f32 t0 = 0.0f;
    f32 t1 = max_t;

    // which slab produced t0, so the normal names the face actually entered
    i32 axis = -1;
    f32 sign = 0.0f;

    for (i32 i = 0; i < 3; i++) {
        if (d[i] > -LINALG_ZERO_EPSILON && d[i] < LINALG_ZERO_EPSILON) {
            // parallel to this pair of planes: inside them for the whole ray, or never
            if (o[i] < lo[i] || o[i] > hi[i])
                return false;
            continue;
        }

        f32 inv = 1.0f / d[i];
        f32 near = (lo[i] - o[i]) * inv;
        f32 far = (hi[i] - o[i]) * inv;

        // travelling backwards along this axis meets the max plane first
        f32 s = -1.0f;
        if (near > far) {
            f32 swap = near;
            near = far;
            far = swap;
            s = 1.0f;
        }

        if (near > t0) {
            t0 = near;
            axis = i;
            sign = s;
        }
        if (far < t1)
            t1 = far;

        if (t0 > t1)
            return false;
    }

    if (out) {
        f32 n[3] = {0.0f, 0.0f, 0.0f};
        if (axis >= 0)
            n[axis] = sign;

        out->t = t0;
        out->point = r.origin + r.dir * t0;

        // axis < 0 means no slab ever pushed t0 forward: the ray started inside
        out->normal = from_array(n);
    }

    return true;
}

b32 ray_sphere(Ray r, linalg::Vec3 center, f32 radius, f32 max_t, Hit *out) {
    linalg::Vec3 m = r.origin - center;

    f32 b = linalg::vec3_dot(m, r.dir);
    f32 c = linalg::vec3_dot(m, m) - radius * radius;

    if (c < 0.0f) { // started inside
        if (out)
            *out = Hit{0.0f, r.origin, linalg::Vec3{0.0f, 0.0f, 0.0f}};
        return true;
    }

    // outside and pointing away
    if (b > 0.0f)
        return false;

    f32 disc = b * b - c;
    if (disc < 0.0f)
        return false;

    f32 t = -b - sqrtf(disc);
    if (t < 0.0f || t > max_t)
        return false;

    if (out) {
        linalg::Vec3 p = r.origin + r.dir * t;
        out->t = t;
        out->point = p;
        out->normal = linalg::vec3_normalize(p - center);
    }

    return true;
}

b32 ray_triangle(Ray r, linalg::Vec3 a, linalg::Vec3 b, linalg::Vec3 c, f32 max_t,
                 b32 cull_backface, Hit *out) {
    linalg::Vec3 e1 = b - a;
    linalg::Vec3 e2 = c - a;

    linalg::Vec3 p = linalg::vec3_cross(r.dir, e2);
    f32 det = linalg::vec3_dot(e1, p);

    if (cull_backface) {
        if (det < INTERSECT_PARALLEL_EPSILON)
            return false;
    } else if (det > -INTERSECT_PARALLEL_EPSILON && det < INTERSECT_PARALLEL_EPSILON) {
        return false;
    }

    f32 inv = 1.0f / det;

    linalg::Vec3 tv = r.origin - a;
    f32 u = linalg::vec3_dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;

    linalg::Vec3 q = linalg::vec3_cross(tv, e1);
    f32 v = linalg::vec3_dot(r.dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    f32 t = linalg::vec3_dot(e2, q) * inv;
    if (t < 0.0f || t > max_t)
        return false;

    if (out) {
        linalg::Vec3 n = linalg::vec3_normalize(linalg::vec3_cross(e1, e2));

        out->t = t;
        out->point = r.origin + r.dir * t;
        out->normal = linalg::vec3_dot(n, r.dir) > 0.0f ? -n : n;
    }

    return true;
}

b32 depenetrate_sphere_aabb(linalg::Vec3 center, f32 radius, Aabb box, linalg::Vec3 *out_normal,
                            f32 *out_depth) {
    linalg::Vec3 d = center - aabb_closest_point(box, center);
    f32 dist_sq = linalg::vec3_length_sq(d);

    // outside the box, or on its surface: the closest point gives the direction
    if (dist_sq > LINALG_ZERO_EPSILON) {
        if (dist_sq >= radius * radius)
            return false;

        f32 dist = sqrtf(dist_sq);

        if (out_normal)
            *out_normal = d * (1.0f / dist);
        if (out_depth)
            *out_depth = radius - dist;

        return true;
    }

    // The centre is inside, so there is no direction from the box to it. Leave
    // by the nearest face: it is the shortest way out, and the only choice that
    // does not look like the body teleported.
    f32 c[3];
    f32 lo[3];
    f32 hi[3];
    to_array(center, c);
    to_array(box.min, lo);
    to_array(box.max, hi);

    i32 axis = 0;
    f32 sign = 1.0f;
    f32 best = 0.0f;
    b32 first = true;

    for (i32 i = 0; i < 3; i++) {
        f32 to_lo = c[i] - lo[i];
        f32 to_hi = hi[i] - c[i];

        if (first || to_lo < best) {
            best = to_lo;
            axis = i;
            sign = -1.0f;
            first = false;
        }
        if (to_hi < best) {
            best = to_hi;
            axis = i;
            sign = 1.0f;
        }
    }

    f32 n[3] = {0.0f, 0.0f, 0.0f};
    n[axis] = sign;

    if (out_normal)
        *out_normal = from_array(n);

    // out of the box, and then clear of it by the radius
    if (out_depth)
        *out_depth = best + radius;

    return true;
}

// One rounded edge of the Minkowski sum: a cylinder of `radius` running along
// `k` between klo and khi, centred at ci/cj on the other two axes. Because the
// cylinder is axis aligned this is a circle test in the plane across it, plus a
// range check along it.
static b32 ray_edge(Ray r, i32 k, f32 ci, f32 cj, f32 klo, f32 khi, f32 radius, f32 max_t,
                    Hit *out) {
    i32 i = (k + 1) % 3;
    i32 j = (k + 2) % 3;

    f32 o[3];
    f32 d[3];
    to_array(r.origin, o);
    to_array(r.dir, d);

    f32 ei = o[i] - ci;
    f32 ej = o[j] - cj;

    f32 aa = d[i] * d[i] + d[j] * d[j];

    // running along the edge rather than across it; the corner spheres at its
    // ends are what it can actually touch
    if (aa < LINALG_ZERO_EPSILON)
        return false;

    f32 bb = ei * d[i] + ej * d[j];
    f32 cc = ei * ei + ej * ej - radius * radius;

    f32 disc = bb * bb - aa * cc;
    if (disc < 0.0f)
        return false;

    f32 t = (-bb - sqrtf(disc)) / aa;
    if (t < 0.0f || t > max_t)
        return false;

    f32 p[3];
    for (i32 x = 0; x < 3; x++)
        p[x] = o[x] + d[x] * t;

    // past an end of the edge: that is a corner, and its sphere answers for it
    if (p[k] < klo || p[k] > khi)
        return false;

    if (out) {
        f32 n[3] = {0.0f, 0.0f, 0.0f};
        n[i] = p[i] - ci;
        n[j] = p[j] - cj;

        out->t = t;
        out->point = from_array(p);
        out->normal = linalg::vec3_normalize(from_array(n));
    }

    return true;
}

b32 sweep_sphere_aabb(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, Aabb box, Hit *out) {
    f32 len = linalg::vec3_length(motion);
    if (len < LINALG_ZERO_EPSILON)
        return false;

    // Genuinely overlapping: there is no surface to have hit, only a way out.
    // This is a distance test and not a containment test on purpose -- see the
    // note further down about the expanded box.
    linalg::Vec3 gap = center - aabb_closest_point(box, center);
    if (linalg::vec3_length_sq(gap) < radius * radius) {
        if (out)
            *out = Hit{0.0f, center, linalg::Vec3{0.0f, 0.0f, 0.0f}};
        return true;
    }

    Ray r{center, motion * (1.0f / len)};

    // The swept shape is the box grown by the sphere: flat faces where the
    // sphere meets a face, rounded along every edge and corner. It sits inside
    // the box grown by radius on every axis, so missing that box misses it.
    Aabb outer = aabb_expand(box, radius);

    Hit h{};
    if (!ray_aabb(r, outer, len, &h))
        return false;

    f32 lo[3];
    f32 hi[3];
    to_array(box.min, lo);
    to_array(box.max, hi);

    // Starting inside the expanded box is not the same as overlapping, and
    // confusing the two freezes anything that touches anything: a body resting
    // against a surface is inside that box for as long as it stays there. Only
    // the real distance answers it, and it was already decided above.
    if (h.t > 0.0f) {
        f32 p[3];
        to_array(h.point, p);

        i32 outside = 0;
        for (i32 i = 0; i < 3; i++)
            if (p[i] < lo[i] || p[i] > hi[i])
                outside++;

        // Entering over a face -- at most one axis past the original box --
        // means the rounded shape and the expanded box coincide here, so the
        // slab test was already the exact answer.
        if (outside <= 1) {
            if (out) {
                *out = h;
                out->t = h.t / len;
            }
            return true;
        }
    }

    // Otherwise the first contact is on a rounded edge or corner. It cannot be
    // on a flat face further along: those lie in the expanded box's own faces,
    // and between entry and exit the ray is strictly inside it. So these twenty
    // features are the whole remaining surface.
    Hit best{};
    b32 found = false;

    for (i32 c = 0; c < 8; c++) {
        linalg::Vec3 corner{
            (c & 1) ? hi[0] : lo[0],
            (c & 2) ? hi[1] : lo[1],
            (c & 4) ? hi[2] : lo[2],
        };

        Hit ch{};
        if (ray_sphere(r, corner, radius, found ? best.t : len, &ch)) {
            best = ch;
            found = true;
        }
    }

    for (i32 k = 0; k < 3; k++) {
        i32 i = (k + 1) % 3;
        i32 j = (k + 2) % 3;

        for (i32 q = 0; q < 4; q++) {
            Hit eh{};
            if (ray_edge(r, k, (q & 1) ? hi[i] : lo[i], (q & 2) ? hi[j] : lo[j], lo[k], hi[k],
                         radius, found ? best.t : len, &eh)) {
                best = eh;
                found = true;
            }
        }
    }

    if (!found)
        return false;

    if (out) {
        *out = best;
        out->t = best.t / len;
    }

    return true;
}

} // namespace intersect
