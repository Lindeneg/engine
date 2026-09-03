#include <string.h>

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "core/frametime.h"
#include "core/logger.h"
#include "core/memory.h"
#include "editor/dashboard.h"
#include "editor/editor.h"
#include "game/player.h"
#include "gfx/canvas.h"
#include "gfx/debugdraw.h"
#include "gfx/font.h"
#include "gfx/render.h"
#include "gfx/shader.h"
#include "platform/input.h"
#include "platform/path.h"
#include "platform/window.h"
#include "project/project.h"
#include "scene/entity.h"
#include "ui/ui.h"

// The engine's own resources -- shaders, the UI font, its settings, the
// project template -- baked in by CMake for development; a shipped build finds
// them beside the binary. Nothing about any game lives there: a game is a
// project, given as the argument or chosen on the dashboard.
#ifndef CL_ENGINE_ROOT
#error "CL_ENGINE_ROOT must be defined by the build; see CMakeLists.txt"
#endif

#define ENGINE_CFG "engine.cfg"
#define INPUT_CFG "input.cfg"

// address space only, free to reserve; every arena is carved from it, sized by engine.cfg
#define RESERVE_BYTES GiB(4)

// holds engine.cfg, which sizes every other arena, and a few paths
#define BOOT_ARENA_BYTES MiB(1)

#define DEFAULT_PERM_MIB 64
#define DEFAULT_SCRATCH_MIB 64
#define DEFAULT_MAX_MAPS 8
#define DEFAULT_MAX_ACTIONS 128
#define DEFAULT_RECENT_PROJECTS 8
#define DEFAULT_PLAYER_BODY "player"
#define DEFAULT_BOUNDS_COLOR linalg::Vec4{1.0f, 0.85f, 0.1f, 1.0f}
#define DEFAULT_COLLIDER_COLOR linalg::Vec4{0.3f, 0.9f, 0.9f, 1.0f}

#define DEFAULT_OVERLAY_PADDING 8.0f
#define DEFAULT_OVERLAY_PANEL_W 340.0f
#define DEFAULT_OVERLAY_PANEL_H 330.0f
#define DEFAULT_OVERLAY_SMOOTHING 0.05f
#define DEFAULT_OVERLAY_WINDOW 1.0
#define DEFAULT_SENSITIVITY_MIN 0.01f
#define DEFAULT_SENSITIVITY_MAX 0.5f
#define OVERLAY_TEXT_CAP 128

// the debug panel toggle_debug_draw opens; the `overlay` block of engine.cfg
struct Overlay {
    f32 padding;
    linalg::Vec2 panel; // width and height
    f32 smoothing;      // per-frame blend toward the new frame time
    f64 window;         // seconds, for the frame rate and the worst frame
    f32 sensitivity_min;
    f32 sensitivity_max;
};

static Overlay overlay_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    config::Node *panel = config::find(body, "panel");
    config::Node *range = config::find(body, "sensitivity_range");
    return Overlay{
        .padding = config::get_f32(body, "padding", DEFAULT_OVERLAY_PADDING),
        .panel = linalg::Vec2{config::value_f32(panel, 0, DEFAULT_OVERLAY_PANEL_W),
                              config::value_f32(panel, 1, DEFAULT_OVERLAY_PANEL_H)},
        .smoothing = config::get_f32(body, "smoothing", DEFAULT_OVERLAY_SMOOTHING),
        .window = (f64)config::get_f32(body, "window", (f32)DEFAULT_OVERLAY_WINDOW),
        .sensitivity_min = config::value_f32(range, 0, DEFAULT_SENSITIVITY_MIN),
        .sensitivity_max = config::value_f32(range, 1, DEFAULT_SENSITIVITY_MAX),
    };
}

// what the toggle shows; the panel edits these
struct DebugView {
    b32 bounds;
    b32 body_sphere;
    b32 colliders;
};

static void timing_label(const char *fmt, f64 value) {
    char line[OVERLAY_TEXT_CAP];
    snprintf(line, sizeof(line), fmt, value);
    ui::label(line);
}

