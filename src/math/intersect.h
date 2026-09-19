#pragma once

#include "core/common.h"
#include "math/linalg.h"

// Geometric queries: what a ray or a moving sphere runs into. Separate from
// linalg because these answer questions about a world rather than build
// transforms, and because everything here is pure -- no engine types, no
// allocation -- so it can be tested exhaustively without a world existing.
//
// Two different jobs live here and they are held to different standards. A
// bullet is a ray, so the ray tests have to be exact: no epsilon fudging that
// makes a shot land where it did not, and no early-out that misses a thin
// target. Character movement is a swept sphere, and there the requirement is
// weaker but specific -- it must never tunnel, and a miss is worse than a hit
// reported slightly early.
//
// Conventions. A ray direction is unit length; that is the caller's job, and
// every t below is therefore a distance in metres. The one exception is the
// sweep, whose t is a fraction of the motion vector, so the caller advances by
// t * motion. A normal always points back at whatever did the moving. A query
// that starts already overlapping reports t = 0 and a zero normal, because
// there is no meaningful surface to have hit -- that is the caller's signal to
// push out rather than to slide.

// Below this the triangle is degenerate or exactly edge-on, and no answer it
// could give would mean anything.
#define INTERSECT_PARALLEL_EPSILON 1e-8f

namespace intersect {

struct Ray {
    linalg::Vec3 origin;
    linalg::Vec3 dir; // unit length
};

struct Aabb {
    linalg::Vec3 min;
    linalg::Vec3 max;
};

struct Hit {
    f32 t;
    linalg::Vec3 point;

