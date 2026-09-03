#include "scene/motion.h"

#include <math.h>
#include <string.h>

#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
#define RADIUS 0.5f

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
                                    .max_renderables = 4,
                                    .max_animators = 2,
                                    .max_cameras = 2,
                                    .max_lights = 2,
                                    .max_colliders = 8,
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
    motion::Result r = motion::move_and_slide(w, m, start, RADIUS, toward, entity::NONE);
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

    motion::Result r = motion::move_and_slide(w, m, linalg::Vec3{0.0f, 0.0f, 0.0f}, RADIUS,
                                              linalg::Vec3{0.0f, 0.0f, -10.0f}, entity::NONE);
    CHECK(r.hit);
    CHECK(NEAR(r.normal.z, 1.0f));
    CHECK(NEAR(r.position.z, -5.0f + 1.0f + RADIUS + MOTION_SKIN));

    // and pushes a body that starts inside it out, along the line of centres
    r = motion::move_and_slide(w, m, linalg::Vec3{0.0f, 0.0f, -4.4f}, RADIUS,
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

    motion::Result r = motion::move_and_slide(w, m, linalg::Vec3{1.0f, 2.0f, 1.0f}, RADIUS,
                                              linalg::Vec3{0.0f, -5.0f, 0.0f}, entity::NONE);
    CHECK(r.grounded);
    CHECK(NEAR(r.position.y, RADIUS + MOTION_SKIN));

    // scale on the owner does not grow the shape: a body past x = 5 falls through
    entity::get(w, h)->local.scale = linalg::Vec3{3.0f, 3.0f, 3.0f};
    entity::end_frame(w, m, a, 0.0f);
    r = motion::move_and_slide(w, m, linalg::Vec3{8.0f, 2.0f, 1.0f}, RADIUS,
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
    motion::Result r = motion::move_and_slide(w, m, linalg::Vec3{0.0f, 0.0f, 0.0f}, RADIUS,
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

test::Suite motion_suite() {
    static const test::Case cases[] = {
        CASE(a_rotated_box_stops_at_its_real_face),
        CASE(a_sphere_collider_stops_a_radius_short),
        CASE(a_floor_grounds_and_the_offset_places_the_shape),
        CASE(the_ignored_subtree_is_not_solid),
        CASE(colliders_round_trip_through_a_file),
    };
    return test::Suite{.name = "motion", .cases = cases, .count = ARRAY_COUNT(cases)};
}