static void debug_panel(const Overlay *o, const frametime::Stats *t, u32 steps, DebugView *view,
                        game::Player *player) {
    ui::begin_panel("Debug", canvas::Rect{o->padding, o->padding, o->panel.x, o->panel.y});

    if (ui::tree_node("Timing", true)) {
        timing_label("%.0f fps", (f64)t->fps);
        timing_label("frame %.2f ms", (f64)t->smoothed_dt * 1000.0);
        timing_label("worst %.2f ms", (f64)t->worst_dt * 1000.0);
        timing_label("fixed steps %.0f", (f64)steps);
        ui::tree_pop();
    }

    if (ui::tree_node("Lines", false)) {
        timing_label("this frame %.0f", (f64)debugdraw::line_count());
        timing_label("kept %.0f", (f64)debugdraw::retained_count());
        ui::tree_pop();
    }

    ui::separator();
    ui::checkbox("Bounds", &view->bounds);
    ui::checkbox("Body sphere", &view->body_sphere);
    ui::checkbox("Colliders", &view->colliders);

    ui::columns(2);
    ui::label("Look sensitivity");
    ui::slider("look_sensitivity", &player->look_sensitivity, o->sensitivity_min,
               o->sensitivity_max);
    ui::columns(1);

    ui::end_panel();
}

static entity::Handle find_root(entity::World *w, const char *name) {
    for (entity::Handle h = entity::first(w); h.v; h = entity::next(w, h)) {
        entity::Entity *e = entity::get(w, h);
        if (e && !e->parent.v && strcmp(e->name, name) == 0)
            return h;
    }
    return entity::NONE;
}

// The player is game code driving an entity the level placed. The engine knows
// nothing about it beyond the handle it is given, and the handle is looked up
// each time play starts, since play runs on a fresh copy of the level.
static game::Player make_player(config::Node *player_cfg, input::State *in, entity::World *w,
                                const char *body_name) {
    entity::Handle body = find_root(w, body_name);
    if (!body.v)
        LOG_WARN("main: the level has no root entity named \"%s\", so nothing is driven",
                 body_name);
    return game::create(player_cfg, in, body);
}

// Frames until a project is chosen: the dashboard alone over a cleared screen.
// No world exists yet, so nothing else draws. False when the window closed or
// Quit was chosen.
static b32 choose_project(dashboard::Dashboard *dash, window::Window *win, input::State *in,
                          memory::Arena *perm, memory::Arena *scratch, linalg::Vec4 clear,
                          char *out_dir, u32 cap) {
    while (!window::should_close(win)) {
        window::poll(win);
        input::update(in, win);

        i32 w = 0;
        i32 h = 0;
        window::framebuffer_size(win, &w, &h);
        render::resize(w, h);

        canvas::begin_frame(w, h);
        ui::Input ui_in = ui::input_from(in);
        ui::begin_frame(&ui_in);
        dashboard::Result r = dashboard::frame(dash, perm, scratch, w, h, out_dir, cap);
        ui::end_frame();

        shader::FrameData frame{};
        render::begin_frame(&frame, clear);
        canvas::flush();
        window::swap(win);

        if (r == dashboard::RESULT_OPEN)
            return true;
        if (r == dashboard::RESULT_QUIT)
            return false;
    }
    return false;
}