    // unit and facing the ray, or zero when the query started inside
    linalg::Vec3 normal;
};

// Inverted, so merging points into it from nothing produces the right box.
static inline Aabb aabb_empty() {
    return Aabb{linalg::Vec3{1e30f, 1e30f, 1e30f}, linalg::Vec3{-1e30f, -1e30f, -1e30f}};
}

static inline b32 aabb_valid(Aabb a) {
    return a.min.x <= a.max.x && a.min.y <= a.max.y && a.min.z <= a.max.z;
}

static inline Aabb aabb_add_point(Aabb a, linalg::Vec3 p) {
    return Aabb{
        linalg::Vec3{MIN(a.min.x, p.x), MIN(a.min.y, p.y), MIN(a.min.z, p.z)},
        linalg::Vec3{MAX(a.max.x, p.x), MAX(a.max.y, p.y), MAX(a.max.z, p.z)},
    };
}

static inline Aabb aabb_merge(Aabb a, Aabb b) {
    return Aabb{
        linalg::Vec3{MIN(a.min.x, b.min.x), MIN(a.min.y, b.min.y), MIN(a.min.z, b.min.z)},
        linalg::Vec3{MAX(a.max.x, b.max.x), MAX(a.max.y, b.max.y), MAX(a.max.z, b.max.z)},
    };
}

static inline Aabb aabb_expand(Aabb a, f32 r) {
    return Aabb{
        linalg::Vec3{a.min.x - r, a.min.y - r, a.min.z - r},
        linalg::Vec3{a.max.x + r, a.max.y + r, a.max.z + r},
    };
}

static inline linalg::Vec3 aabb_center(Aabb a) { return (a.min + a.max) * 0.5f; }

// Half the size, which is what most box maths wants.
static inline linalg::Vec3 aabb_extents(Aabb a) { return (a.max - a.min) * 0.5f; }

static inline b32 aabb_contains(Aabb a, linalg::Vec3 p) {
    return p.x >= a.min.x && p.x <= a.max.x && p.y >= a.min.y && p.y <= a.max.y && p.z >= a.min.z &&
           p.z <= a.max.z;
}

static inline linalg::Vec3 aabb_closest_point(Aabb a, linalg::Vec3 p) {
    return linalg::Vec3{
        MIN(MAX(p.x, a.min.x), a.max.x),
        MIN(MAX(p.y, a.min.y), a.max.y),
        MIN(MAX(p.z, a.min.z), a.max.z),
    };
}

static inline b32 aabb_overlaps(Aabb a, Aabb b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// The axis-aligned box around a transformed box. Rotation grows it: this is the
// bound of the rotated box, not the rotated bound, and re-transforming a result
// keeps growing it. Take it from the source bounds each time.
static inline Aabb aabb_transform(Aabb a, linalg::Mat4 m) {
    linalg::Vec3 c = linalg::mat4_mul_point(m, aabb_center(a));
    linalg::Vec3 e = aabb_extents(a);

    linalg::Vec3 x = linalg::mat4_axis(m, 0);
    linalg::Vec3 y = linalg::mat4_axis(m, 1);
    linalg::Vec3 z = linalg::mat4_axis(m, 2);

    linalg::Vec3 r{
        e.x * (x.x < 0.0f ? -x.x : x.x) + e.y * (y.x < 0.0f ? -y.x : y.x) +
            e.z * (z.x < 0.0f ? -z.x : z.x),
        e.x * (x.y < 0.0f ? -x.y : x.y) + e.y * (y.y < 0.0f ? -y.y : y.y) +
            e.z * (z.y < 0.0f ? -z.y : z.y),
        e.x * (x.z < 0.0f ? -x.z : x.z) + e.y * (y.z < 0.0f ? -y.z : y.z) +
            e.z * (z.z < 0.0f ? -z.z : z.z),
    };

    return Aabb{c - r, c + r};
}

// Slab test. A ray starting inside reports t = 0 and a zero normal.
b32 ray_aabb(Ray r, Aabb box, f32 max_t, Hit *out);

b32 ray_sphere(Ray r, linalg::Vec3 center, f32 radius, f32 max_t, Hit *out);

// Moller-Trumbore. cull_backface drops triangles facing away, which is what a
// closed mesh wants; without it the normal is flipped to face the ray.
b32 ray_triangle(Ray r, linalg::Vec3 a, linalg::Vec3 b, linalg::Vec3 c, f32 max_t,
                 b32 cull_backface, Hit *out);

// Exact: the expanded box is only used for the flat faces, and the rounded
// edges and corners of the Minkowski sum are tested as cylinders and spheres,
// so a sphere clipping a box corner is not reported as hitting a face early.
// Hit::t is the fraction of motion, in [0, 1].
b32 sweep_sphere_aabb(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, Aabb box, Hit *out);

// A moving sphere against a still one: a ray against the sum of the radii.
// Hit::t is the fraction of motion, the normal points from the still sphere
// toward the moving one, and a start already overlapping reports t = 0 and a
// zero normal, as the box sweep does.
b32 sweep_sphere_sphere(linalg::Vec3 center, f32 radius, linalg::Vec3 motion,
                        linalg::Vec3 other, f32 other_radius, Hit *out);

// The shortest push that takes two overlapping spheres apart, false when they
// do not overlap. Concentric spheres are pushed up, which is a choice, not a fact.
b32 depenetrate_sphere_sphere(linalg::Vec3 center, f32 radius, linalg::Vec3 other,
                              f32 other_radius, linalg::Vec3 *out_normal, f32 *out_depth);

// The shortest push that takes an already-overlapping sphere clear of a box,
// false when they do not overlap. The normal points from the box toward the
// sphere, so moving by normal * depth leaves it exactly touching. A sweep
// cannot answer this -- it has no surface to report -- and something has to,
// because bodies do end up inside geometry: spawned there, drifted there, or
// put there by something that moved.
b32 depenetrate_sphere_aabb(linalg::Vec3 center, f32 radius, Aabb box, linalg::Vec3 *out_normal,
                            f32 *out_depth);

// A convex polytope: outward planes, vertices and edges. What a box becomes to
// a capsule, since a moving capsule against a box is a moving sphere against
// the box extruded along the capsule's axis, and that extrusion is convex.
// The sizes fit that shape: two boxes' corners, their edges and the edges
// between them, the six faces and up to six between them.
#define INTERSECT_POLYTOPE_MAX_VERTICES 16
#define INTERSECT_POLYTOPE_MAX_EDGES 32
#define INTERSECT_POLYTOPE_MAX_PLANES 12

// how far a point may sit outside a plane and still count as on it
#define INTERSECT_PLANE_EPSILON 1e-5f

struct Plane {
    linalg::Vec3 n; // unit, outward
    f32 d;          // n . p == d on the plane
};

struct PolytopeEdge {
    u32 a;
    u32 b;
};

struct Polytope {
    Plane planes[INTERSECT_POLYTOPE_MAX_PLANES];
    u32 plane_count;
    linalg::Vec3 vertices[INTERSECT_POLYTOPE_MAX_VERTICES];
    u32 vertex_count;
    PolytopeEdge edges[INTERSECT_POLYTOPE_MAX_EDGES];
    u32 edge_count;
    Aabb bounds;
};

// The hull of the box and the box moved by d. A zero d is the box itself.
void polytope_extrude_box(Aabb box, linalg::Vec3 d, Polytope *out);

// Inside or on the surface.
b32 polytope_contains(const Polytope *p, linalg::Vec3 point);

// The point itself when inside; else the nearest of its faces, edges and vertices.
linalg::Vec3 polytope_closest_point(const Polytope *p, linalg::Vec3 point);

// Exact, as the box sweep is: the faces offset by the radius, the edges as
// cylinders, the vertices as spheres, and the nearest of them all. Hit::t is
// the fraction of motion; Hit::point is where the sphere's centre stands at
// contact. A start already overlapping reports t = 0 and a zero normal.
b32 sweep_sphere_polytope(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, const Polytope *p,
                          Hit *out);

// As depenetrate_sphere_aabb, for a polytope: from outside by the closest
// point, from inside by the nearest plane.
b32 depenetrate_sphere_polytope(linalg::Vec3 center, f32 radius, const Polytope *p,
                                linalg::Vec3 *out_normal, f32 *out_depth);

// A capsule is a segment with a radius; a capsule against a sphere is a
// sphere against a capsule with the roles swapped, which these answer.
linalg::Vec3 closest_point_segment(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec3 p);

// The sphere against the still capsule from a to b: a ray against the
// segment's cylinder and end spheres with the summed radius. Conventions as
// sweep_sphere_polytope.
b32 sweep_sphere_capsule(linalg::Vec3 center, f32 radius, linalg::Vec3 motion, linalg::Vec3 a,
                         linalg::Vec3 b, f32 capsule_radius, Hit *out);

b32 depenetrate_sphere_capsule(linalg::Vec3 center, f32 radius, linalg::Vec3 a, linalg::Vec3 b,
                               f32 capsule_radius, linalg::Vec3 *out_normal, f32 *out_depth);

} // namespace intersect
