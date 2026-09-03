#include "editor/undo.h"

#include <stddef.h>
#include <string.h>

#include "platform/file.h"
#include "platform/path.h"
#include "suites.h"

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

// changes the camera's fov in place and journals it
static void set_fov(undo::Journal *j, entity::World *w, entity::Handle h, f32 fov, b32 continuing) {
    entity::Camera *c = entity::camera(w, h);
    f32 before = c->fov_y;
    c->fov_y = fov;
    undo::record_field(j, h, entity::COMPONENT_CAMERA, (u32)offsetof(entity::Camera, fov_y),
                       sizeof(f32), &before, &c->fov_y, continuing);
}

static void field_edits_round_trip(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 8);

    entity::Handle h = entity::spawn(w, "cam");
    entity::add_camera(w, h);
    CHECK(!undo::can_undo(j) && !undo::can_redo(j));

    set_fov(j, w, h, 90.0f, false);
    CHECK(undo::can_undo(j) && !undo::can_redo(j));

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h)->fov_y == 60.0f); // the engine default
    CHECK(undo::can_redo(j));

    CHECK(undo::redo(j, w, m));
    CHECK(entity::camera(w, h)->fov_y == 90.0f);
    CHECK(!undo::redo(j, w, m));

    // the entity's own fields go by offset into it
    entity::Entity *e = entity::get(w, h);
    char before[ENTITY_NAME_CAP];
    memcpy(before, e->name, sizeof(before));
    snprintf(e->name, ENTITY_NAME_CAP, "renamed");
    undo::record_field(j, h, UNDO_ENTITY, (u32)offsetof(entity::Entity, name), ENTITY_NAME_CAP,
                       before, e->name, false);
    CHECK(undo::undo(j, w, m));
    CHECK(strcmp(entity::get(w, h)->name, "cam") == 0);
}

static void continuing_edits_merge_until_closed(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 8);

    entity::Handle h = entity::spawn(w, "cam");
    entity::add_camera(w, h);

    // a drag: three values, one entry, from the first before to the last after
    set_fov(j, w, h, 61.0f, true);
    set_fov(j, w, h, 62.0f, true);
    set_fov(j, w, h, 63.0f, true);
    CHECK(undo::count(j) == 1);

    undo::close(j);
    set_fov(j, w, h, 64.0f, true);
    CHECK(undo::count(j) == 2);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h)->fov_y == 63.0f);
    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h)->fov_y == 60.0f);

    // a different field on the same component is never merged
    entity::Camera *c = entity::camera(w, h);
    set_fov(j, w, h, 70.0f, true);
    f32 before = c->z_far;
    c->z_far = 100.0f;
    undo::record_field(j, h, entity::COMPONENT_CAMERA, (u32)offsetof(entity::Camera, z_far),
                       sizeof(f32), &before, &c->z_far, true);
    CHECK(undo::count(j) == 2);
}

static void new_edits_drop_the_redo_tail_and_depth_the_oldest(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 2);

    entity::Handle h = entity::spawn(w, "cam");
    entity::add_camera(w, h);

    set_fov(j, w, h, 61.0f, false);
    set_fov(j, w, h, 62.0f, false);
    set_fov(j, w, h, 63.0f, false); // the 61 entry falls off
    CHECK(undo::count(j) == 2);

    CHECK(undo::undo(j, w, m) && undo::undo(j, w, m));
    CHECK(entity::camera(w, h)->fov_y == 61.0f);
    CHECK(!undo::undo(j, w, m));

    // a fresh edit after undoing forgets what could have been redone
    set_fov(j, w, h, 70.0f, false);
    CHECK(!undo::can_redo(j));
    CHECK(undo::count(j) == 1);

    undo::clear(j);
    CHECK(!undo::can_undo(j) && undo::count(j) == 0);
}

static void components_come_and_go(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 8);

    entity::Handle h = entity::spawn(w, "thing");

    entity::Camera *c = entity::add_camera(w, h);
    c->fov_y = 45.0f;
    undo::record_add(j, h, entity::COMPONENT_CAMERA, c);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h) == NULL);
    CHECK(undo::redo(j, w, m));
    CHECK(entity::camera(w, h) && entity::camera(w, h)->fov_y == 45.0f);

    undo::record_remove(j, h, entity::COMPONENT_CAMERA, entity::camera(w, h));
    entity::remove_component(w, h, entity::COMPONENT_CAMERA);
    CHECK(entity::camera(w, h) == NULL);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h) && entity::camera(w, h)->fov_y == 45.0f);
    CHECK(entity::camera(w, h)->owner.v == h.v);
}

#define SIDECAR_TEST_FILE "zz_undo_test.glb"
#define SIDECAR_TEST_FILE_CFG "zz_undo_test.import.cfg"

static void asset_settings_write_the_sidecar_both_ways(memory::Arena *a) {
    asset::Config cfg{
        .root = CL_ASSET_ROOT,
        .default_shader = shader::Handle{0},
        .max_models = 8,
        .max_meshes = 8,
        .max_materials = 8,
        .max_skeletons = 4,
        .max_clips = 4,
        .scope_bytes = {KiB(256), KiB(256)},
    };
    asset::Manager *m = MUST(asset::init(a, a, cfg));
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 8);

    // nothing of this name is loaded, so only the file on disk changes
    asset::ClipOptions off{.rotation_only = false};
    asset::ClipOptions on{.rotation_only = true};
    asset::write_clip_options(m, SIDECAR_TEST_FILE, on);
    undo::record_clip_options(j, SIDECAR_TEST_FILE, off, on);

    CHECK(undo::undo(j, w, m));
    CHECK(!asset::clip_options(m, SIDECAR_TEST_FILE).rotation_only);
    CHECK(undo::redo(j, w, m));
    CHECK(asset::clip_options(m, SIDECAR_TEST_FILE).rotation_only);

    // a drag on the scale merges like a field edit
    asset::ImportOptions one{.scale = 1.0f, .z_up = false};
    asset::ImportOptions two{.scale = 2.0f, .z_up = false};
    asset::ImportOptions three{.scale = 3.0f, .z_up = false};
    undo::record_import_options(j, SIDECAR_TEST_FILE, one, two, true);
    undo::record_import_options(j, SIDECAR_TEST_FILE, two, three, true);
    CHECK(undo::count(j) == 2);

    CHECK(undo::undo(j, w, m));
    CHECK(asset::import_options(m, SIDECAR_TEST_FILE).scale == 1.0f);

    CHECK(file::remove(FPATH(a, CL_ASSET_ROOT, SIDECAR_TEST_FILE_CFG)));
}

static void a_dead_entity_is_skipped(memory::Arena *a) {
    asset::Manager *m = make_assets(a);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, 8);

    entity::Handle h = entity::spawn(w, "cam");
    entity::add_camera(w, h);
    set_fov(j, w, h, 90.0f, false);
    entity::destroy(w, h);

    test::mute();
    CHECK(undo::undo(j, w, m)); // moved past it
    CHECK(test::unmute() == 1);
    CHECK(!undo::can_undo(j));
}

test::Suite undo_suite() {
    static const test::Case cases[] = {
        CASE(field_edits_round_trip),
        CASE(continuing_edits_merge_until_closed),
        CASE(new_edits_drop_the_redo_tail_and_depth_the_oldest),
        CASE(components_come_and_go),
        CASE(asset_settings_write_the_sidecar_both_ways),
        CASE(a_dead_entity_is_skipped),
    };
    return test::Suite{.name = "undo", .cases = cases, .count = ARRAY_COUNT(cases)};
}
