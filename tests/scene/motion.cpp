#include "scene/motion.h"

#include <math.h>
#include <string.h>

#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
#define RADIUS 0.5f

// the sphere every older case moves: a body of no height and no step
static motion::Body ball(linalg::Vec3 at) { return motion::Body{at, RADIUS, 2.0f * RADIUS, 0.0f}; }

static asset::Manager *make_assets(memory::Arena *a) {
    return MUST(asset::init(a, a,
                            asset::Config{
                                .root = ".",
                                .default_shader = shader::Handle{0},
                                .max_models = 8,
                                .max_meshes = 8,
                                .max_materials = 8,
                                .max_skeletons = 4,
                                .max_clips = 4,
                                .scope_bytes = {KiB(256), KiB(256)},
                            }));
}

static entity::World *make_world(memory::Arena *a) {
    return MUST(entity::init(a, entity::Config{
                                    .max_entities = 16,
                                    .pool_default = 8,
                                }));
}

static entity::Handle box(entity::World *w, const char *name, linalg::Vec3 at, linalg::Vec3 size,
                          f32 yaw) {
    entity::Handle h = entity::spawn(w, name);
    entity::get(w, h)->local.position = at;
    entity::set_euler(&entity::get(w, h)->local, linalg::Vec3{0.0f, yaw, 0.0f});
    entity::Collider *c = entity::add_collider(w, h);
    c->shape = entity::COLLIDER_BOX;
    c->size = size;
    return h;
}

static void a_rotated_box_stops_at_its_real_face(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // a thin wall turned 45 degrees about Y, centred at the origin: its face
    // toward +X+Z is the line x + z = 0.25 * sqrt(2), where a world-aligned
    // bound of it would stop a body a long way earlier
    box(w, "wall", linalg::Vec3{0.0f, 0.0f, 0.0f}, linalg::Vec3{4.0f, 4.0f, 0.5f}, 45.0f);
    entity::end_frame(w, m, a, 0.0f);

    linalg::Vec3 start{3.0f, 0.0f, 3.0f};
    linalg::Vec3 toward{-2.5f, 0.0f, -2.5f};
    motion::Result r = motion::move_and_slide(w, m, ball(start), toward, entity::NONE);
    CHECK(r.hit);

    // the face's outward normal is (1, 0, 1) / sqrt(2), and the body rests a
    // radius plus the skin off it
    f32 s = 1.0f / sqrtf(2.0f);
    CHECK(NEAR(r.normal.x, s) && NEAR(r.normal.z, s) && NEAR(r.normal.y, 0.0f));
    f32 along = (r.position.x + r.position.z) * s; // distance of the centre from the wall's plane
    CHECK(NEAR(along, 0.25f + RADIUS + MOTION_SKIN));

    // a world-aligned bound would have reached out to x = 1.59; the real face is closer to the origin
    CHECK(r.position.x < 1.2f);
}

static void a_sphere_collider_stops_a_radius_short(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    entity::Handle h = entity::spawn(w, "ball");
    entity::get(w, h)->local.position = linalg::Vec3{0.0f, 0.0f, -5.0f};
    entity::Collider *c = entity::add_collider(w, h);
    c->shape = entity::COLLIDER_SPHERE;
    c->radius = 1.0f;
    entity::end_frame(w, m, a, 0.0f);

    motion::Result r = motion::move_and_slide(w, m, ball(linalg::Vec3{0.0f, 0.0f, 0.0f}),
                                              linalg::Vec3{0.0f, 0.0f, -10.0f}, entity::NONE);
    CHECK(r.hit);
    CHECK(NEAR(r.normal.z, 1.0f));
    CHECK(NEAR(r.position.z, -5.0f + 1.0f + RADIUS + MOTION_SKIN));

    // and pushes a body that starts inside it out, along the line of centres
    r = motion::move_and_slide(w, m, ball(linalg::Vec3{0.0f, 0.0f, -4.4f}),
                               linalg::Vec3{0.0f, 0.0f, 0.0f}, entity::NONE);
    CHECK(r.depenetrated && !r.stuck);
    CHECK(r.position.z > -4.4f + 0.8f);
}

