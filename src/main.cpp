#include "anim.h"
#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "logger.h"
#include "memory.h"
#include "path.h"
#include "render.h"
#include "shader.h"
#include "window.h"

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

    // Both the shader registry and the asset manager hang off this, so it is
    // resolved once here and handed to each rather than owned by either.
    char root[512];
    ASSERT_MSG(path::exe_dir(root, sizeof(root)), "cannot resolve the executable directory");

    // Shaders before assets: a material is given its program at load, and a
    // shader that will not build means the deployment is broken, so this
    // aborts rather than limping on.
    MUST(shader::init(&perm, &scratch, root));

    render::init(&perm,
                 render::Config{
                     .max_draw_items = 4096,
                     .max_objects = 512,
                     .max_skinned_draws = 32,
                 },
                 fb_w, fb_h);

    asset::Config cfg{
        .root = root,
        .default_shader = shader::default_handle(),
        .max_models = 64,
        .max_meshes = 256,
        .max_materials = 256,
        .max_skeletons = 32,
        .max_clips = 128,
        .scope_bytes = {MiB(128), MiB(128)},
    };

    asset::Manager *assets{MUST(asset::init(&perm, &scratch, cfg))};

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

    // rotation_only is stated, not guessed: this clip was authored for another
    // rig, and only its author knows that. See ClipOptions.
    asset::ClipHandle idle{
        OR_ELSE(asset::load_clip(assets, asset::SCOPE_PERSISTENT, "animations/pistol_idle.glb",
                                 asset::get(assets, soldier)->skeleton,
                                 asset::ClipOptions{.rotation_only = true}),
                asset::CLIP_FALLBACK)};
    asset::Clip *clip = asset::get(assets, idle);

    linalg::Mat4 palette[SHADER_MAX_JOINTS];

    // Frame the camera off real bounds. Guessing a distance is how you end up
    // staring at a black window wondering which of five things is broken.
    // These are bind-pose bounds, cached at load, so they are the model's
    // actual shape rather than its pre-skin vertex soup.
    linalg::Vec3 lo, hi;
    asset::model_bounds(assets, soldier, &lo, &hi);
    linalg::Vec3 center = (lo + hi) * 0.5f;
    linalg::Vec3 extent = hi - lo;
    f32 radius = linalg::vec3_length(extent) * 0.5f;
    f32 dist = radius / tanf(DEG2RAD(45.0f) * 0.5f) * 1.15f;

    LOG_INFO("soldier bounds: (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)  size %.3f x %.3f x %.3f",
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

        // Lighting is frame state rather than a constant in the fragment
        // shader. Where these numbers come from is the app's business -- for
        // now literals here, a scene config file once there is a scene.
        shader::FrameData frame{
            .view_proj = proj * view,
            .camera_pos = linalg::Vec4{eye.x, eye.y, eye.z, 0.0f},
            .light_dir = linalg::Vec4{0.4f, 0.9f, 0.35f, 0.0f},
            .light_color = linalg::Vec4{0.72f, 0.72f, 0.72f, 0.28f},
        };

        render::begin_frame(&frame, linalg::Vec4{0.10f, 0.11f, 0.13f, 1.0f});

        anim::sample(&scratch, skel, clip, t, palette);

        // Submission order carries no meaning now -- flush sorts. The palette
        // is copied on submit, so it can be reused for the next model.
        render::submit_model(assets, soldier, linalg::mat4_identity(), palette,
                             skel->joint_count);
        render::submit_model(
            assets, pistol,
            linalg::mat4_from_translation(linalg::Vec3{extent.x * 0.9f, center.y, 0.0f}), NULL, 0);

        render::flush(assets);

        window::swap(win);
    }

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_PERSISTENT), "assets");
#endif

    render::shutdown();
    shader::shutdown();
    window::destroy(win);
    logger::shutdown();
    return EXIT_SUCCESS;
}
