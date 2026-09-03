#include "editor/undo.h"

#include <stddef.h>
#include <string.h>

#include "platform/file.h"
#include "platform/path.h"
#include "suites.h"

#define DEPTH 8
#define BLOB 4096

static asset::Manager *make_assets(memory::Arena *a, const char *root) {
    return MUST(asset::init(a, a,
                            asset::Config{
                                .root = root,
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
                                    .max_cameras = 4,
                                    .max_lights = 2,
                                    .max_colliders = 4,
                                }));
}

// changes the camera's fov in place and journals it
static void set_fov(undo::Journal *j, entity::World *w, entity::Handle h, f32 fov, b32 continuing) {
    entity::Camera *c = entity::camera(w, h);
    f32 before = c->fov_y;
    c->fov_y = fov;
    undo::record_field(j, entity::id_of(w, h), entity::COMPONENT_CAMERA,
                       (u32)offsetof(entity::Camera, fov_y), sizeof(f32), &before, &c->fov_y,
                       continuing);
}

static void field_edits_round_trip(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

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
    undo::record_field(j, e->id, UNDO_ENTITY, (u32)offsetof(entity::Entity, name),
                       ENTITY_NAME_CAP, before, e->name, false);
    CHECK(undo::undo(j, w, m));
    CHECK(strcmp(entity::get(w, h)->name, "cam") == 0);
}

static void continuing_edits_merge_until_closed(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

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
    undo::record_field(j, entity::id_of(w, h), entity::COMPONENT_CAMERA,
                       (u32)offsetof(entity::Camera, z_far), sizeof(f32), &before, &c->z_far, true);
    CHECK(undo::count(j) == 2);
}

static void new_edits_drop_the_redo_tail_and_depth_the_oldest(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, 2, BLOB);

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

static void the_save_point_is_clean_until_the_journal_moves(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, 2, BLOB);

    entity::Handle h = entity::spawn(w, "cam");
    entity::add_camera(w, h);
    CHECK(undo::is_clean(j)); // a fresh load

    set_fov(j, w, h, 61.0f, false);
    CHECK(!undo::is_clean(j));
    undo::mark_clean(j);
    CHECK(undo::is_clean(j));

    // an edit dirties, undoing it back to the save point cleans, redoing dirties again
    set_fov(j, w, h, 62.0f, false);
    CHECK(!undo::is_clean(j));
    CHECK(undo::undo(j, w, m));
    CHECK(undo::is_clean(j));
    CHECK(undo::redo(j, w, m));
    CHECK(!undo::is_clean(j));

    // undoing past the save point and editing anew drops it with the redo tail
    CHECK(undo::undo(j, w, m) && undo::undo(j, w, m));
    set_fov(j, w, h, 70.0f, false);
    CHECK(!undo::is_clean(j));
    for (u32 i = 0; i < 3; i++)
        CHECK(!undo::undo(j, w, m) || !undo::is_clean(j));

    // and depth dropping the oldest entry loses the point too: the journal is
    // empty and two deep here, so the third edit pushes the first one off
    undo::mark_clean(j);
    set_fov(j, w, h, 71.0f, false);
    set_fov(j, w, h, 72.0f, false);
    CHECK(undo::undo(j, w, m) && undo::undo(j, w, m) && undo::is_clean(j)); // still reachable
    CHECK(undo::redo(j, w, m) && undo::redo(j, w, m));
    set_fov(j, w, h, 73.0f, false);
    CHECK(undo::undo(j, w, m) && undo::undo(j, w, m) && !undo::undo(j, w, m));
    CHECK(!undo::is_clean(j));
}

static void components_come_and_go(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

    entity::Handle h = entity::spawn(w, "thing");
    u32 id = entity::id_of(w, h);

    entity::Camera *c = entity::add_camera(w, h);
    c->fov_y = 45.0f;
    undo::record_add(j, id, entity::COMPONENT_CAMERA, c);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h) == NULL);
    CHECK(undo::redo(j, w, m));
    CHECK(entity::camera(w, h) && entity::camera(w, h)->fov_y == 45.0f);

    undo::record_remove(j, id, entity::COMPONENT_CAMERA, entity::camera(w, h));
    entity::remove_component(w, h, entity::COMPONENT_CAMERA);
    CHECK(entity::camera(w, h) == NULL);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, h) && entity::camera(w, h)->fov_y == 45.0f);
    CHECK(entity::camera(w, h)->owner.v == h.v);
}

// a parent with a camera and a child under it, positioned and named
static entity::Handle build_family(entity::World *w, entity::Handle *out_child) {
    entity::Handle parent = entity::spawn(w, "parent");
    entity::get(w, parent)->local.position = linalg::Vec3{1.0f, 2.0f, 3.0f};
    entity::add_camera(w, parent)->fov_y = 33.0f;

    entity::Handle child = entity::spawn(w, "child");
    entity::get(w, child)->local.position = linalg::Vec3{0.5f, 0.0f, 0.0f};
    *out_child = child;
    return parent;
}

