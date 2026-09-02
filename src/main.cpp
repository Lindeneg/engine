#include "asset.h"
#include "common.h"
#include "entity.h"
#include "linalg.h"
#include "logger.h"
#include "memory.h"
#include "render.h"
#include "shader.h"
#include "window.h"

// CMake defines this as the source tree's assets folder. The fallback only
// matters for a build that bypasses CMake entirely.
#ifndef CL_ASSET_ROOT
#define CL_ASSET_ROOT "assets"
#endif

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

    // The assets folder in the source tree, baked in by CMake. There is exactly
    // one of them and nothing copies it, so editing a shader, an .entity or a
    // .level and restarting is enough -- no build in between. A shipped build
    // would take this from a launch argument instead.
    const char *root = CL_ASSET_ROOT;

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

    asset::Manager *assets{MUST(asset::init(&perm, &scratch,
                                            asset::Config{
                                                .root = root,
                                                .default_shader = shader::default_handle(),
                                                .max_models = 64,
                                                .max_meshes = 256,
                                                .max_materials = 256,
                                                .max_skeletons = 32,
                                                .max_clips = 128,
                                                .scope_bytes = {MiB(128), MiB(128)},
                                            }))};

    render::attach_assets(&perm, assets);

    entity::World *world{MUST(entity::init(&perm, entity::Config{
                                                      .max_entities = 4096,
                                                      .max_renderables = 2048,
                                                      .max_animators = 64,
                                                      .max_cameras = 8,
                                                      .max_lights = 16,
                                                      .fixed_hz = 120.0f,
                                                      .max_fixed_steps = 8,
                                                  }))};

    // Everything the world contains comes from here. No model handles, import
    // scales or attachment offsets in this file.
    MUST(entity::load_level(world, assets, "test"));

    // Frame the camera off the player's real bounds. Guessing a distance is how
    // you end up staring at a black window wondering which of five things is
    // broken.
    linalg::Vec3 center{};
    f32 radius = 1.0f;

    // the first renderable *root*: iterating raw would find the attached weapon
    // just as readily as the soldier holding it
    entity::Renderable *player = NULL;
    for (entity::Handle h = entity::first(world); h.v; h = entity::next(world, h)) {
        if (entity::get(world, h)->parent.v)
            continue;
        player = entity::renderable(world, h);
        if (player)
            break;
    }

    if (player) {
        linalg::Vec3 lo, hi;
        asset::model_bounds(assets, player->model, &lo, &hi);
        center = (lo + hi) * 0.5f;
        radius = linalg::vec3_length(hi - lo) * 0.5f;
        LOG_INFO("camera: framing (%.3f %.3f %.3f) radius %.3f", (f64)center.x, (f64)center.y,
                 (f64)center.z, (f64)radius);
    } else {
        LOG_WARN("camera: the level spawned nothing renderable");
    }

    // The camera the level declared. Its fov drives the framing distance, so
    // changing fov in the .entity file reframes the shot without touching this.
    entity::Handle cam = entity::active_camera(world);
    f32 fov = cam.v ? entity::camera(world, cam)->fov_y : 60.0f;
    f32 dist = radius / tanf(DEG2RAD(fov) * 0.5f) * 1.15f;

    f64 last = window::time();

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

        f64 now = window::time();
        f32 dt = (f32)(now - last);
        last = now;

        // Standing in for a player controller. The camera is an entity, and
        // something has to move it -- when there is input, that something
        // becomes a controller and this goes away. The camera does not care who
        // writes its transform.
        if (cam.v) {
            f32 t = (f32)now;
            linalg::Vec3 eye{center.x + sinf(t * 0.4f) * dist, center.y + radius * 0.35f,
                             center.z + cosf(t * 0.4f) * dist};

            entity::Entity *ce = entity::get(world, cam);
            ce->local.position = eye;
            entity::set_rotation(&ce->local, linalg::quat_look_rotation(
                                                 center - eye, linalg::Vec3{0.0f, 1.0f, 0.0f}));
        }

        entity::update(world, assets, &scratch, dt);

        // View, projection and lighting all come from entities now.
        shader::FrameData frame{};
        if (!entity::build_frame(world, (f32)fb_w / (f32)fb_h, &frame))
            LOG_WARN_ONCE("main: the level has no active camera, nothing will be visible");

        render::begin_frame(&frame, linalg::Vec4{0.10f, 0.11f, 0.13f, 1.0f});
        entity::submit(world, assets);
        render::flush(assets);

        window::swap(win);
    }

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_PERSISTENT), "assets-persistent");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_TRANSIENT), "assets-transient");
#endif

    render::shutdown();
    shader::shutdown();
    window::destroy(win);
    logger::shutdown();
    return EXIT_SUCCESS;
}
