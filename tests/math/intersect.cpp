#include "math/intersect.h"

#include "suites.h"

using linalg::Vec3;

#define EPS 1e-4f

static b32 near_f32(f32 a, f32 b) { return (a - b) < EPS && (b - a) < EPS; }

static b32 near_vec3(Vec3 a, Vec3 b) {
    return near_f32(a.x, b.x) && near_f32(a.y, b.y) && near_f32(a.z, b.z);
}

static intersect::Aabb unit_box() {
    return intersect::Aabb{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}};
}

static intersect::Ray ray_to(Vec3 origin, Vec3 dir) {
    return intersect::Ray{origin, linalg::vec3_normalize(dir)};
}

// Distance from a point to a box, zero inside it. The sweep's whole contract is
// that this equals the radius at the moment of contact, whichever feature of
// the box was hit, so the tests check that rather than hand-computed numbers.
static f32 dist_to_box(Vec3 p, intersect::Aabb b) {
    f32 dx = MAX(MAX(b.min.x - p.x, 0.0f), p.x - b.max.x);
    f32 dy = MAX(MAX(b.min.y - p.y, 0.0f), p.y - b.max.y);
    f32 dz = MAX(MAX(b.min.z - p.z, 0.0f), p.z - b.max.z);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void ray_aabb_enters_the_near_face(memory::Arena *) {
    intersect::Hit h{};
    CHECK(intersect::ray_aabb(ray_to(Vec3{-2.0f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                              100.0f, &h));
    CHECK(near_f32(h.t, 2.0f));
    CHECK(near_vec3(h.point, Vec3{0.0f, 0.5f, 0.5f}));
    CHECK(near_vec3(h.normal, Vec3{-1.0f, 0.0f, 0.0f}));

    // and from the other side the normal flips with it
    CHECK(intersect::ray_aabb(ray_to(Vec3{3.0f, 0.5f, 0.5f}, Vec3{-1.0f, 0.0f, 0.0f}), unit_box(),
                              100.0f, &h));
    CHECK(near_f32(h.t, 2.0f));
    CHECK(near_vec3(h.normal, Vec3{1.0f, 0.0f, 0.0f}));
}

static void ray_aabb_misses_and_respects_max_t(memory::Arena *) {
    // alongside the box, never crossing it
    CHECK(!intersect::ray_aabb(ray_to(Vec3{-2.0f, 2.0f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                               100.0f, NULL));

    // behind the ray
    CHECK(!intersect::ray_aabb(ray_to(Vec3{-2.0f, 0.5f, 0.5f}, Vec3{-1.0f, 0.0f, 0.0f}), unit_box(),
                               100.0f, NULL));

    // in front, but further than we asked about
    CHECK(!intersect::ray_aabb(ray_to(Vec3{-2.0f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                               1.5f, NULL));
    CHECK(intersect::ray_aabb(ray_to(Vec3{-2.0f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                              2.5f, NULL));
}

static void ray_aabb_from_inside_reports_no_surface(memory::Arena *) {
    intersect::Hit h{};
    CHECK(intersect::ray_aabb(ray_to(Vec3{0.5f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                              100.0f, &h));
    CHECK(near_f32(h.t, 0.0f));
    CHECK(near_vec3(h.normal, Vec3{0.0f, 0.0f, 0.0f}));
}

static void ray_aabb_parallel_to_a_slab(memory::Arena *) {
    // running along +X with y outside the box: the y slab rejects it outright,
    // which is the division-by-zero case the slab test has to survive
    CHECK(!intersect::ray_aabb(ray_to(Vec3{-2.0f, 5.0f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                               100.0f, NULL));
    CHECK(intersect::ray_aabb(ray_to(Vec3{-2.0f, 1.0f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}), unit_box(),
                              100.0f, NULL));
}

static void ray_sphere_hits_the_near_side(memory::Arena *) {
    intersect::Hit h{};
    CHECK(intersect::ray_sphere(ray_to(Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}),
                                Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 100.0f, &h));
    CHECK(near_f32(h.t, 3.0f));
    CHECK(near_vec3(h.normal, Vec3{-1.0f, 0.0f, 0.0f}));

    // grazing outside the radius
    CHECK(!intersect::ray_sphere(ray_to(Vec3{-5.0f, 2.5f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}),
                                 Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 100.0f, NULL));

    // pointing away
    CHECK(!intersect::ray_sphere(ray_to(Vec3{-5.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}),
                                 Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 100.0f, NULL));

    // started inside
    CHECK(intersect::ray_sphere(ray_to(Vec3{0.5f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}),
                                Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 100.0f, &h));
    CHECK(near_f32(h.t, 0.0f));
    CHECK(near_vec3(h.normal, Vec3{0.0f, 0.0f, 0.0f}));
}

static void ray_triangle_hits_inside_the_winding(memory::Arena *) {
    Vec3 a{0.0f, 0.0f, 0.0f};
    Vec3 b{2.0f, 0.0f, 0.0f};
    Vec3 c{0.0f, 2.0f, 0.0f};

    // straight down the -Z axis into the middle of the triangle
    intersect::Hit h{};
    CHECK(intersect::ray_triangle(ray_to(Vec3{0.25f, 0.25f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}), a, b,
                                  c, 100.0f, false, &h));
    CHECK(near_f32(h.t, 3.0f));
    CHECK(near_vec3(h.point, Vec3{0.25f, 0.25f, 0.0f}));

    // the normal always faces the ray, whichever side it came from
    CHECK(near_vec3(h.normal, Vec3{0.0f, 0.0f, 1.0f}));
    CHECK(intersect::ray_triangle(ray_to(Vec3{0.25f, 0.25f, -3.0f}, Vec3{0.0f, 0.0f, 1.0f}), a, b,
                                  c, 100.0f, false, &h));
    CHECK(near_vec3(h.normal, Vec3{0.0f, 0.0f, -1.0f}));

    // just past the hypotenuse: u + v > 1
    CHECK(!intersect::ray_triangle(ray_to(Vec3{1.5f, 1.5f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}), a, b, c,
                                   100.0f, false, NULL));

    // and outside the other two edges
    CHECK(!intersect::ray_triangle(ray_to(Vec3{-0.1f, 0.5f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}), a, b,
                                   c, 100.0f, false, NULL));
    CHECK(!intersect::ray_triangle(ray_to(Vec3{0.5f, -0.1f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}), a, b,
                                   c, 100.0f, false, NULL));
}

static void ray_triangle_culls_the_back(memory::Arena *) {
    Vec3 a{0.0f, 0.0f, 0.0f};
    Vec3 b{2.0f, 0.0f, 0.0f};
    Vec3 c{0.0f, 2.0f, 0.0f};

    // from +Z the winding faces us, from -Z it does not
    CHECK(intersect::ray_triangle(ray_to(Vec3{0.25f, 0.25f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}), a, b,
                                  c, 100.0f, true, NULL));
    CHECK(!intersect::ray_triangle(ray_to(Vec3{0.25f, 0.25f, -3.0f}, Vec3{0.0f, 0.0f, 1.0f}), a, b,
                                   c, 100.0f, true, NULL));

    // edge-on is never a hit either way
    CHECK(!intersect::ray_triangle(ray_to(Vec3{-1.0f, 0.5f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}), a, b, c,
                                   100.0f, false, NULL));
}

static void sweep_stops_a_radius_short_of_a_face(memory::Arena *) {
    intersect::Hit h{};
    CHECK(intersect::sweep_sphere_aabb(Vec3{-2.0f, 0.5f, 0.5f}, 0.5f, Vec3{4.0f, 0.0f, 0.0f},
                                       unit_box(), &h));

    // 2 units to travel of the 4 asked for
    CHECK(near_f32(h.t, 0.375f));
    CHECK(near_vec3(h.normal, Vec3{-1.0f, 0.0f, 0.0f}));

    Vec3 at = Vec3{-2.0f, 0.5f, 0.5f} + Vec3{4.0f, 0.0f, 0.0f} * h.t;
    CHECK(near_f32(dist_to_box(at, unit_box()), 0.5f));
}

// The one that separates an exact sweep from expanding the box and casting a
// ray at it: approaching a corner, the expanded box reports contact as soon as
// the ray crosses its face, which is too early and gives a face normal for what
// is really a corner.
static void sweep_rounds_the_corners(memory::Arena *) {
    intersect::Aabb box = unit_box();
    const f32 radius = 0.5f;

    Vec3 starts[] = {
        {-1.5f, -1.5f, 0.5f},  // toward an edge, square on
        {-1.2f, -1.2f, -1.2f}, // toward a corner, along the diagonal
        {-1.5f, -0.7f, 0.5f},  // grazing an edge
        {-2.0f, -0.9f, -0.9f}, // grazing a corner
    };
    Vec3 motions[] = {
        {3.0f, 3.0f, 0.0f},
        {3.0f, 3.0f, 3.0f},
        {3.0f, 1.2f, 0.0f},
        {4.0f, 1.6f, 1.6f},
    };

    for (u32 i = 0; i < ARRAY_COUNT(starts); i++) {
        intersect::Hit h{};
        if (!intersect::sweep_sphere_aabb(starts[i], radius, motions[i], box, &h))
            continue; // a genuine miss is allowed; a wrong contact is not

        CHECK(h.t >= 0.0f && h.t <= 1.0f);

        // whatever it touched, it touched at exactly one radius
        Vec3 at = starts[i] + motions[i] * h.t;
        CHECK(near_f32(dist_to_box(at, box), radius));

        // and the normal points from the box back at the sphere
        CHECK(near_f32(linalg::vec3_length(h.normal), 1.0f));
        CHECK(linalg::vec3_dot(h.normal, linalg::vec3_normalize(motions[i])) < 0.0f);
    }
}

static void sweep_past_a_corner_is_not_a_hit(memory::Arena *) {
    // Passes through the expanded box the whole way -- an implementation that
    // only grew the box and cast a ray would report a hit here -- but stays
    // further than a radius from every part of the real one.
    CHECK(!intersect::sweep_sphere_aabb(Vec3{-3.0f, -1.0f, 0.5f}, 0.5f, Vec3{6.0f, 0.3f, 0.0f},
                                        unit_box(), NULL));
}

static void sweep_does_not_tunnel(memory::Arena *) {
    // a thin slab and a step far bigger than it
    intersect::Aabb wall{Vec3{0.0f, -10.0f, -10.0f}, Vec3{0.02f, 10.0f, 10.0f}};

    intersect::Hit h{};
    CHECK(intersect::sweep_sphere_aabb(Vec3{-50.0f, 0.0f, 0.0f}, 0.25f, Vec3{500.0f, 0.0f, 0.0f},
                                       wall, &h));

    Vec3 at = Vec3{-50.0f, 0.0f, 0.0f} + Vec3{500.0f, 0.0f, 0.0f} * h.t;
    CHECK(near_f32(dist_to_box(at, wall), 0.25f));
}

static void sweep_already_touching_reports_no_surface(memory::Arena *) {
    intersect::Hit h{};
    CHECK(intersect::sweep_sphere_aabb(Vec3{0.5f, 0.5f, 0.5f}, 0.5f, Vec3{1.0f, 0.0f, 0.0f},
                                       unit_box(), &h));
    CHECK(near_f32(h.t, 0.0f));
    CHECK(near_vec3(h.normal, Vec3{0.0f, 0.0f, 0.0f}));

    // standing still is not a sweep, whatever it overlaps
    CHECK(!intersect::sweep_sphere_aabb(Vec3{0.5f, 0.5f, 0.5f}, 0.5f, Vec3{0.0f, 0.0f, 0.0f},
                                        unit_box(), NULL));
}

// Resting against something puts a body inside that thing's expanded box and
// leaves it there. Reading that as an overlap reports a zero normal every step,
// which a caller can only take as "wedged" -- so it stops moving and, since it
// never moves, never stops resting against it either.
static void sweep_touching_is_not_overlapping(memory::Arena *) {
    intersect::Aabb box = unit_box();
    const f32 radius = 0.5f;

    // Resting against a corner, a hair further out than the radius. It has to be
    // a corner: a body resting on a flat face sits just *outside* the expanded
    // box and was never at risk. The slack is all in the rounded parts, which is
    // also why a body the size of this one meets a small object there and
    // almost nowhere else.
    Vec3 out_dir = linalg::vec3_normalize(Vec3{-1.0f, -1.0f, -1.0f});
    Vec3 resting = out_dir * (radius + 1e-3f);

    CHECK(intersect::aabb_contains(intersect::aabb_expand(box, radius), resting));
    CHECK(dist_to_box(resting, box) > radius);

    // backing away from it is free
    CHECK(!intersect::sweep_sphere_aabb(resting, radius, out_dir, box, NULL));

    // pushing into it is a contact with the corner's normal, not a zero one
    intersect::Hit h{};
    CHECK(intersect::sweep_sphere_aabb(resting, radius, -out_dir, box, &h));
    CHECK(near_vec3(h.normal, out_dir));
    CHECK(near_f32(dist_to_box(resting + (-out_dir) * h.t, box), radius));
}

static void sweep_inside_the_expanded_box_near_a_corner(memory::Arena *) {
    // Diagonally off a corner: inside the expanded box, but further than a
    // radius from the box itself, so nothing has been touched yet.
    intersect::Aabb box = unit_box();
    Vec3 from{-0.35f, -0.35f, -0.35f};

    CHECK(intersect::aabb_contains(intersect::aabb_expand(box, 0.5f), from));
    CHECK(dist_to_box(from, box) > 0.5f);

    // away from it hits nothing
    CHECK(!intersect::sweep_sphere_aabb(from, 0.5f, Vec3{-1.0f, -1.0f, -1.0f}, box, NULL));

    // into it finds the corner, at a real distance and with a real normal
    Vec3 motion{1.0f, 1.0f, 1.0f};
    intersect::Hit h{};
    CHECK(intersect::sweep_sphere_aabb(from, 0.5f, motion, box, &h));
    CHECK(h.t > 0.0f && h.t <= 1.0f);
    CHECK(near_f32(linalg::vec3_length(h.normal), 1.0f));
    CHECK(near_f32(dist_to_box(from + motion * h.t, box), 0.5f));
}

static void depenetrate_leaves_by_the_nearest_surface(memory::Arena *) {
    intersect::Aabb box = unit_box();

    linalg::Vec3 n{};
    f32 depth = 0.0f;

    // overlapping the -X face by 0.2
    Vec3 from{-0.3f, 0.5f, 0.5f};
    CHECK(intersect::depenetrate_sphere_aabb(from, 0.5f, box, &n, &depth));
    CHECK(near_vec3(n, Vec3{-1.0f, 0.0f, 0.0f}));
    CHECK(near_f32(depth, 0.2f));

    // and the push leaves it exactly touching, which is the whole contract
    CHECK(near_f32(dist_to_box(from + n * depth, box), 0.5f));

    // a radius clear of it is not an overlap
    CHECK(!intersect::depenetrate_sphere_aabb(Vec3{-1.0f, 0.5f, 0.5f}, 0.5f, box, NULL, NULL));
    CHECK(!intersect::depenetrate_sphere_aabb(Vec3{-0.5f, 0.5f, 0.5f}, 0.5f, box, NULL, NULL));
}

static void depenetrate_from_a_corner_leaves_diagonally(memory::Arena *) {
    // over a corner the way out is the diagonal, not whichever axis is checked first
    Vec3 from{-0.2f, -0.2f, -0.2f};

    linalg::Vec3 n{};
    f32 depth = 0.0f;
    CHECK(intersect::depenetrate_sphere_aabb(from, 0.5f, unit_box(), &n, &depth));

    f32 k = -1.0f / sqrtf(3.0f);
    CHECK(near_vec3(n, Vec3{k, k, k}));
    CHECK(near_f32(dist_to_box(from + n * depth, unit_box()), 0.5f));
}

static void depenetrate_from_deep_inside_takes_the_short_way(memory::Arena *) {
    // a wide flat box, with the centre nearer the top than any other face
    intersect::Aabb slab{Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 2.0f, 4.0f}};
    Vec3 from{2.0f, 1.5f, 2.0f};

    linalg::Vec3 n{};
    f32 depth = 0.0f;
    CHECK(intersect::depenetrate_sphere_aabb(from, 0.25f, slab, &n, &depth));
    CHECK(near_vec3(n, Vec3{0.0f, 1.0f, 0.0f}));

    // out of the box, and then clear of it by the radius
    CHECK(near_f32(depth, 0.75f));
    CHECK(near_f32(dist_to_box(from + n * depth, slab), 0.25f));

    // dead centre still produces a real direction rather than a zero vector
    CHECK(
        intersect::depenetrate_sphere_aabb(intersect::aabb_center(slab), 0.25f, slab, &n, &depth));
    CHECK(near_f32(linalg::vec3_length(n), 1.0f));
    CHECK(near_f32(dist_to_box(intersect::aabb_center(slab) + n * depth, slab), 0.25f));
}

static void aabb_transform_bounds_the_rotated_box(memory::Arena *) {
    // a unit cube centred on the origin, turned 45 degrees about Y: its
    // footprint grows to the diagonal on X and Z and is unchanged on Y
    intersect::Aabb b{Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}};
    linalg::Mat4 m = linalg::mat4_from_quat(linalg::quat_from_euler(Vec3{0.0f, 45.0f, 0.0f}));

    intersect::Aabb r = intersect::aabb_transform(b, m);
    f32 half = sqrtf(2.0f) * 0.5f;

    CHECK(near_vec3(r.min, Vec3{-half, -0.5f, -half}));
    CHECK(near_vec3(r.max, Vec3{half, 0.5f, half}));
}

static void aabb_helpers_agree(memory::Arena *) {
    intersect::Aabb e = intersect::aabb_empty();
    CHECK(!intersect::aabb_valid(e));

    e = intersect::aabb_add_point(e, Vec3{1.0f, 2.0f, 3.0f});
    CHECK(intersect::aabb_valid(e));
    CHECK(near_vec3(e.min, Vec3{1.0f, 2.0f, 3.0f}));
    CHECK(near_vec3(e.max, Vec3{1.0f, 2.0f, 3.0f}));

    e = intersect::aabb_add_point(e, Vec3{-1.0f, 0.0f, 5.0f});
    CHECK(near_vec3(e.min, Vec3{-1.0f, 0.0f, 3.0f}));
    CHECK(near_vec3(e.max, Vec3{1.0f, 2.0f, 5.0f}));
    CHECK(near_vec3(intersect::aabb_center(e), Vec3{0.0f, 1.0f, 4.0f}));
    CHECK(near_vec3(intersect::aabb_extents(e), Vec3{1.0f, 1.0f, 1.0f}));

    CHECK(intersect::aabb_contains(e, Vec3{0.0f, 1.0f, 4.0f}));
    CHECK(!intersect::aabb_contains(e, Vec3{0.0f, 1.0f, 2.0f}));

    CHECK(intersect::aabb_overlaps(
        unit_box(), intersect::Aabb{Vec3{0.5f, 0.5f, 0.5f}, Vec3{2.0f, 2.0f, 2.0f}}));
    CHECK(!intersect::aabb_overlaps(
        unit_box(), intersect::Aabb{Vec3{1.5f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 1.0f}}));

    // touching counts, so a sweep that lands exactly on a face still sees it
    CHECK(intersect::aabb_overlaps(
        unit_box(), intersect::Aabb{Vec3{1.0f, 0.0f, 0.0f}, Vec3{2.0f, 1.0f, 1.0f}}));
}

static const test::Case CASES[] = {
    CASE(ray_aabb_enters_the_near_face),
    CASE(ray_aabb_misses_and_respects_max_t),
    CASE(ray_aabb_from_inside_reports_no_surface),
    CASE(ray_aabb_parallel_to_a_slab),
    CASE(ray_sphere_hits_the_near_side),
    CASE(ray_triangle_hits_inside_the_winding),
    CASE(ray_triangle_culls_the_back),
    CASE(sweep_stops_a_radius_short_of_a_face),
    CASE(sweep_rounds_the_corners),
    CASE(sweep_past_a_corner_is_not_a_hit),
    CASE(sweep_does_not_tunnel),
    CASE(sweep_already_touching_reports_no_surface),
    CASE(sweep_touching_is_not_overlapping),
    CASE(sweep_inside_the_expanded_box_near_a_corner),
    CASE(depenetrate_leaves_by_the_nearest_surface),
    CASE(depenetrate_from_a_corner_leaves_diagonally),
    CASE(depenetrate_from_deep_inside_takes_the_short_way),
    CASE(aabb_transform_bounds_the_rotated_box),
    CASE(aabb_helpers_agree),
};

test::Suite intersect_suite() { return test::Suite{"intersect", CASES, ARRAY_COUNT(CASES)}; }
