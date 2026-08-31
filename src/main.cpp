#include "anim.h"
#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "logger.h"
#include "memory.h"
#include "render.h"
#include "window.h"

// Bounds of a model with a joint palette applied. Raw vertex bounds are
// meaningless for a skinned mesh -- they are pre-skin, in whatever space the
// inverse bind matrices convert from. Belongs in asset:: once physics needs it.
static void posed_bounds(asset::Manager *assets, asset::ModelHandle h, const linalg::Mat4 *palette,
                         u32 joint_count, linalg::Vec3 *out_lo, linalg::Vec3 *out_hi) {
    asset::Model *model = asset::get(assets, h);
    linalg::Vec3 lo{1e30f, 1e30f, 1e30f};
    linalg::Vec3 hi{-1e30f, -1e30f, -1e30f};

    for (u32 i = 0; i < model->mesh_count; i++) {
        asset::Mesh *mesh = asset::get(assets, model->meshes[i]);

        for (u32 v = 0; v < mesh->vertex_count; v++) {
            linalg::Vec3 p = asset::vertex_pos(mesh, v);
            linalg::Vec4 h4{p.x, p.y, p.z, 1.0f};

            if (mesh->vertex_kind == asset::VERTEX_SKINNED) {
                const asset::VertexSkinned *sv = &mesh->vertices.skinned[v];
                linalg::Vec4 acc{};
                f32 wsum = 0.0f;
                for (u32 k = 0; k < 4; k++) {
                    f32 w = sv->weights[k];
                    if (w <= 0.0f || sv->joints[k] >= joint_count)
                        continue;
                    wsum += w;
                    acc = acc + linalg::mat4_mul_vec4(palette[sv->joints[k]], h4) * w;
                }
                if (wsum > 1e-4f)
                    h4 = acc;
            }

            linalg::Vec4 w4 = linalg::mat4_mul_vec4(model->import, h4);
            lo.x = MIN(lo.x, w4.x);
            lo.y = MIN(lo.y, w4.y);
            lo.z = MIN(lo.z, w4.z);
            hi.x = MAX(hi.x, w4.x);
            hi.y = MAX(hi.y, w4.y);
            hi.z = MAX(hi.z, w4.z);
        }
    }

    *out_lo = lo;
    *out_hi = hi;
}

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(1024));
    memory::Arena perm = memory::create(MiB(64));
    memory::Arena scratch = memory::create(MiB(64));

    window::Window *win{MUST(window::create(&perm, window::Config{
                                                       .width = 1280,
                                                       .height = 720,
                                                       .title = "explore",
                                                       .vsync = true,
                                                   }))};

    i32 fb_w = 0;
    i32 fb_h = 0;
    window::framebuffer_size(win, &fb_w, &fb_h);
    render::init(fb_w, fb_h);

    asset::Config cfg{
        .max_models = 64,
        .max_meshes = 256,
        .max_materials = 256,
        .max_skeletons = 32,
        .max_clips = 128,
        .scope_bytes = {MiB(128), MiB(128)},
    };

    asset::Manager *assets{MUST(asset::init(&perm, &scratch, cfg))};
    MUST(render::load_shaders(&scratch, asset::root(assets)));

    // The soldier needs almost no import transform: its inverse bind matrices
    // already carry a 14.71x mesh->skeleton scale and the Z-up to Y-up
    // rotation, so skinning does that work. Only the raw, unskinned vertices
    // look small and sideways. 0.143 * 14.71 = 2.10, trimmed to a 1.8m height.
    asset::ModelHandle soldier{
        OR_ELSE(asset::load_model(assets, asset::SCOPE_PERSISTENT, "characters/soldier.glb",
                                  asset::ImportOptions{.scale = 0.857f, .z_up = false}),
                asset::MODEL_FALLBACK)};
    asset::ModelHandle pistol{
        OR_ELSE(asset::load_model(assets, asset::SCOPE_PERSISTENT, "weapons/pistol.glb",
                                  asset::ImportOptions{.scale = 0.0012f, .z_up = false}),
                asset::MODEL_FALLBACK)};

    render::upload_model(assets, soldier);
    render::upload_model(assets, pistol);

    asset::Skeleton *skel = asset::get(assets, asset::get(assets, soldier)->skeleton);
    asset::ClipHandle idle{
        OR_ELSE(asset::load_clip(assets, asset::SCOPE_PERSISTENT, "animations/pistol_idle.glb",
                                 asset::get(assets, soldier)->skeleton),
                asset::CLIP_FALLBACK)};
    asset::Clip *clip = asset::get(assets, idle);

    linalg::Mat4 palette[RENDER_MAX_JOINTS];

    // The bind palette is NOT identity in general: inverse bind matrices carry
    // whatever scale and rotation separates mesh space from skeleton space. The
    // length of a column is that scale, which is the number import settings
    // have to account for.
    anim::sample(&scratch, skel, NULL, 0.0f, palette);
    f32 skin_scale = linalg::vec3_length(
        linalg::Vec3{palette[0].m[0], palette[0].m[1], palette[0].m[2]});

    LOG_INFO("skeleton: %u joints, mesh->skeleton scale %.4f", skel->joint_count, (f64)skin_scale);


    // Frame the camera off real bounds. Guessing a distance is how you end up
    // staring at a black window wondering which of five things is broken.
    linalg::Vec3 lo, hi;
    posed_bounds(assets, soldier, palette, skel->joint_count, &lo, &hi);
    linalg::Vec3 center = (lo + hi) * 0.5f;
    linalg::Vec3 extent = hi - lo;
    f32 radius = linalg::vec3_length(extent) * 0.5f;
    f32 dist = radius / tanf(DEG2RAD(45.0f) * 0.5f) * 1.15f;

    LOG_INFO("soldier posed: (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)  size %.3f x %.3f x %.3f",
             (f64)lo.x, (f64)lo.y, (f64)lo.z, (f64)hi.x, (f64)hi.y, (f64)hi.z, (f64)extent.x,
             (f64)extent.y, (f64)extent.z);

    linalg::Vec3 plo, phi;
    asset::model_bounds(assets, pistol, &plo, &phi);
    linalg::Vec3 pext = phi - plo;
    LOG_INFO("pistol  bounds: (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)  size %.3f x %.3f x %.3f",
             (f64)plo.x, (f64)plo.y, (f64)plo.z, (f64)phi.x, (f64)phi.y, (f64)phi.z, (f64)pext.x,
             (f64)pext.y, (f64)pext.z);

    while (!window::should_close(win)) {
        window::poll(win);
        if (window::key_down(win, window::KEY_ESCAPE))
            break;

        i32 w = 0;
        i32 h = 0;
        window::framebuffer_size(win, &w, &h);
        if (w != fb_w || h != fb_h) {
            fb_w = w;
            fb_h = h;
            render::resize(fb_w, fb_h);
        }

        f32 t = (f32)window::time();
        linalg::Vec3 eye{center.x + sinf(t * 0.4f) * dist, center.y + radius * 0.35f,
                         center.z + cosf(t * 0.4f) * dist};

        linalg::Mat4 view = linalg::mat4_look_at(eye, center, linalg::Vec3{0.0f, 1.0f, 0.0f});
        linalg::Mat4 proj = linalg::mat4_perspective(DEG2RAD(45.0f), (f32)fb_w / (f32)fb_h,
                                                     radius * 0.01f, dist * 4.0f);
        linalg::Mat4 view_proj = proj * view;

        render::begin_frame(linalg::Vec4{0.10f, 0.11f, 0.13f, 1.0f});

        anim::sample(&scratch, skel, clip, t, palette);
        render::set_joint_palette(palette, skel->joint_count);

        render::draw_model(assets, soldier, linalg::mat4_identity(), view_proj);
        render::draw_model(
            assets, pistol,
            linalg::mat4_from_translation(linalg::Vec3{extent.x * 0.9f, center.y, 0.0f}),
            view_proj);

        window::swap(win);
    }

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_PERSISTENT), "assets");
#endif

    window::destroy(win);
    logger::shutdown();
    return EXIT_SUCCESS;
}
