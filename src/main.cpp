#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "scene/entity.h"
#include "math/linalg.h"
#include "core/logger.h"
#include "core/memory.h"
#include "platform/path.h"
#include "gfx/render.h"
#include "gfx/shader.h"
#include "platform/window.h"

// the assets folder in the source tree, baked in by CMake; a shipped build would
// take a launch argument instead
#ifndef CL_ASSET_ROOT
#error "CL_ASSET_ROOT must be defined by the build; see CMakeLists.txt"
#endif

#define ENGINE_CFG "engine.cfg"

// address space only, free to reserve; every arena is carved from it, sized by engine.cfg
#define RESERVE_BYTES GiB(4)

// holds engine.cfg, which sizes every other arena
#define BOOT_ARENA_BYTES MiB(1)

#define DEFAULT_LEVEL "test"
#define DEFAULT_PERM_MIB 64
#define DEFAULT_SCRATCH_MIB 64
#define DEFAULT_ORBIT_SPEED 0.4f
#define DEFAULT_ORBIT_HEIGHT 0.35f
#define DEFAULT_ORBIT_DISTANCE 1.15f

int main() {
    logger::init(NULL);
    memory::init(RESERVE_BYTES);

    const char *root = CL_ASSET_ROOT;

    memory::Arena boot = memory::create(BOOT_ARENA_BYTES);
    config::Doc *engine = MUST(config::load(&boot, FPATH(&boot, root, ENGINE_CFG)));
    if (engine->problems)
        LOG_FATAL("main: %s has %u structural problem(s)", ENGINE_CFG, engine->problems);
    config::Node *cfg = engine->first;

    logger::Level level = logger::TRACE;
    const char *level_name = config::get(cfg, "log_level", NULL);
    if (level_name && !logger::level_from_name(level_name, &level))
        LOG_WARN("main: unknown log_level '%s'", level_name);
    logger::set_level(level);

    config::Node *mem = config::find(cfg, "memory");
    config::Node *mem_body = mem ? mem->first_child : NULL;
    memory::Arena perm = memory::create(MiB(config::get_i32(mem_body, "perm", DEFAULT_PERM_MIB)));
    memory::Arena scratch =
        memory::create(MiB(config::get_i32(mem_body, "scratch", DEFAULT_SCRATCH_MIB)));

    window::Window *win{
        MUST(window::create(&perm, window::config_from(config::find(cfg, "window"))))};

    i32 fb_w = 0;
    i32 fb_h = 0;
    window::framebuffer_size(win, &fb_w, &fb_h);

    // shaders before assets: materials get their program at load
    MUST(shader::init(&perm, &scratch, root));

    render::Config render_cfg = render::config_from(config::find(cfg, "render"));
    render::init(&perm, render_cfg, fb_w, fb_h);

    asset::Manager *assets{MUST(asset::init(
        &perm, &scratch,
        asset::config_from(config::find(cfg, "assets"), root, shader::default_handle())))};

    render::attach_assets(&perm, assets);

    entity::World *world{MUST(entity::init(&perm, entity::config_from(config::find(cfg, "world"))))};

    // everything in the world comes from the level file
    MUST(entity::load_level(world, assets, config::get(cfg, "level", DEFAULT_LEVEL)));

    // frame the camera off real bounds rather than a guessed distance
    linalg::Vec3 center{};
    f32 radius = 1.0f;

    // the first renderable root: iterating raw could find the attached weapon first
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

    // stand-in for a player controller: orbit the level's camera around the
    // player, its fov driving the distance
    config::Node *orbit = config::find(cfg, "camera_orbit");
    config::Node *orbit_body = orbit ? orbit->first_child : NULL;
    f32 orbit_speed = config::get_f32(orbit_body, "speed", DEFAULT_ORBIT_SPEED);
    f32 orbit_height = config::get_f32(orbit_body, "height", DEFAULT_ORBIT_HEIGHT);
    f32 orbit_distance = config::get_f32(orbit_body, "distance", DEFAULT_ORBIT_DISTANCE);

    entity::Handle cam = entity::active_camera(world);
    const entity::Camera *cam_defaults =
        (const entity::Camera *)entity::component_desc(entity::COMPONENT_CAMERA)->defaults;
    f32 fov = cam.v ? entity::camera(world, cam)->fov_y : cam_defaults->fov_y;
    f32 dist = radius / tanf(DEG2RAD(fov) * 0.5f) * orbit_distance;

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

        if (cam.v) {
            f32 t = (f32)now * orbit_speed;
            linalg::Vec3 eye{center.x + sinf(t) * dist, center.y + radius * orbit_height,
                             center.z + cosf(t) * dist};

            entity::Entity *ce = entity::get(world, cam);
            ce->local.position = eye;
            entity::set_rotation(&ce->local, linalg::quat_look_rotation(
                                                 center - eye, linalg::Vec3{0.0f, 1.0f, 0.0f}));
        }

        entity::update(world, assets, &scratch, dt);

        shader::FrameData frame{};
        if (!entity::build_frame(world, (f32)fb_w / (f32)fb_h, &frame))
            LOG_WARN_ONCE("main: the level has no active camera, nothing will be visible");

        render::begin_frame(&frame, render_cfg.clear_color);
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
