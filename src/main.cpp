#include <string.h>

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/memory.h"
#include "game/player.h"
#include "gfx/render.h"
#include "gfx/shader.h"
#include "platform/input.h"
#include "platform/path.h"
#include "platform/window.h"
#include "scene/entity.h"

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
#define DEFAULT_PLAYER_BODY "player"

static entity::Handle find_root(entity::World *w, const char *name) {
    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h)) {
        entity::Entity *e = entity::get(w, h);
        if (e && !e->parent.v && strcmp(e->name, name) == 0)
            return h;
    }
    return entity::NONE;
}

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

    input::State *in{MUST(input::init(&perm, &scratch, root))};

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

    entity::World *world{
        MUST(entity::init(&perm, entity::config_from(config::find(cfg, "world"))))};

    // everything in the world comes from the level file
    MUST(entity::load_level(world, assets, config::get(cfg, "level", DEFAULT_LEVEL)));

    // The player is game code driving an entity the level placed. The engine
    // knows nothing about it beyond the handle it is given.
    config::Node *player_cfg = config::find(cfg, "player");
    const char *body_name =
        config::get(player_cfg ? player_cfg->first_child : NULL, "body", DEFAULT_PLAYER_BODY);

    entity::Handle body = find_root(world, body_name);
    if (!body.v)
        LOG_WARN("main: the level has no root entity named \"%s\", so nothing is driven",
                 body_name);

    game::Player player = game::create(player_cfg, in, body);

    input::Map gameplay = input::find_map(in, "gameplay");
    input::Action quit = input::find(in, "quit");
    input::Action toggle_look = input::find(in, "toggle_mouse_look");

    // playable without a click to start; the toggle hands the cursor back
    input::set_mouse_look(in, win, true);

    f64 last = window::time();

    while (!window::should_close(win)) {
        window::poll(win);
        input::update(in, win);

        if (input::pressed(in, quit))
            break;

        if (input::pressed(in, toggle_look)) {
            b32 look = !input::mouse_look(in);
            input::set_mouse_look(in, win, look);

            // With the cursor loose, gameplay stops receiving anything. This is
            // the same seam an editor panel will use when it takes focus.
            input::set_map_enabled(in, gameplay, look);
        }

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

        // aim first, so the fixed steps below move in the direction being looked
        game::aim(&player, in);

        u32 steps = entity::begin_frame(world, dt);
        for (u32 i = 0; i < steps; i++) {
            game::step(&player, world, assets, in, entity::fixed_dt(world));
            entity::step_fixed(world, assets);
        }

        // after the body has moved, before end_frame rebuilds world transforms
        game::place_camera(&player, world);

        entity::end_frame(world, assets, &scratch, dt);

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