static void a_floor_grounds_and_the_offset_places_the_shape(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // the entity sits at y = 0 and the box hangs below it by its offset, so the
    // top face is exactly at the entity's height
    entity::Handle h = box(w, "floor", linalg::Vec3{0.0f, 0.0f, 0.0f},
                           linalg::Vec3{10.0f, 1.0f, 10.0f}, 0.0f);
    entity::collider(w, h)->offset = linalg::Vec3{0.0f, -0.5f, 0.0f};
    entity::end_frame(w, m, a, 0.0f);

    motion::Result r = motion::move_and_slide(w, m, ball(linalg::Vec3{1.0f, 2.0f, 1.0f}),
                                              linalg::Vec3{0.0f, -5.0f, 0.0f}, entity::NONE);
    CHECK(r.grounded);
    CHECK(NEAR(r.position.y, RADIUS + MOTION_SKIN));

    // scale on the owner does not grow the shape: a body past x = 5 falls through
    entity::get(w, h)->local.scale = linalg::Vec3{3.0f, 3.0f, 3.0f};
    entity::end_frame(w, m, a, 0.0f);
    r = motion::move_and_slide(w, m, ball(linalg::Vec3{8.0f, 2.0f, 1.0f}),
                               linalg::Vec3{0.0f, -5.0f, 0.0f}, entity::NONE);
    CHECK(!r.grounded);
}

static void the_ignored_subtree_is_not_solid(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    entity::Handle body = box(w, "body", linalg::Vec3{0.0f, 0.0f, 0.0f},
                              linalg::Vec3{1.0f, 2.0f, 1.0f}, 0.0f);
    entity::Handle gun = box(w, "gun", linalg::Vec3{0.0f, 0.0f, -1.0f},
                             linalg::Vec3{0.2f, 0.2f, 0.5f}, 0.0f);
    entity::set_parent(w, m, gun, body, NULL);
    box(w, "wall", linalg::Vec3{0.0f, 0.0f, -5.0f}, linalg::Vec3{4.0f, 4.0f, 0.5f}, 0.0f);
    entity::end_frame(w, m, a, 0.0f);

    // from inside its own box and through its own gun, stopped only by the wall
    motion::Result r = motion::move_and_slide(w, m, ball(linalg::Vec3{0.0f, 0.0f, 0.0f}),
                                              linalg::Vec3{0.0f, 0.0f, -10.0f}, body);
    CHECK(!r.depenetrated);
    CHECK(r.hit);
    CHECK(NEAR(r.position.z, -5.0f + 0.25f + RADIUS + MOTION_SKIN));
}

static void colliders_round_trip_through_a_file(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    entity::Handle h = box(w, "wall", linalg::Vec3{1.0f, 2.0f, 3.0f}, linalg::Vec3{6.0f, 3.0f, 0.5f},
                           35.0f);
    entity::collider(w, h)->offset = linalg::Vec3{0.0f, 1.5f, 0.0f};

    config::Doc *doc = config::create(a, "test.level");
    entity::write_entity(a, doc, NULL, w, h, "instance", false);
    char *text = config::write(a, doc);
    CHECK(strstr(text, "collider {") != NULL);
    CHECK(strstr(text, "shape box") != NULL);
    CHECK(strstr(text, "size 6 3 0.5") != NULL);
    CHECK(strstr(text, "offset 0 1.5 0") != NULL);

    // read back into a fresh world: the same shape, and the same world frame
    entity::World *w2 = make_world(a);
    config::Doc *back = MUST(config::parse(a, text, "test.level"));
    entity::Handle h2 = entity::spawn_from_config(w2, m, back->first, entity::NONE);
    entity::Collider *c = entity::collider(w2, h2);
    CHECK(c != NULL);
    CHECK(c->shape == entity::COLLIDER_BOX);
    CHECK(c->size.x == 6.0f && c->offset.y == 1.5f);

    entity::end_frame(w, m, a, 0.0f);
    entity::end_frame(w2, m, a, 0.0f);
    linalg::Mat4 f1 = entity::collider_frame(w, entity::collider(w, h));
    linalg::Mat4 f2 = entity::collider_frame(w2, c);
    for (u32 i = 0; i < 16; i++)
        CHECK(NEAR(f1.m[i], f2.m[i]));

    // a size of nothing is refused on read, loudly
    config::Doc *bad = MUST(config::parse(
        a, memory::push_str(a, "instance x {\n    collider {\n        size 0 1 1\n    }\n}\n"),
        "bad.level"));
    test::mute();
    entity::Handle hb = entity::spawn_from_config(w2, m, bad->first, entity::NONE);
    CHECK(test::unmute() == 1);
    CHECK(entity::collider(w2, hb)->size.x == 1.0f);
}

