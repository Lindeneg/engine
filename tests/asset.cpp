#include <string.h>

#include "asset.h"
#include "logger.h"
#include "suites.h"

static asset::Config small_config() {
    return asset::Config{
        .max_models = 8,
        .max_meshes = 8,
        .max_materials = 8,
        .max_skeletons = 4,
        .max_clips = 4,
        // small on purpose: init carves these from the global reserve and never
        // returns them, and every case here builds its own manager
        .scope_bytes = {KiB(256), KiB(256)},
    };
}

static asset::Manager *make(memory::Arena *a) {
    return MUST(asset::init(a, a, small_config()));
}

static void fallbacks_are_drawable(memory::Arena *a) {
    asset::Manager *m = make(a);

    asset::Model *model = asset::get(m, asset::MODEL_FALLBACK);
    CHECK(model != NULL);
    CHECK(model->mesh_count == 1);

    asset::Mesh *mesh = asset::get(m, model->meshes[0]);
    CHECK(mesh->vertex_count == 8);
    CHECK(mesh->index_count == 36);
    CHECK(mesh->submesh_count == 1);
    CHECK(mesh->vertex_kind == asset::VERTEX_STATIC);

    // every index has to address a real vertex or the fallback crashes the
    // renderer it exists to protect
    for (u32 i = 0; i < mesh->index_count; i++)
        CHECK(mesh->indices[i] < mesh->vertex_count);

    asset::Material *mat = asset::get(m, mesh->submeshes[0].material);
    CHECK(mat->base_color.x == 1.0f && mat->base_color.y == 0.0f && mat->base_color.z == 1.0f);

    asset::Skeleton *skel = asset::get(m, model->skeleton);
    CHECK(skel->joint_count == 1);
    CHECK(skel->joints[0].parent == -1);
}

static void created_handles_resolve(memory::Arena *a) {
    asset::Manager *m = make(a);

    asset::ModelHandle h{MUST(asset::create_model(m, asset::SCOPE_PERSISTENT, "rifle"))};
    CHECK(h.v != 0); // must not collide with the fallback

    asset::Model *model = asset::get(m, h);
    CHECK(strcmp(model->name, "rifle") == 0);

    // distinct creates must not alias
    asset::ModelHandle h2{MUST(asset::create_model(m, asset::SCOPE_PERSISTENT, "pistol"))};
    CHECK(h2.v != h.v);
    CHECK(strcmp(asset::get(m, h)->name, "rifle") == 0);
    CHECK(strcmp(asset::get(m, h2)->name, "pistol") == 0);
}

static void payloads_live_in_the_scope_arena(memory::Arena *a) {
    asset::Manager *m = make(a);

    asset::MeshHandle h{MUST(asset::create_mesh(m, asset::SCOPE_TRANSIENT))};
    memory::Arena *scope = asset::scope_arena(m, asset::SCOPE_TRANSIENT);

    asset::Mesh *mesh = asset::get(m, h);
    mesh->vertex_kind = asset::VERTEX_SKINNED;
    mesh->vertices.skinned = push_array(scope, asset::VertexSkinned, 3);
    mesh->vertex_count = 3;
    mesh->vertices.skinned[1].pos = linalg::Vec3{1.0f, 2.0f, 3.0f};

    CHECK(scope->used > 0);
    CHECK(asset::vertex_pos(mesh, 1).y == 2.0f);
}

static void exhausting_a_pool_reports_it(memory::Arena *a) {
    asset::Manager *m = make(a);

    // cap is 4 and slot 0 is the fallback, so 3 clips fit
    for (u32 i = 0; i < 3; i++)
        CHECK(asset::create_clip(m, asset::SCOPE_PERSISTENT).err == result::OK);

    asset::ClipResult over = asset::create_clip(m, asset::SCOPE_PERSISTENT);
    CHECK(over.err == result::ERR_EXHAUSTED);
    CHECK(over.val.v == 0);
}

static void unload_invalidates_only_its_own_scope(memory::Arena *a) {
    asset::Manager *m = make(a);

    asset::ModelHandle keep{MUST(asset::create_model(m, asset::SCOPE_PERSISTENT, "player"))};
    asset::ModelHandle drop{MUST(asset::create_model(m, asset::SCOPE_TRANSIENT, "crate"))};

    asset::unload_scope(m, asset::SCOPE_TRANSIENT);

    CHECK(strcmp(asset::get(m, keep)->name, "player") == 0);

    logger::set_level(logger::FATAL); // the stale lookup warns, by design
    asset::Model *gone = asset::get(m, drop);
    logger::set_level(logger::TRACE);
    CHECK(strcmp(gone->name, "<fallback>") == 0);
}

// the whole reason handles carry an epoch: a recycled slot must not answer to
// the handle that used to own it
static void recycled_slots_reject_stale_handles(memory::Arena *a) {
    asset::Manager *m = make(a);

    asset::ModelHandle old{MUST(asset::create_model(m, asset::SCOPE_TRANSIENT, "old"))};
    asset::unload_scope(m, asset::SCOPE_TRANSIENT);

    asset::ModelHandle fresh{MUST(asset::create_model(m, asset::SCOPE_TRANSIENT, "fresh"))};

    CHECK(asset::handle_index(old.v) == asset::handle_index(fresh.v)); // same slot reused
    CHECK(asset::handle_epoch(old.v) != asset::handle_epoch(fresh.v)); // different epoch

    CHECK(strcmp(asset::get(m, fresh)->name, "fresh") == 0);

    logger::set_level(logger::FATAL);
    asset::Model *stale = asset::get(m, old);
    logger::set_level(logger::TRACE);
    CHECK(strcmp(stale->name, "<fallback>") == 0);
}

static void garbage_handles_yield_the_fallback(memory::Arena *a) {
    asset::Manager *m = make(a);

    logger::set_level(logger::FATAL);
    CHECK(strcmp(asset::get(m, asset::ModelHandle{0xDEADBEEF})->name, "<fallback>") == 0);
    CHECK(asset::get(m, asset::MeshHandle{999})->vertex_count == 8);
    logger::set_level(logger::TRACE);
}

static void remove_is_catalog_only(memory::Arena *a) {
    asset::Manager *m = make(a);
    memory::Arena *scope = asset::scope_arena(m, asset::SCOPE_PERSISTENT);

    asset::ModelHandle h{MUST(asset::create_model(m, asset::SCOPE_PERSISTENT, "crate"))};
    memory::push(scope, 128); // stand in for the model's payload
    u64 used_before = scope->used;

    asset::remove(m, h);

    logger::set_level(logger::FATAL);
    CHECK(strcmp(asset::get(m, h)->name, "<fallback>") == 0);
    logger::set_level(logger::TRACE);

    CHECK(scope->used == used_before); // bytes survive until unload_scope
}

test::Suite asset_suite() {
    static const test::Case cases[] = {
        {"fallbacks_are_drawable", fallbacks_are_drawable},
        {"created_handles_resolve", created_handles_resolve},
        {"payloads_live_in_the_scope_arena", payloads_live_in_the_scope_arena},
        {"exhausting_a_pool_reports_it", exhausting_a_pool_reports_it},
        {"unload_invalidates_only_its_own_scope", unload_invalidates_only_its_own_scope},
        {"recycled_slots_reject_stale_handles", recycled_slots_reject_stale_handles},
        {"garbage_handles_yield_the_fallback", garbage_handles_yield_the_fallback},
        {"remove_is_catalog_only", remove_is_catalog_only},
    };
    return test::Suite{.name = "asset", .cases = cases, .count = ARRAY_COUNT(cases)};
}
