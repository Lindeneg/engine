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

b32 sweep_sphere_sphere(linalg::Vec3 center, f32 radius, linalg::Vec3 motion,
                        linalg::Vec3 other, f32 other_radius, Hit *out) {
    f32 len = linalg::vec3_length(motion);
    if (len < LINALG_ZERO_EPSILON)
        return false;

    f32 sum = radius + other_radius;
    Ray r{center, motion * (1.0f / len)};
    Hit h;
    if (!ray_sphere(r, other, sum, len, &h))
        return false;

    if (out) {
        // ray_sphere reports a start inside with a zero normal; keep that meaning
        b32 inside = linalg::vec3_length_sq(h.normal) < LINALG_ZERO_EPSILON;
        linalg::Vec3 at = center + r.dir * h.t;
        linalg::Vec3 n = inside ? linalg::Vec3{0.0f, 0.0f, 0.0f}
                                : linalg::vec3_normalize_or(at - other, linalg::Vec3{0, 1, 0});
        *out = Hit{h.t / len, other + n * other_radius, n};
    }
    return true;
}

b32 depenetrate_sphere_sphere(linalg::Vec3 center, f32 radius, linalg::Vec3 other,
                              f32 other_radius, linalg::Vec3 *out_normal, f32 *out_depth) {
    f32 sum = radius + other_radius;
    linalg::Vec3 d = center - other;
    f32 dist = linalg::vec3_length(d);
    if (dist >= sum)
        return false;

    if (out_normal)
        *out_normal = linalg::vec3_normalize_or(d, linalg::Vec3{0.0f, 1.0f, 0.0f});
    if (out_depth)
        *out_depth = sum - dist;
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

// A ray against the cylinder of `radius` about the segment a-b, between its
// ends; the end spheres answer for anything past them. The axial component
// is removed and what is left is a circle test in the plane across the axis.
static b32 ray_cylinder(Ray r, linalg::Vec3 a, linalg::Vec3 b, f32 radius, f32 max_t, Hit *out) {
    linalg::Vec3 ab = b - a;
    f32 len_sq = linalg::vec3_length_sq(ab);
    if (len_sq < LINALG_ZERO_EPSILON)
        return false;
    linalg::Vec3 u = ab * (1.0f / sqrtf(len_sq));

    linalg::Vec3 m = r.origin - a;
    linalg::Vec3 dp = r.dir - u * linalg::vec3_dot(r.dir, u);
    linalg::Vec3 mp = m - u * linalg::vec3_dot(m, u);

    f32 aa = linalg::vec3_dot(dp, dp);
    if (aa < LINALG_ZERO_EPSILON) // running along the axis
        return false;

    f32 bb = linalg::vec3_dot(mp, dp);
    f32 cc = linalg::vec3_dot(mp, mp) - radius * radius;

    f32 disc = bb * bb - aa * cc;
    if (disc < 0.0f)
        return false;

    f32 t = (-bb - sqrtf(disc)) / aa;
    if (t < 0.0f || t > max_t)
        return false;

    linalg::Vec3 p = r.origin + r.dir * t;
    f32 s = linalg::vec3_dot(p - a, u);
    if (s < 0.0f || s * s > len_sq)
        return false;

    if (out) {
        out->t = t;
        out->point = p;
        out->normal = linalg::vec3_normalize(p - (a + u * s));
    }
    return true;
}

static inline linalg::Vec3 aabb_corner(Aabb box, u32 c) {
    return linalg::Vec3{
        (c & 1) ? box.max.x : box.min.x,
        (c & 2) ? box.max.y : box.min.y,
        (c & 4) ? box.max.z : box.min.z,
    };
}

static const linalg::Vec3 AXES[3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
};

static void add_plane(Polytope *p, linalg::Vec3 n, f32 d) {
    ASSERT(p->plane_count < INTERSECT_POLYTOPE_MAX_PLANES);
    p->planes[p->plane_count++] = Plane{n, d};
}

static void add_edge(Polytope *p, u32 a, u32 b) {
    ASSERT(p->edge_count < INTERSECT_POLYTOPE_MAX_EDGES);
    p->edges[p->edge_count++] = PolytopeEdge{a, b};
}

void polytope_extrude_box(Aabb box, linalg::Vec3 d, Polytope *out) {
    ASSERT(out);
    *out = Polytope{};

    // the corners of both boxes; a vertex the hull does not need is a sphere
    // inside the solid, which no sweep can reach first
    for (u32 c = 0; c < 8; c++) {
        out->vertices[c] = aabb_corner(box, c);
        out->vertices[8 + c] = out->vertices[c] + d;
    }
    out->vertex_count = 16;

    for (u32 c = 0; c < 8; c++) {
        for (u32 axis = 0; axis < 3; axis++) {
            u32 bit = 1u << axis;
            if (c & bit)
                continue;
            add_edge(out, c, c | bit);
            add_edge(out, 8 + c, 8 + (c | bit));
        }
        add_edge(out, c, 8 + c);
    }

    // the box's faces, moved by d where d leaves through them and kept where
    // it enters; a face d runs along keeps its plane and merely grows
    f32 lo[3];
    f32 hi[3];
    to_array(box.min, lo);
    to_array(box.max, hi);
    f32 shift[3][2]; // per axis, per sign: how far n . d moves the plane out
    for (u32 axis = 0; axis < 3; axis++) {
        f32 s = linalg::vec3_dot(AXES[axis], d);
        shift[axis][0] = s < 0.0f ? -s : 0.0f; // the -axis face moves when d points -axis
        shift[axis][1] = s > 0.0f ? s : 0.0f;
        add_plane(out, -1.0f * AXES[axis], -lo[axis] + shift[axis][0]);
        add_plane(out, AXES[axis], hi[axis] + shift[axis][1]);
    }

    // one parallelogram per silhouette edge: an edge whose two faces d leaves
    // through and enters through respectively. Edge k of corner c runs along
    // axis k; the faces beside it are on the other two axes, on c's sides.
    linalg::Vec3 centroid = aabb_center(box) + d * 0.5f;
    for (u32 c = 0; c < 8; c++) {
        for (u32 k = 0; k < 3; k++) {
            if (c & (1u << k))
                continue;
            u32 i = (k + 1) % 3;
            u32 j = (k + 2) % 3;
            f32 si = linalg::vec3_dot(AXES[i], d) * ((c & (1u << i)) ? 1.0f : -1.0f);
            f32 sj = linalg::vec3_dot(AXES[j], d) * ((c & (1u << j)) ? 1.0f : -1.0f);
            if (si * sj >= 0.0f) // both leave, both enter, or one runs along: no new face
                continue;

            linalg::Vec3 n = linalg::vec3_cross(AXES[k], d);
            if (linalg::vec3_length_sq(n) < LINALG_ZERO_EPSILON)
                continue;
            n = linalg::vec3_normalize(n);

            linalg::Vec3 a = out->vertices[c];
            if (linalg::vec3_dot(n, centroid - a) > 0.0f)
                n = -1.0f * n;
            add_plane(out, n, linalg::vec3_dot(n, a));
        }
    }

    out->bounds = aabb_merge(box, Aabb{box.min + d, box.max + d});
}

b32 polytope_contains(const Polytope *p, linalg::Vec3 point) {
    ASSERT(p);
    for (u32 i = 0; i < p->plane_count; i++)
        if (linalg::vec3_dot(p->planes[i].n, point) - p->planes[i].d > INTERSECT_PLANE_EPSILON)
            return false;
    return true;
}

linalg::Vec3 closest_point_segment(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec3 p) {
    linalg::Vec3 ab = b - a;
    f32 len_sq = linalg::vec3_length_sq(ab);
    if (len_sq < LINALG_ZERO_EPSILON)
        return a;
    f32 t = linalg::vec3_dot(p - a, ab) / len_sq;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return a + ab * t;
}

// The nearest boundary point of a convex solid lies on a face, an edge or a
// vertex, and every candidate below is a point of the solid, so the nearest
// candidate is the answer.
linalg::Vec3 polytope_closest_point(const Polytope *p, linalg::Vec3 point) {
    ASSERT(p);
    if (polytope_contains(p, point))
        return point;

    linalg::Vec3 best = point;
    f32 best_sq = 0.0f;
    b32 found = false;

    for (u32 i = 0; i < p->plane_count; i++) {
        const Plane *pl = &p->planes[i];
        linalg::Vec3 q = point - pl->n * (linalg::vec3_dot(pl->n, point) - pl->d);
        if (!polytope_contains(p, q))
            continue;
        f32 d_sq = linalg::vec3_length_sq(q - point);
        if (!found || d_sq < best_sq) {
            best = q;
            best_sq = d_sq;
            found = true;
        }
    }
    for (u32 i = 0; i < p->edge_count; i++) {
        linalg::Vec3 q =
            closest_point_segment(p->vertices[p->edges[i].a], p->vertices[p->edges[i].b], point);
        f32 d_sq = linalg::vec3_length_sq(q - point);
        if (!found || d_sq < best_sq) {
            best = q;
            best_sq = d_sq;
            found = true;
        }
    }
    for (u32 i = 0; i < p->vertex_count; i++) {
        f32 d_sq = linalg::vec3_length_sq(p->vertices[i] - point);
        if (!found || d_sq < best_sq) {
            best = p->vertices[i];
            best_sq = d_sq;
            found = true;
        }
    }
    return best;
}

b32 sweep_sphere_polytope(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, const Polytope *p,
                          Hit *out) {
    ASSERT(p);

    f32 len = linalg::vec3_length(motion);
    if (len < LINALG_ZERO_EPSILON)
        return false;

    linalg::Vec3 gap = center - polytope_closest_point(p, center);
    if (linalg::vec3_length_sq(gap) < radius * radius) {
        if (out)
            *out = Hit{0.0f, center, linalg::Vec3{0.0f, 0.0f, 0.0f}};
        return true;
    }

    Ray r{center, motion * (1.0f / len)};
    if (!ray_aabb(r, aabb_expand(p->bounds, radius), len, NULL))
        return false;

    // Every feature below is a piece of the rounded solid, and its boundary is
    // made of nothing else, so the first of them along the ray is the entry.
    Hit best{};
    b32 found = false;

    // a face, offset by the radius, where the point behind it is still on the face
    for (u32 i = 0; i < p->plane_count; i++) {
        const Plane *pl = &p->planes[i];
        f32 denom = linalg::vec3_dot(pl->n, r.dir);
        if (denom >= 0.0f) // moving away from, or along, this face
            continue;
        f32 t = (pl->d + radius - linalg::vec3_dot(pl->n, center)) / denom;
        if (t < 0.0f || t > (found ? best.t : len))
            continue;
        linalg::Vec3 at = center + r.dir * t;
        if (!polytope_contains(p, at - pl->n * radius))
            continue;
        best = Hit{t, at, pl->n};
        found = true;
    }

    for (u32 i = 0; i < p->edge_count; i++) {
        Hit h{};
        if (ray_cylinder(r, p->vertices[p->edges[i].a], p->vertices[p->edges[i].b], radius,
                         found ? best.t : len, &h)) {
            best = h;
            found = true;
        }
    }

    for (u32 i = 0; i < p->vertex_count; i++) {
        Hit h{};
        if (ray_sphere(r, p->vertices[i], radius, found ? best.t : len, &h)) {
            best = h;
            found = true;
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

b32 depenetrate_sphere_polytope(linalg::Vec3 center, f32 radius, const Polytope *p,
                                linalg::Vec3 *out_normal, f32 *out_depth) {
    ASSERT(p);

    if (!polytope_contains(p, center)) {
        linalg::Vec3 d = center - polytope_closest_point(p, center);
        f32 dist_sq = linalg::vec3_length_sq(d);
        if (dist_sq >= radius * radius)
            return false;
        f32 dist = sqrtf(dist_sq);
        if (out_normal)
            *out_normal = dist > LINALG_ZERO_EPSILON ? d * (1.0f / dist)
                                                      : linalg::Vec3{0.0f, 1.0f, 0.0f};
        if (out_depth)
            *out_depth = radius - dist;
        return true;
    }

    // inside: out by the nearest plane, as the box does
    const Plane *nearest = &p->planes[0];
    f32 best = linalg::vec3_dot(nearest->n, center) - nearest->d; // at most 0
    for (u32 i = 1; i < p->plane_count; i++) {
        f32 s = linalg::vec3_dot(p->planes[i].n, center) - p->planes[i].d;
        if (s > best) {
            best = s;
            nearest = &p->planes[i];
        }
    }
    if (out_normal)
        *out_normal = nearest->n;
    if (out_depth)
        *out_depth = -best + radius;
    return true;
}

b32 sweep_sphere_capsule(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, linalg::Vec3 a,
                         linalg::Vec3 b, f32 capsule_radius, Hit *out) {
    f32 len = linalg::vec3_length(motion);
    if (len < LINALG_ZERO_EPSILON)
        return false;

    f32 sum = radius + capsule_radius;
    linalg::Vec3 gap = center - closest_point_segment(a, b, center);
    if (linalg::vec3_length_sq(gap) < sum * sum) {
        if (out)
            *out = Hit{0.0f, center, linalg::Vec3{0.0f, 0.0f, 0.0f}};
        return true;
    }

    Ray r{center, motion * (1.0f / len)};
    Hit best{};
    b32 found = false;

    Hit h{};
    if (ray_cylinder(r, a, b, sum, len, &h)) {
        best = h;
        found = true;
    }
    if (ray_sphere(r, a, sum, found ? best.t : len, &h)) {
        best = h;
        found = true;
    }
    if (ray_sphere(r, b, sum, found ? best.t : len, &h)) {
        best = h;
        found = true;
    }

    if (!found)
        return false;

    if (out) {
        *out = best;
        out->t = best.t / len;
    }
    return true;
}

b32 depenetrate_sphere_capsule(linalg::Vec3 center, f32 radius, linalg::Vec3 a, linalg::Vec3 b,
                               f32 capsule_radius, linalg::Vec3 *out_normal, f32 *out_depth) {
    f32 sum = radius + capsule_radius;
    linalg::Vec3 d = center - closest_point_segment(a, b, center);
    f32 dist = linalg::vec3_length(d);
    if (dist >= sum)
        return false;

    if (out_normal)
        *out_normal = linalg::vec3_normalize_or(d, linalg::Vec3{0.0f, 1.0f, 0.0f});
    if (out_depth)
        *out_depth = sum - dist;
    return true;
}

} // namespace intersect