// a person, not a ball: an overhang at chest height stops a body tall enough to
// reach it and lets a sphere of the same radius walk under
static void a_capsule_meets_what_its_head_would(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // a floor at y = 0 and a beam whose underside is at y = 1.2, across the path
    box(w, "floor", linalg::Vec3{0.0f, -0.5f, 0.0f}, linalg::Vec3{20.0f, 1.0f, 20.0f}, 0.0f);
    box(w, "beam", linalg::Vec3{0.0f, 1.45f, -3.0f}, linalg::Vec3{6.0f, 0.5f, 0.5f}, 0.0f);
    entity::end_frame(w, m, a, 0.0f);

    linalg::Vec3 start{0.0f, RADIUS + MOTION_SKIN, 0.0f};
    linalg::Vec3 forward{0.0f, 0.0f, -6.0f};

    motion::Body tall{start, RADIUS, 1.8f, 0.0f};
    motion::Result r = motion::move_and_slide(w, m, tall, forward, entity::NONE);
    CHECK(r.hit);
    CHECK(NEAR(r.normal.z, 1.0f));

    // the top sphere's centre is at y = 1.3, so its front meets the beam's near
    // face at z = -2.75, and the body stops a radius and a skin short of it
    CHECK(NEAR(r.position.z, -2.75f + RADIUS + MOTION_SKIN));

    motion::Body short_one{start, RADIUS, 2.0f * RADIUS, 0.0f};
    r = motion::move_and_slide(w, m, short_one, forward, entity::NONE);
    CHECK(!r.hit);
    CHECK(NEAR(r.position.z, -6.0f));

    // a tall body dropped onto the floor stands on its bottom sphere as before
    motion::Body dropped{linalg::Vec3{2.0f, 3.0f, 2.0f}, RADIUS, 1.8f, 0.0f};
    r = motion::move_and_slide(w, m, dropped, linalg::Vec3{0.0f, -5.0f, 0.0f}, entity::NONE);
    CHECK(r.grounded);
    CHECK(NEAR(r.position.y, RADIUS + MOTION_SKIN));
}

// A step it may climb is climbed, one too tall is a wall, and one with no
// headroom to lift into is a wall too. The body is narrow, so its bottom
// sphere's centre sits below the risers and meets their faces rather than
// rolling on their edges, which keeps the numbers exact.
#define STEP_RADIUS 0.2f

static void a_step_is_walked_up_within_its_height(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    box(w, "floor", linalg::Vec3{0.0f, -0.5f, 0.0f}, linalg::Vec3{20.0f, 1.0f, 20.0f}, 0.0f);
    // a step 0.3 high with its riser at z = -2, a block 0.8 high with its face at z = -6
    box(w, "step", linalg::Vec3{0.0f, 0.15f, -3.0f}, linalg::Vec3{4.0f, 0.3f, 2.0f}, 0.0f);
    box(w, "block", linalg::Vec3{0.0f, 0.4f, -7.0f}, linalg::Vec3{4.0f, 0.8f, 2.0f}, 0.0f);
    entity::end_frame(w, m, a, 0.0f);

    f32 standing = STEP_RADIUS + MOTION_SKIN; // the centre's height over what it stands on
    linalg::Vec3 start{0.0f, standing, 0.0f};

    // no step allowed: stopped at the riser
    motion::Body ballish{start, STEP_RADIUS, 1.8f, 0.0f};
    motion::Result r = motion::move_and_slide(w, m, ballish, linalg::Vec3{0.0f, 0.0f, -3.0f},
                                              entity::NONE);
    CHECK(r.hit && NEAR(r.normal.z, 1.0f));
    CHECK(NEAR(r.position.z, -2.0f + standing));

    // with a 0.4 step: up onto it and on to the full move, standing on its top at y = 0.3
    motion::Body stepper{start, STEP_RADIUS, 1.8f, 0.4f};
    r = motion::move_and_slide(w, m, stepper, linalg::Vec3{0.0f, 0.0f, -3.0f}, entity::NONE);
    CHECK(r.grounded);
    CHECK(NEAR(r.position.z, -3.0f));
    CHECK(NEAR(r.position.y, 0.3f + standing));

    // the block is 0.8 high: too tall for the step, so its face stops the body
    motion::Body on_step{linalg::Vec3{0.0f, 0.3f + standing, -3.0f}, STEP_RADIUS, 1.8f, 0.4f};
    r = motion::move_and_slide(w, m, on_step, linalg::Vec3{0.0f, 0.0f, -4.0f}, entity::NONE);
    CHECK(r.hit && NEAR(r.normal.z, 1.0f));
    CHECK(NEAR(r.position.z, -6.0f + standing));

    // a ceiling 0.1 above the crown: less lift than the step needs, so it is a wall again
    box(w, "ceiling", linalg::Vec3{0.0f, 2.0f, -1.0f}, linalg::Vec3{4.0f, 0.2f, 2.0f}, 0.0f);
    entity::end_frame(w, m, a, 0.0f);
    r = motion::move_and_slide(w, m, stepper, linalg::Vec3{0.0f, 0.0f, -3.0f}, entity::NONE);
    CHECK(r.hit && NEAR(r.normal.z, 1.0f));
    CHECK(NEAR(r.position.z, -2.0f + standing));
}

