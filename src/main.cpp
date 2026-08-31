#include "asset.h"
#include "common.h"
#include "logger.h"
#include "memory.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(1024));
    memory::Arena perm = memory::create(MiB(64));
    memory::Arena scratch = memory::create(MiB(64));

    asset::Config cfg{
        .max_models = 64,
        .max_meshes = 256,
        .max_materials = 256,
        .max_skeletons = 32,
        .max_clips = 128,
        .scope_bytes = {MiB(128), MiB(128)},
    };

    asset::Manager *assets{MUST(asset::init(&perm, &scratch, cfg))};

    asset::ModelHandle soldier{OR_ELSE(
        asset::load_model(assets, asset::SCOPE_PERSISTENT, "characters/soldier.glb"),
        asset::MODEL_FALLBACK)};

    asset::ModelHandle pistol{OR_ELSE(
        asset::load_model(assets, asset::SCOPE_PERSISTENT, "weapons/pistol.glb"),
        asset::MODEL_FALLBACK)};

    asset::Model *s = asset::get(assets, soldier);
    asset::Skeleton *skel = asset::get(assets, s->skeleton);

    asset::ClipHandle idle{
        OR_ELSE(asset::load_clip(assets, asset::SCOPE_PERSISTENT, "animations/pistol_idle.glb",
                                 s->skeleton),
                asset::CLIP_FALLBACK)};

    LOG_INFO("soldier: %u mesh(es), %u joints", s->mesh_count, skel->joint_count);
    for (u32 i = 0; i < s->mesh_count; i++) {
        asset::Mesh *mesh = asset::get(assets, s->meshes[i]);
        LOG_INFO("  mesh %u: %u verts, %u indices, %u submeshes, %s", i, mesh->vertex_count,
                 mesh->index_count, mesh->submesh_count,
                 mesh->vertex_kind == asset::VERTEX_SKINNED ? "skinned" : "static");
    }

    asset::Model *p = asset::get(assets, pistol);
    for (u32 i = 0; i < p->mesh_count; i++) {
        asset::Mesh *mesh = asset::get(assets, p->meshes[i]);
        LOG_INFO("pistol mesh %u: %u verts, %u indices, %u submeshes", i, mesh->vertex_count,
                 mesh->index_count, mesh->submesh_count);
    }

    asset::Clip *clip = asset::get(assets, idle);
    LOG_INFO("clip \"%s\": %.3fs, %u channels, %u bound", clip->name, (f64)clip->duration,
             clip->channel_count, clip->bound_count);

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_PERSISTENT), "assets");
#endif

    logger::shutdown();
    return EXIT_SUCCESS;
}
