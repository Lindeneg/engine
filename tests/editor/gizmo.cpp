#include "editor/gizmo.h"

#include <math.h>

#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static gizmo::Frame world_frame() {
    gizmo::Frame f{};
    f.origin = linalg::Vec3{0.0f, 0.0f, 0.0f};
    f.axis[0] = linalg::Vec3{1.0f, 0.0f, 0.0f};
    f.axis[1] = linalg::Vec3{0.0f, 1.0f, 0.0f};
    f.axis[2] = linalg::Vec3{0.0f, 0.0f, 1.0f};
    f.length = 1.0f;
    f.plane_offset = 0.2f;
    f.plane_size = 0.3f;
    return f;
}

// looking down -Z from z = 5 through the point (x, y)
static intersect::Ray toward(f32 x, f32 y) {
    return intersect::Ray{linalg::Vec3{x, y, 5.0f}, linalg::Vec3{0.0f, 0.0f, -1.0f}};
}

static void hit_finds_the_nearest_handle(memory::Arena *) {
    gizmo::Frame f = world_frame();

    // a hair above the X axis, halfway along it
    CHECK(gizmo::hit(&f, toward(0.5f, 0.05f), 0.1f) == gizmo::HANDLE_X);
    CHECK(gizmo::hit(&f, toward(0.5f, 0.05f), 0.01f) == gizmo::HANDLE_NONE);

    // a hair beside the Y axis
    CHECK(gizmo::hit(&f, toward(0.05f, 0.5f), 0.1f) == gizmo::HANDLE_Y);

    // past the end of the axis line is a miss
    CHECK(gizmo::hit(&f, toward(1.5f, 0.05f), 0.1f) == gizmo::HANDLE_NONE);

    // inside the XY square, which wins over the axes near it
    CHECK(gizmo::hit(&f, toward(0.35f, 0.35f), 0.1f) == gizmo::HANDLE_XY);
    CHECK(gizmo::hit(&f, toward(0.6f, 0.6f), 0.1f) == gizmo::HANDLE_NONE); // beyond the square

    // the Z axis points at the eye, so it cannot be grabbed from here; the
    // origin still belongs to some handle rather than to nothing
    gizmo::Handle at_origin = gizmo::hit(&f, toward(0.0f, 0.0f), 0.1f);
    CHECK(at_origin != gizmo::HANDLE_NONE && at_origin != gizmo::HANDLE_Z);
}

static void axis_drag_measures_along_its_axis(memory::Arena *) {
    gizmo::Frame f = world_frame();

    gizmo::Drag d = gizmo::begin(&f, gizmo::HANDLE_X, toward(0.0f, 0.0f));
    CHECK(d.valid);

    linalg::Vec3 moved = gizmo::delta(&d, toward(2.0f, 3.0f), 0.0f);
    CHECK(NEAR(moved.x, 2.0f) && NEAR(moved.y, 0.0f) && NEAR(moved.z, 0.0f));

    // a grab away from the origin measures from the grab, not the origin
    gizmo::Drag off = gizmo::begin(&f, gizmo::HANDLE_X, toward(0.5f, 0.0f));
    moved = gizmo::delta(&off, toward(2.0f, 0.0f), 0.0f);
    CHECK(NEAR(moved.x, 1.5f));

    // snapping rounds to the step
    moved = gizmo::delta(&d, toward(2.3f, 0.0f), 0.5f);
    CHECK(NEAR(moved.x, 2.5f));
    moved = gizmo::delta(&d, toward(2.2f, 0.0f), 0.5f);
    CHECK(NEAR(moved.x, 2.0f));
}

static void plane_drag_moves_in_two_axes(memory::Arena *) {
    gizmo::Frame f = world_frame();

    gizmo::Drag d = gizmo::begin(&f, gizmo::HANDLE_XY, toward(0.3f, 0.3f));
    CHECK(d.valid);

    linalg::Vec3 moved = gizmo::delta(&d, toward(1.3f, 2.3f), 0.0f);
    CHECK(NEAR(moved.x, 1.0f) && NEAR(moved.y, 2.0f) && NEAR(moved.z, 0.0f));
}

static void a_parallel_ray_cannot_drag(memory::Arena *) {
    gizmo::Frame f = world_frame();

    // straight down the X axis: no position along it can be read from the ray
    intersect::Ray along{linalg::Vec3{-5.0f, 0.0f, 0.0f}, linalg::Vec3{1.0f, 0.0f, 0.0f}};
    gizmo::Drag d = gizmo::begin(&f, gizmo::HANDLE_X, along);
    CHECK(!d.valid);
    linalg::Vec3 moved = gizmo::delta(&d, toward(2.0f, 0.0f), 0.0f);
    CHECK(moved.x == 0.0f && moved.y == 0.0f && moved.z == 0.0f);

    // and a ray in the plane of a plane handle
    gizmo::Drag p = gizmo::begin(&f, gizmo::HANDLE_XY, along);
    CHECK(!p.valid);
}

test::Suite gizmo_suite() {
    static const test::Case cases[] = {
        CASE(hit_finds_the_nearest_handle),
        CASE(axis_drag_measures_along_its_axis),
        CASE(plane_drag_moves_in_two_axes),
        CASE(a_parallel_ray_cannot_drag),
    };
    return test::Suite{.name = "gizmo", .cases = cases, .count = ARRAY_COUNT(cases)};
}
