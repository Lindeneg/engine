#include "asset/asset.h"

#include <string.h>

#include "suites.h"

static asset::Config small_config() {
    return asset::Config{
        // nothing here reads a file or draws, so root and shader only have to be present
        .root = ".",
        .default_shader = shader::Handle{0},
        .max_models = 8,
        .max_meshes = 8,
        .max_materials = 8,
        .max_skeletons = 4,
        .max_clips = 4,
        // small: init carves these from the global reserve for good, and every case builds a
        // manager
        .scope_bytes = {KiB(256), KiB(256)},
    };
}

static asset::Manager *make(memory::Arena *a) { return MUST(asset::init(a, a, small_config())); }

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

    // every index must address a real vertex
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

    test::mute();
    asset::Model *gone = asset::get(m, drop);
    CHECK(test::unmute() == 1); // the stale lookup warns, by design
    CHECK(strcmp(gone->name, "<fallback>") == 0);
}

// a recycled slot must not answer to the handle that used to own it
static void recycled_slots_reject_stale_handles(memory::Arena *a) {
    asset::Manager *m = make(a);

    // fill the scope, so after the unload the first slot comes back around and is reused
    asset::ModelHandle old{MUST(asset::create_model(m, asset::SCOPE_TRANSIENT, "old"))};
    while (asset::create_model(m, asset::SCOPE_TRANSIENT, "filler").err == result::OK)
        ;
    asset::unload_scope(m, asset::SCOPE_TRANSIENT);

    asset::ModelHandle fresh{MUST(asset::create_model(m, asset::SCOPE_TRANSIENT, "fresh"))};

    CHECK(asset::handle_index(old.v) == asset::handle_index(fresh.v)); // same slot reused
    CHECK(asset::handle_epoch(old.v) != asset::handle_epoch(fresh.v)); // different epoch

    CHECK(strcmp(asset::get(m, fresh)->name, "fresh") == 0);

    test::mute();
    asset::Model *stale = asset::get(m, old);
    CHECK(test::unmute() == 1);
    CHECK(strcmp(stale->name, "<fallback>") == 0);
}

static void garbage_handles_yield_the_fallback(memory::Arena *a) {
    asset::Manager *m = make(a);

    test::mute();
    CHECK(strcmp(asset::get(m, asset::ModelHandle{0xDEADBEEF})->name, "<fallback>") == 0);
    CHECK(asset::get(m, asset::MeshHandle{999})->vertex_count == 8);
    CHECK(test::unmute() == 2);
}

static void remove_is_catalog_only(memory::Arena *a) {
    asset::Manager *m = make(a);
    memory::Arena *scope = asset::scope_arena(m, asset::SCOPE_PERSISTENT);

    asset::ModelHandle h{MUST(asset::create_model(m, asset::SCOPE_PERSISTENT, "crate"))};
    memory::push(scope, 128); // stand in for the model's payload
    u64 used_before = scope->used;

    asset::remove(m, h);

    test::mute();
    CHECK(strcmp(asset::get(m, h)->name, "<fallback>") == 0);
    CHECK(test::unmute() == 1);

    CHECK(scope->used == used_before); // bytes survive until unload_scope
}

test::Suite asset_suite() {
    static const test::Case cases[] = {
        CASE(fallbacks_are_drawable),
        CASE(created_handles_resolve),
        CASE(payloads_live_in_the_scope_arena),
        CASE(exhausting_a_pool_reports_it),
        CASE(unload_invalidates_only_its_own_scope),
        CASE(recycled_slots_reject_stale_handles),
        CASE(garbage_handles_yield_the_fallback),
        CASE(remove_is_catalog_only),
    };
    return test::Suite{.name = "asset", .cases = cases, .count = ARRAY_COUNT(cases)};
}
