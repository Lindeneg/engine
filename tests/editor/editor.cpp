#include "editor/editor.h"

#include <math.h>
#include <string.h>

#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define FB_W 800
#define FB_H 600
#define FOV 60.0f

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
                                    .max_renderables = 8,
                                    .max_animators = 2,
                                    .max_cameras = 2,
                                    .max_lights = 2,
                                }));
}

// an entity at p, drawing the fallback cube when visible
static entity::Handle place(entity::World *w, const char *name, linalg::Vec3 p, b32 visible) {
    entity::Handle h = entity::spawn(w, name);
    entity::get(w, h)->local.position = p;
    if (visible) {
        entity::Renderable *r = entity::add_renderable(w, h);
        r->model = asset::MODEL_FALLBACK;
        r->visible = true;
    }
    return h;
}

static void pick_ray_leaves_through_the_pixel(memory::Arena *) {
    editor::Viewpoint v{linalg::Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
    f32 aspect = (f32)FB_W / (f32)FB_H;

    // the centre pixel looks straight ahead, which is -Z
    intersect::Ray r = editor::pick_ray(v, FOV, aspect, linalg::Vec2{FB_W * 0.5f, FB_H * 0.5f},
                                        FB_W, FB_H);
    CHECK(NEAR(r.dir.x, 0.0f) && NEAR(r.dir.y, 0.0f) && NEAR(r.dir.z, -1.0f));

    // the top edge tilts up by half the vertical field of view
    r = editor::pick_ray(v, FOV, aspect, linalg::Vec2{FB_W * 0.5f, 0.0f}, FB_W, FB_H);
    CHECK(NEAR(r.dir.y / -r.dir.z, tanf(DEG2RAD(FOV) * 0.5f)));

    // the right edge is wider than the top by the aspect
    r = editor::pick_ray(v, FOV, aspect, linalg::Vec2{FB_W, FB_H * 0.5f}, FB_W, FB_H);
    CHECK(NEAR(r.dir.x / -r.dir.z, tanf(DEG2RAD(FOV) * 0.5f) * aspect));

    // yawed a quarter turn, straight ahead is -X
    v.yaw = 90.0f;
    r = editor::pick_ray(v, FOV, aspect, linalg::Vec2{FB_W * 0.5f, FB_H * 0.5f}, FB_W, FB_H);
    CHECK(NEAR(r.dir.x, -1.0f) && NEAR(r.dir.z, 0.0f));
}

static void pick_takes_the_nearest(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    entity::Handle near_cube = place(w, "near", linalg::Vec3{0.0f, 0.0f, -5.0f}, true);
    place(w, "far", linalg::Vec3{0.0f, 0.0f, -10.0f}, true);
    entity::Handle marker = place(w, "camera", linalg::Vec3{3.0f, 0.0f, -5.0f}, false);
    entity::Handle hidden = place(w, "hidden", linalg::Vec3{0.45f, 0.0f, -2.0f}, true);
    entity::renderable(w, hidden)->visible = false;

    entity::end_frame(w, m, a, 0.0f); // world transforms

    linalg::Vec3 eye{0.0f, 0.0f, 0.0f};
    linalg::Vec3 ahead{0.0f, 0.0f, -1.0f};

    // both cubes sit on the ray; the near one wins. The hidden one in front
    // of it is a marker rather than a box, so its cube (which the ray would
    // cross) does not count and its marker sphere is off to the side
    CHECK(editor::pick(w, m, intersect::Ray{eye, ahead}, 0.3f, 100.0f).v == near_cube.v);

    // a marker is picked by its sphere
    linalg::Vec3 to_marker = linalg::vec3_normalize(linalg::Vec3{3.0f, 0.0f, -5.0f});
    CHECK(editor::pick(w, m, intersect::Ray{eye, to_marker}, 0.3f, 100.0f).v == marker.v);

    // nothing along +X, and nothing within a short reach
    CHECK(editor::pick(w, m, intersect::Ray{eye, linalg::Vec3{1.0f, 0.0f, 0.0f}}, 0.3f, 100.0f).v ==
          0);
    CHECK(editor::pick(w, m, intersect::Ray{eye, ahead}, 0.3f, 1.0f).v == 0);
}

static void pick_respects_a_rotated_box(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // A cube turned 45 degrees about Y is a diamond seen from above, |x| + |dz|
    // <= corner, inside a world-aligned bound of |x|, |dz| <= corner. A ray
    // running along one of the diamond's edges, offset outward, crosses that
    // bound's corner region and never touches the cube; the same ray offset
    // inward crosses the cube. Only an exact test in the cube's frame tells them apart.
    entity::Handle h = place(w, "turned", linalg::Vec3{0.0f, 0.0f, -5.0f}, true);
    entity::set_euler(&entity::get(w, h)->local, linalg::Vec3{0.0f, 45.0f, 0.0f});
    entity::end_frame(w, m, a, 0.0f);

    linalg::Vec3 lo, hi;
    asset::model_bounds(m, asset::MODEL_FALLBACK, &lo, &hi);
    f32 half = (hi.x - lo.x) * 0.5f;
    f32 corner = half * sqrtf(2.0f);

    // the line x + (z + 5) = sum, travelled toward -x and +z
    linalg::Vec3 edge_dir = linalg::vec3_normalize(linalg::Vec3{-1.0f, 0.0f, 1.0f});
    linalg::Vec3 outside{2.0f, 0.0f, -5.0f + (corner * 1.3f - 2.0f)};
    linalg::Vec3 inside{2.0f, 0.0f, -5.0f + (corner * 0.6f - 2.0f)};

    CHECK(editor::pick(w, m, intersect::Ray{inside, edge_dir}, 0.3f, 100.0f).v == h.v);
    CHECK(editor::pick(w, m, intersect::Ray{outside, edge_dir}, 0.3f, 100.0f).v == 0);

    // and the world-aligned bound would have said otherwise
    intersect::Aabb bound = intersect::aabb_transform(intersect::Aabb{lo, hi},
                                                      entity::get(w, h)->world);
    intersect::Hit unused;
    CHECK(intersect::ray_aabb(intersect::Ray{outside, edge_dir}, bound, 100.0f, &unused));
}

static void world_position_writes_into_the_parents_frame(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);

    // a parent at x = 10, scaled by two: a child put at world x = 14 is local x = 2
    entity::Handle parent = place(w, "parent", linalg::Vec3{10.0f, 0.0f, 0.0f}, false);
    entity::get(w, parent)->local.scale = linalg::Vec3{2.0f, 2.0f, 2.0f};
    entity::Handle child = place(w, "child", linalg::Vec3{0.0f, 0.0f, 0.0f}, false);
    entity::set_parent(w, m, child, parent, NULL);
    entity::end_frame(w, m, a, 0.0f);

    entity::set_world_position(w, child, linalg::Vec3{14.0f, 0.0f, 0.0f});
    CHECK(NEAR(entity::get(w, child)->local.position.x, 2.0f));

    entity::end_frame(w, m, a, 0.0f);
    CHECK(NEAR(linalg::mat4_translation(entity::get(w, child)->world).x, 14.0f));

    // a root takes the world position as its own
    entity::set_world_position(w, parent, linalg::Vec3{1.0f, 2.0f, 3.0f});
    CHECK(entity::get(w, parent)->local.position.z == 3.0f);
}

static void config_reads_the_file_shape(memory::Arena *a) {
    config::Doc *doc = MUST(config::parse(
        a, memory::push_str(a, "play_map game\ncamera {\n    speed 9\n    near 1\n    far 0.5\n}\n"),
        "editor.cfg"));

    test::mute();
    editor::Config c = editor::config_from(a, doc->first);
    CHECK(test::unmute() == 1); // the inverted range

    CHECK(strcmp(c.play_map, "game") == 0);
    CHECK(c.camera.speed == 9.0f);
    CHECK(c.camera.z_far > c.camera.z_near && c.camera.z_near > 0.0f);
    CHECK(c.marker_size > 0.0f && c.hierarchy_width > 0.0f);

    CHECK(editor::load_config(a, a, CL_ASSET_ROOT).err == result::OK);
}

test::Suite editor_suite() {
    static const test::Case cases[] = {
        CASE(pick_ray_leaves_through_the_pixel),
        CASE(pick_takes_the_nearest),
        CASE(pick_respects_a_rotated_box),
        CASE(world_position_writes_into_the_parents_frame),
        CASE(config_reads_the_file_shape),
    };
    return test::Suite{.name = "editor", .cases = cases, .count = ARRAY_COUNT(cases)};
}