// a bullet: the nearest face, exactly, whatever the box's turn, and never its own subtree
static void a_ray_hits_the_nearest_collider_exactly(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // two walls down -Z, the nearer one turned 45 degrees so its face is a diagonal
    entity::Handle near = box(w, "near", linalg::Vec3{0.0f, 0.0f, -5.0f},
                              linalg::Vec3{4.0f, 4.0f, 0.5f}, 45.0f);
    box(w, "far", linalg::Vec3{0.0f, 0.0f, -10.0f}, linalg::Vec3{4.0f, 4.0f, 0.5f}, 0.0f);
    entity::end_frame(w, m, a, 0.0f);

    // straight down -Z from the origin: the diagonal face x + z = -5 + 0.25 *
    // sqrt(2) in the world is met at x = 0, and the length need not be unit
    linalg::Vec3 origin{0.0f, 0.0f, 0.0f};
    motion::RayResult r =
        motion::ray_cast(w, origin, linalg::Vec3{0.0f, 0.0f, -3.0f}, 100.0f, entity::NONE);
    CHECK(r.hit);
    CHECK(r.entity.v == near.v);
    f32 face = 5.0f - 0.25f * sqrtf(2.0f);
    CHECK(NEAR(r.distance, face));
    CHECK(NEAR(r.point.z, -face) && NEAR(r.point.x, 0.0f));

    // the normal faces the ray: (1, 0, 1) / sqrt(2) on that face
    f32 s = 1.0f / sqrtf(2.0f);
    CHECK(NEAR(r.normal.x, s) && NEAR(r.normal.z, s) && NEAR(r.normal.y, 0.0f));

    // ignoring the near wall reaches the far one, at its face 9.75 away
    r = motion::ray_cast(w, origin, linalg::Vec3{0.0f, 0.0f, -1.0f}, 100.0f, near);
    CHECK(r.hit && r.entity.v != near.v);
    CHECK(NEAR(r.distance, 9.75f));
    CHECK(NEAR(r.normal.z, 1.0f));

    // out of range, the wrong way, and no direction at all
    CHECK(!motion::ray_cast(w, origin, linalg::Vec3{0.0f, 0.0f, -1.0f}, 4.0f, entity::NONE).hit);
    CHECK(!motion::ray_cast(w, origin, linalg::Vec3{0.0f, 0.0f, 1.0f}, 100.0f, entity::NONE).hit);
    test::mute();
    CHECK(!motion::ray_cast(w, origin, linalg::Vec3{0.0f, 0.0f, 0.0f}, 100.0f, entity::NONE).hit);
    CHECK(test::unmute() == 1);
}

static void a_ray_hits_a_sphere_and_reports_being_inside(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    entity::Handle ball = entity::spawn(w, "ball");
    entity::get(w, ball)->local.position = linalg::Vec3{3.0f, 1.0f, 0.0f};
    entity::Collider *c = entity::add_collider(w, ball);
    c->shape = entity::COLLIDER_SPHERE;
    c->radius = 1.0f;
    entity::end_frame(w, m, a, 0.0f);

    // from the left along +X, at the sphere's height: the surface is a radius short of the centre
    motion::RayResult r = motion::ray_cast(w, linalg::Vec3{0.0f, 1.0f, 0.0f},
                                           linalg::Vec3{1.0f, 0.0f, 0.0f}, 10.0f, entity::NONE);
    CHECK(r.hit && r.entity.v == ball.v);
    CHECK(NEAR(r.distance, 2.0f));
    CHECK(NEAR(r.point.x, 2.0f) && NEAR(r.normal.x, -1.0f));

    // from inside: a hit at no distance with nothing to face
    r = motion::ray_cast(w, linalg::Vec3{3.0f, 1.0f, 0.0f}, linalg::Vec3{1.0f, 0.0f, 0.0f}, 10.0f,
                         entity::NONE);
    CHECK(r.hit && r.distance == 0.0f);
    CHECK(linalg::vec3_length_sq(r.normal) == 0.0f);
}

test::Suite motion_suite() {
    static const test::Case cases[] = {
        CASE(a_capsule_meets_what_its_head_would),
        CASE(a_step_is_walked_up_within_its_height),
        CASE(a_ray_hits_the_nearest_collider_exactly),
        CASE(a_ray_hits_a_sphere_and_reports_being_inside),
        CASE(a_rotated_box_stops_at_its_real_face),
        CASE(a_sphere_collider_stops_a_radius_short),
        CASE(a_floor_grounds_and_the_offset_places_the_shape),
        CASE(the_ignored_subtree_is_not_solid),
        CASE(colliders_round_trip_through_a_file),
    };
    return test::Suite{.name = "motion", .cases = cases, .count = ARRAY_COUNT(cases)};
}