static void a_deleted_subtree_comes_back_with_its_ids(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

    entity::Handle child;
    entity::Handle parent = build_family(w, &child);
    entity::set_parent(w, m, child, parent, NULL);
    u32 parent_id = entity::id_of(w, parent);
    u32 child_id = entity::id_of(w, child);

    // an older edit on the parent, then the delete
    set_fov(j, w, parent, 44.0f, false);
    CHECK(undo::record_delete(j, w, parent));
    entity::destroy(w, parent);
    CHECK(entity::find_by_id(w, parent_id).v == 0 && entity::find_by_id(w, child_id).v == 0);

    // undoing the delete brings both back under the same ids, in place
    CHECK(undo::undo(j, w, m));
    entity::Handle back = entity::find_by_id(w, parent_id);
    entity::Handle child_back = entity::find_by_id(w, child_id);
    CHECK(back.v && child_back.v);
    CHECK(entity::get(w, child_back)->parent.v == back.v);
    CHECK(entity::get(w, back)->local.position.z == 3.0f);
    CHECK(entity::camera(w, back)->fov_y == 44.0f);
    CHECK(strcmp(entity::get(w, back)->name, "parent") == 0);

    // and the older edit still knows the entity: undo it on the rebuilt one
    CHECK(undo::undo(j, w, m));
    CHECK(entity::camera(w, back)->fov_y == 33.0f);

    // redo both: the edit, then the delete again
    CHECK(undo::redo(j, w, m) && undo::redo(j, w, m));
    CHECK(entity::find_by_id(w, parent_id).v == 0);
}

static void a_created_entity_is_undone_and_redone(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

    entity::Handle h = entity::spawn(w, "fresh");
    u32 id = entity::id_of(w, h);
    CHECK(undo::record_create(j, w, h));

    CHECK(undo::undo(j, w, m));
    CHECK(entity::find_by_id(w, id).v == 0);
    CHECK(undo::redo(j, w, m));
    CHECK(entity::find_by_id(w, id).v != 0);
    CHECK(strcmp(entity::get(w, entity::find_by_id(w, id))->name, "fresh") == 0);
}

static void a_reparent_is_undone_with_its_transform(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

    entity::Handle child;
    entity::Handle parent = build_family(w, &child);
    entity::Handle other = entity::spawn(w, "other");

    entity::set_parent(w, m, child, parent, NULL);
    entity::Transform before = entity::get(w, child)->local;

    // moved under another parent, with a different local position to go with it
    entity::set_parent(w, m, child, other, NULL);
    entity::get(w, child)->local.position = linalg::Vec3{9.0f, 9.0f, 9.0f};
    undo::record_reparent(j, entity::id_of(w, child), entity::id_of(w, parent),
                          entity::id_of(w, other), NULL, &before, &entity::get(w, child)->local);

    CHECK(undo::undo(j, w, m));
    CHECK(entity::get(w, child)->parent.v == parent.v);
    CHECK(entity::get(w, child)->local.position.x == 0.5f);

    CHECK(undo::redo(j, w, m));
    CHECK(entity::get(w, child)->parent.v == other.v);
    CHECK(entity::get(w, child)->local.position.x == 9.0f);
}

static void a_subtree_too_big_is_refused(memory::Arena *a) {
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, 64); // a blob nothing real fits

    entity::Handle child;
    entity::Handle parent = build_family(w, &child);
    (void)m;

    test::mute();
    CHECK(!undo::record_delete(j, w, parent));
    CHECK(test::unmute() == 1);
    CHECK(undo::count(j) == 0);
}

#define SIDECAR_TEST_FILE "zz_undo_test.glb"
#define SIDECAR_TEST_FILE_CFG "zz_undo_test.import.cfg"

static void asset_settings_write_the_sidecar_both_ways(memory::Arena *a) {
    asset::Manager *m = make_assets(a, CL_ASSET_ROOT);
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

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
    asset::Manager *m = make_assets(a, ".");
    entity::World *w = make_world(a);
    undo::Journal *j = undo::create(a, a, DEPTH, BLOB);

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
        CASE(the_save_point_is_clean_until_the_journal_moves),
        CASE(components_come_and_go),
        CASE(a_deleted_subtree_comes_back_with_its_ids),
        CASE(a_created_entity_is_undone_and_redone),
        CASE(a_reparent_is_undone_with_its_transform),
        CASE(a_subtree_too_big_is_refused),
        CASE(asset_settings_write_the_sidecar_both_ways),
        CASE(a_dead_entity_is_skipped),
    };
    return test::Suite{.name = "undo", .cases = cases, .count = ARRAY_COUNT(cases)};
}