int main(int argc, char **argv) {
    logger::init(NULL);
    memory::init(RESERVE_BYTES);

    const char *engine_root = CL_ENGINE_ROOT;

    memory::Arena boot = memory::create(BOOT_ARENA_BYTES);
    config::Doc *engine = MUST(config::load(&boot, FPATH(&boot, engine_root, ENGINE_CFG)));
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

    // the engine's maps now; the project's join them once it is known
    config::Node *input_block = config::find(cfg, "input");
    config::Node *input_body = input_block ? input_block->first_child : NULL;
    input::State *in{MUST(input::init(
        &perm, &scratch, FPATH(&boot, engine_root, INPUT_CFG),
        (u32)config::get_i32(input_body, "max_maps", DEFAULT_MAX_MAPS),
        (u32)config::get_i32(input_body, "max_actions", DEFAULT_MAX_ACTIONS)))};

    i32 fb_w = 0;
    i32 fb_h = 0;
    window::framebuffer_size(win, &fb_w, &fb_h);

    // shaders before assets: materials get their program at load
    MUST(shader::init(&perm, &scratch, engine_root));

    render::Config render_cfg = render::config_from(config::find(cfg, "render"));
    render::init(&perm, render_cfg, fb_w, fb_h);

    config::Node *debug_cfg = config::find(cfg, "debug_draw");
    debugdraw::init(&perm, debugdraw::config_from(debug_cfg));
    config::Node *debug_body = debug_cfg ? debug_cfg->first_child : NULL;
    linalg::Vec4 bounds_color =
        config::value_vec4(config::find(debug_body, "bounds_color"), DEFAULT_BOUNDS_COLOR);
    linalg::Vec4 collider_color =
        config::value_vec4(config::find(debug_body, "collider_color"), DEFAULT_COLLIDER_COLOR);

    canvas::init(&perm, canvas::config_from(config::find(cfg, "canvas")));
    font::Font *ui_font{MUST(
        font::load(&perm, &scratch, engine_root, font::config_from(config::find(cfg, "font"))))};
    ui::init(&perm, ui::config_from(config::find(cfg, "ui")),
             MUST(ui::load_style(&scratch, engine_root)), ui_font);

    editor::Config editor_cfg = MUST(editor::load_config(&perm, &scratch, engine_root));
    u32 recent_cap = (u32)config::get_i32(cfg, "recent_projects", DEFAULT_RECENT_PROJECTS);

    // the project: the argument, or the dashboard
    char project_dir[PROJECT_PATH_CAP];
    if (argc > 1) {
        snprintf(project_dir, sizeof(project_dir), "%s", argv[1]);
    } else {
        dashboard::Dashboard *dash =
            dashboard::init(&perm, engine_root,
                            dashboard::Config{.width = editor_cfg.dashboard_width,
                                              .height = editor_cfg.dashboard_height,
                                              .padding = editor_cfg.dashboard_padding,
                                              .recent_cap = recent_cap});
        if (!choose_project(dash, win, in, &perm, &scratch, render_cfg.clear_color, project_dir,
                            sizeof(project_dir))) {
            ui::shutdown();
            font::destroy(ui_font);
            canvas::shutdown();
            debugdraw::shutdown();
            render::shutdown();
            shader::shutdown();
            window::destroy(win);
            logger::shutdown();
            return EXIT_SUCCESS;
        }
    }

    project::Project proj;
    if (project::open(&perm, project_dir, &proj) != result::OK) {
        LOG_ERR("main: %s is not a project that can be opened", project_dir);
        return EXIT_FAILURE;
    }
    project::remember(&scratch, engine_root, proj.dir, recent_cap);
    window::set_title(win, proj.title);

    if (input::add_maps(in, &perm, &scratch, FPATH(&boot, proj.dir, PROJECT_INPUT_CFG)) !=
        result::OK)
        return EXIT_FAILURE;

    asset::Manager *assets{MUST(asset::init(
        &perm, &scratch,
        asset::config_from(config::find(cfg, "assets"), proj.dir,
                           shader::role_handle(shader::ROLE_DEFAULT))))};

    render::attach_assets(&perm, assets);

    entity::World *world{MUST(
        entity::init(&perm, entity::config_from(config::find(proj.doc->first, "world"))))};

    // everything in the world comes from the level file
    MUST(entity::load_level(world, assets, proj.level));

    config::Node *player_cfg = config::find(proj.doc->first, "player");
    const char *body_name =
        config::get(player_cfg ? player_cfg->first_child : NULL, "body", DEFAULT_PLAYER_BODY);

    // built when play starts, from the copy of the level play runs on
    game::Player player{};

    Overlay overlay = overlay_from(config::find(cfg, "overlay"));
    DebugView view{.bounds = true, .body_sphere = true, .colliders = true};

    editor::Editor *ed = editor::init(&perm, &scratch, editor_cfg, in, &proj);

    input::Map gameplay = input::find_map(in, "gameplay");
    input::Action quit = input::find(in, "quit");
    input::Action toggle_look = input::find(in, "toggle_mouse_look");
    input::Action toggle_debug = input::find(in, "toggle_debug_draw");
    input::Action toggle_editor = input::find(in, "toggle_editor");
    b32 debug_draw = false;

    // The session opens in edit mode: the level file is the authority, and play
    // is a copy of it taken when F1 says so.
    editor::set_active(ed, world, assets, in, win, true);

    f64 last = window::time();
    frametime::Stats timing = frametime::create(overlay.smoothing, overlay.window, last);

    while (!window::should_close(win)) {
        window::poll(win);
        input::update(in, win);

        if (input::pressed(in, quit))
            break;

        if (input::pressed(in, toggle_editor)) {
            b32 edit = !editor::active(ed);
            editor::set_active(ed, world, assets, in, win, edit);

            // play starts from a fresh copy of the level, so the player starts fresh too
            if (!edit)
                player = make_player(player_cfg, in, world, body_name);
        }

        // while editing the editor owns the cursor and the maps
        if (input::pressed(in, toggle_look) && !editor::active(ed)) {
            b32 look = !input::mouse_look(in);
            input::set_mouse_look(in, win, look);

            // With the cursor loose, gameplay stops receiving anything. This is
            // the same seam an editor panel will use when it takes focus.
            input::set_map_enabled(in, gameplay, look);
        }

        if (input::pressed(in, toggle_debug))
            debug_draw = !debug_draw;

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

        frametime::tick(&timing, now, dt);

        // before anything draws: the fixed steps below may trace into it
        debugdraw::begin_frame(dt);
        canvas::begin_frame(fb_w, fb_h);

        ui::Input ui_in = ui::input_from(in);
        ui::begin_frame(&ui_in);

        // Editing holds the level still: no fixed steps are banked or run, so
        // play resumes without a burst of catch-up. The world still draws.
        b32 editing = editor::active(ed);
        u32 steps = 0;
        if (!editing) {
            // aim first, so the fixed steps below move in the direction being looked
            game::aim(&player, in);

            steps = entity::begin_frame(world, dt);
            for (u32 i = 0; i < steps; i++) {
                game::step(&player, world, assets, in, entity::fixed_dt(world));
                entity::step_fixed(world, assets);
            }

            // after the body has moved, before end_frame rebuilds world transforms
            game::place_camera(&player, world);
        }

        entity::end_frame(world, assets, &scratch, dt);

        if (editing)
            editor::frame(ed, world, assets, in, win, dt, fb_w, fb_h);

        // after end_frame, so the boxes sit on this frame's world transforms
        if (debug_draw) {
            if (view.bounds)
                entity::draw_bounds(world, assets, bounds_color);
            if (view.body_sphere)
                game::draw_debug(&player, world);
            if (view.colliders && !editing) // the editor draws them itself
                entity::draw_colliders(world, collider_color);
            debug_panel(&overlay, &timing, steps, &view, &player);
        }
        ui::end_frame();

        shader::FrameData frame{};
        f32 aspect = (f32)fb_w / (f32)fb_h;
        b32 has_view = editing ? editor::build_frame(ed, world, aspect, &frame)
                               : entity::build_frame(world, aspect, &frame);
        if (!has_view)
            LOG_WARN_ONCE("main: the level has no active camera, nothing will be visible");

        render::begin_frame(&frame, render_cfg.clear_color);
        entity::submit(world, assets);
        render::flush(assets);
        debugdraw::flush();
        canvas::flush();

        window::swap(win);
    }

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_PERSISTENT), "assets-persistent");
    memory::log_stats(asset::scope_arena(assets, asset::SCOPE_TRANSIENT), "assets-transient");
#endif

    ui::shutdown();
    font::destroy(ui_font);
    canvas::shutdown();
    debugdraw::shutdown();
    render::shutdown();
    shader::shutdown();
    window::destroy(win);
    logger::shutdown();
    return EXIT_SUCCESS;
}
