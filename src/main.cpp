#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "core/frametime.h"
#include "core/logger.h"
#include "core/memory.h"
#include "editor/dashboard.h"
#include "editor/editor.h"
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
#include "script/script.h"
#include "ui/ui.h"

// The engine's own resources -- shaders, the UI font, its settings, the
// project template -- baked in by CMake for development; a shipped build finds
// them beside the binary. Nothing about any game lives there: a game is a
// project, given as the argument or chosen on the dashboard.
//
// The run is the engine, made once, and a session: one open project, held in
// an arena of its own that closing resets. The dashboard shows while there is
// no session; the editor asks for a reopen or a close through its Project
// section, and a reopen is how a changed script declaration takes.
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
#define DEFAULT_PROJECT_MIB 64
#define DEFAULT_MAX_MAPS 8
#define DEFAULT_MAX_ACTIONS 128
#define DEFAULT_RECENT_PROJECTS 8
#define DEFAULT_BOUNDS_COLOR linalg::Vec4{1.0f, 0.85f, 0.1f, 1.0f}
#define DEFAULT_COLLIDER_COLOR linalg::Vec4{0.3f, 0.9f, 0.9f, 1.0f}

#define DEFAULT_OVERLAY_PADDING 8.0f
#define DEFAULT_OVERLAY_PANEL_W 340.0f
#define DEFAULT_OVERLAY_PANEL_H 330.0f
#define DEFAULT_OVERLAY_SMOOTHING 0.05f
#define DEFAULT_OVERLAY_WINDOW 1.0
#define OVERLAY_TEXT_CAP 128

// the debug panel toggle_debug_draw opens; the `overlay` block of engine.cfg
struct Overlay {
    f32 padding;
    linalg::Vec2 panel; // width and height
    f32 smoothing;      // per-frame blend toward the new frame time
    f64 window;         // seconds, for the frame rate and the worst frame
};

static Overlay overlay_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    config::Node *panel = config::find(body, "panel");
    return Overlay{
        .padding = config::get_f32(body, "padding", DEFAULT_OVERLAY_PADDING),
        .panel = linalg::Vec2{config::value_f32(panel, 0, DEFAULT_OVERLAY_PANEL_W),
                              config::value_f32(panel, 1, DEFAULT_OVERLAY_PANEL_H)},
        .smoothing = config::get_f32(body, "smoothing", DEFAULT_OVERLAY_SMOOTHING),
        .window = (f64)config::get_f32(body, "window", (f32)DEFAULT_OVERLAY_WINDOW),
    };
}

// what the toggle shows; the panel edits these
struct DebugView {
    b32 bounds;
    b32 colliders;
};

static void timing_label(const char *fmt, f64 value) {
    char line[OVERLAY_TEXT_CAP];
    snprintf(line, sizeof(line), fmt, value);
    ui::label(line);
}

// a message's first line: for a traceback the file, the line and the error
static void first_line(const char *text, char *out, u64 cap) {
    u64 n = 0;
    while (text[n] && text[n] != '\n' && n + 1 < cap) {
        out[n] = text[n];
        n++;
    }
    out[n] = 0;
}

static void debug_panel(const Overlay *o, const frametime::Stats *t, u32 steps, DebugView *view,
                        const script::State *scripts) {
    ui::begin_panel("Debug", canvas::Rect{o->padding, o->padding, o->panel.x, o->panel.y});

    if (ui::tree_node("Timing", true)) {
        timing_label("%.0f fps", (f64)t->fps);
        timing_label("frame %.2f ms", (f64)t->smoothed_dt * 1000.0);
        timing_label("worst %.2f ms", (f64)t->worst_dt * 1000.0);
        timing_label("fixed steps %.0f", (f64)steps);
        timing_label("scripts %.0f KiB", (f64)script::bytes(scripts) / 1024.0);
        ui::tree_pop();
    }

    // what the scripts last failed with, while the editor is out of sight
    const char *error = script::last_error(scripts);
    if (error[0]) {
        char line[OVERLAY_TEXT_CAP];
        first_line(error, line, sizeof(line));
        ui::label_dim(line);
    }

    if (ui::tree_node("Lines", false)) {
        timing_label("this frame %.0f", (f64)debugdraw::line_count());
        timing_label("kept %.0f", (f64)debugdraw::retained_count());
        ui::tree_pop();
    }

    ui::separator();
    ui::checkbox("Bounds", &view->bounds);
    ui::checkbox("Colliders", &view->colliders);

    ui::end_panel();
}

// what is made once and serves every project of the run
struct Engine {
    const char *root;
    const char *window_title; // the window's own, back when no project names it
    memory::Arena perm;
    memory::Arena scratch;
    memory::Arena project;  // one open project's everything; reset on close
    memory::Arena snapshot; // the editor's, for the level as text while it plays

    window::Window *win;
    input::State *in;
    asset::Manager *assets;
    dashboard::Dashboard *dash;
    editor::Config editor_cfg;
    u32 recent_cap;

    input::Action quit;
    input::Action toggle_look;
    input::Action toggle_debug;
    input::Action toggle_editor;
    input::Action reload_scripts;
};

// one open project, everything of it in Engine.project
struct Session {
    b32 open;
    project::Project proj;
    input::Mark input_mark; // the tables before the project's maps joined
    entity::World *world;
    script::State *scripts;
    editor::Editor *ed;
    input::Map play_map;
};

static void close_session(Engine *e, Session *s) {
    if (!s->open)
        return;

    script::stop(s->scripts);
    input::set_mouse_look(e->in, e->win, false);
    input::rewind(e->in, s->input_mark);
    window::set_title(e->win, e->window_title);
    LOG_INFO("main: closed %s", s->proj.dir);

    memory::reset(&e->project);
    *s = Session{};
}

// The folder as a session, in edit mode on its start level. False with the
// engine as it was: a project that will not open leaves nothing behind.
static b32 open_session(Engine *e, Session *s, const char *dir) {
    close_session(e, s);
    memory::Arena *a = &e->project;

    if (project::open(a, dir, &s->proj) != result::OK) {
        LOG_ERR("main: %s is not a project that can be opened", dir);
        return false;
    }
    project::remember(&e->scratch, e->root, s->proj.dir, e->recent_cap);

    s->input_mark = input::mark(e->in);
    if (input::add_maps(e->in, a, &e->scratch, FPATH(a, s->proj.dir, PROJECT_INPUT_CFG)) !=
        result::OK) {
        input::rewind(e->in, s->input_mark);
        return false;
    }

    if (asset::reopen(e->assets, s->proj.dir) != result::OK) {
        input::rewind(e->in, s->input_mark);
        return false;
    }

    entity::WorldResult world =
        entity::init(a, entity::config_from(config::find(s->proj.doc->first, "world")));
    if (world.err != result::OK) {
        input::rewind(e->in, s->input_mark);
        return false;
    }
    s->world = world.val;

    // the project's declarations, before the level fills the world with them
    script::StateResult scripts = script::init(a, &e->scratch, s->world, e->assets, e->in, &s->proj);
    if (scripts.err != result::OK) {
        input::rewind(e->in, s->input_mark);
        return false;
    }
    s->scripts = scripts.val;
    script::declare(s->scripts); // its own error is logged; the project opens regardless

    // everything in the world comes from the level file; one that is missing
    // is an empty world the editor can fill and save
    if (entity::load_level(s->world, e->assets, s->proj.level) != result::OK)
        LOG_ERR("main: level '%s' could not be read; the world starts empty", s->proj.level);

    s->ed = editor::init(a, &e->scratch, &e->snapshot, e->editor_cfg, e->in, &s->proj);
    editor::set_script_error(s->ed, script::last_error(s->scripts));
    s->play_map = input::find_map(e->in, e->editor_cfg.play_map);

    // The session opens in edit mode: the level file is the authority, and play
    // is a copy of it taken when F1 says so.
    editor::set_active(s->ed, s->world, e->assets, e->in, e->win, true);
    window::set_title(e->win, s->proj.title);

    s->open = true;
    return true;
}

// The dashboard alone over a cleared screen: no world exists, so nothing
// else draws. Answers what was chosen.
static dashboard::Result dashboard_frame(Engine *e, linalg::Vec4 clear, char *out_dir, u32 cap) {
    i32 w = 0;
    i32 h = 0;
    window::framebuffer_size(e->win, &w, &h);
    render::resize(w, h);

    canvas::begin_frame(w, h);
    ui::Input ui_in = ui::input_from(e->in);
    ui::begin_frame(&ui_in);
    dashboard::Result r = dashboard::frame(e->dash, &e->project, &e->scratch, w, h, out_dir, cap);
    ui::end_frame();

    shader::FrameData frame{};
    render::begin_frame(&frame, clear);
    canvas::flush();
    window::swap(e->win);
    return r;
}

int main(int argc, char **argv) {
    logger::init(NULL);
    memory::init(RESERVE_BYTES);

    Engine e{};
    e.root = CL_ENGINE_ROOT;

    memory::Arena boot = memory::create(BOOT_ARENA_BYTES);
    config::Doc *engine = MUST(config::load(&boot, FPATH(&boot, e.root, ENGINE_CFG)));
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
    e.perm = memory::create(MiB(config::get_i32(mem_body, "perm", DEFAULT_PERM_MIB)));
    e.scratch = memory::create(MiB(config::get_i32(mem_body, "scratch", DEFAULT_SCRATCH_MIB)));
    e.project = memory::create(MiB(config::get_i32(mem_body, "project", DEFAULT_PROJECT_MIB)));

    window::Config window_cfg = window::config_from(config::find(cfg, "window"));
    e.window_title = window_cfg.title;
    e.win = MUST(window::create(&e.perm, window_cfg));

    // the engine's maps now; a project's join them while it is open
    config::Node *input_block = config::find(cfg, "input");
    config::Node *input_body = input_block ? input_block->first_child : NULL;
    e.in = MUST(input::init(&e.perm, &e.scratch, FPATH(&boot, e.root, INPUT_CFG),
                            (u32)config::get_i32(input_body, "max_maps", DEFAULT_MAX_MAPS),
                            (u32)config::get_i32(input_body, "max_actions", DEFAULT_MAX_ACTIONS)));
    e.quit = input::find(e.in, "quit");
    e.toggle_look = input::find(e.in, "toggle_mouse_look");
    e.toggle_debug = input::find(e.in, "toggle_debug_draw");
    e.toggle_editor = input::find(e.in, "toggle_editor");
    e.reload_scripts = input::find(e.in, "reload_scripts");

    i32 fb_w = 0;
    i32 fb_h = 0;
    window::framebuffer_size(e.win, &fb_w, &fb_h);

    // shaders before assets: materials get their program at load
    MUST(shader::init(&e.perm, &e.scratch, e.root));

    render::Config render_cfg = render::config_from(config::find(cfg, "render"));
    render::init(&e.perm, render_cfg, fb_w, fb_h);

    config::Node *debug_cfg = config::find(cfg, "debug_draw");
    debugdraw::init(&e.perm, debugdraw::config_from(debug_cfg));
    config::Node *debug_body = debug_cfg ? debug_cfg->first_child : NULL;
    linalg::Vec4 bounds_color =
        config::value_vec4(config::find(debug_body, "bounds_color"), DEFAULT_BOUNDS_COLOR);
    linalg::Vec4 collider_color =
        config::value_vec4(config::find(debug_body, "collider_color"), DEFAULT_COLLIDER_COLOR);

    canvas::init(&e.perm, canvas::config_from(config::find(cfg, "canvas")));
    font::Font *ui_font{MUST(
        font::load(&e.perm, &e.scratch, e.root, font::config_from(config::find(cfg, "font"))))};
    ui::init(&e.perm, ui::config_from(config::find(cfg, "ui")),
             MUST(ui::load_style(&e.scratch, e.root)), ui_font);

    e.editor_cfg = MUST(editor::load_config(&e.perm, &e.scratch, e.root));
    e.snapshot = memory::create(e.editor_cfg.snapshot_bytes);
    e.recent_cap = (u32)config::get_i32(cfg, "recent_projects", DEFAULT_RECENT_PROJECTS);
    e.dash = dashboard::init(&e.perm, e.root,
                             dashboard::Config{.width = e.editor_cfg.dashboard_width,
                                               .height = e.editor_cfg.dashboard_height,
                                               .padding = e.editor_cfg.dashboard_padding,
                                               .recent_cap = e.recent_cap});

    // The asset manager serves the run; its root moves with the session. It
    // starts on the resources folder, which holds nothing it will be asked for.
    e.assets = MUST(asset::init(&e.perm, &e.scratch,
                                asset::config_from(config::find(cfg, "assets"), e.root,
                                                   shader::role_handle(shader::ROLE_DEFAULT))));
    render::attach_assets(&e.perm, e.assets);

    Overlay overlay = overlay_from(config::find(cfg, "overlay"));
    DebugView view{.bounds = true, .colliders = true};
    b32 debug_draw = false;

    // the project: the argument, else the dashboard; an argument that will
    // not open falls back to the dashboard too
    Session s{};
    char project_dir[PROJECT_PATH_CAP];
    if (argc > 1 && !open_session(&e, &s, argv[1]))
        LOG_ERR("main: %s was given and could not be opened", argv[1]);

    f64 last = window::time();
    frametime::Stats timing = frametime::create(overlay.smoothing, overlay.window, last);

    while (!window::should_close(e.win)) {
        window::poll(e.win);
        input::update(e.in, e.win);

        if (input::pressed(e.in, e.quit))
            break;

        if (!s.open) {
            dashboard::Result r =
                dashboard_frame(&e, render_cfg.clear_color, project_dir, sizeof(project_dir));
            if (r == dashboard::RESULT_QUIT)
                break;
            if (r == dashboard::RESULT_OPEN && !open_session(&e, &s, project_dir))
                dashboard::refresh(e.dash, &e.project);
            last = window::time(); // the frame after opening is not a hitch
            continue;
        }

        if (input::pressed(e.in, e.toggle_editor)) {
            b32 edit = !editor::active(s.ed);

            // scripts stop before the world is put back, and start once the copy is up
            if (edit) {
                script::stop(s.scripts);
                editor::set_active(s.ed, s.world, e.assets, e.in, e.win, true);
            } else {
                editor::set_active(s.ed, s.world, e.assets, e.in, e.win, false);
                if (script::start(s.scripts) != result::OK)
                    editor::set_active(s.ed, s.world, e.assets, e.in, e.win, true);
            }
        }

        // the scripts again, on the level as it stands
        if (input::pressed(e.in, e.reload_scripts) && script::running(s.scripts) &&
            script::reload(s.scripts) != result::OK) {
            script::stop(s.scripts);
            editor::set_active(s.ed, s.world, e.assets, e.in, e.win, true);
        }

        // while editing the editor owns the cursor and the maps
        if (input::pressed(e.in, e.toggle_look) && !editor::active(s.ed)) {
            b32 look = !input::mouse_look(e.in);
            input::set_mouse_look(e.in, e.win, look);

            // With the cursor loose, gameplay stops receiving anything. This is
            // the same seam an editor panel will use when it takes focus.
            input::set_map_enabled(e.in, s.play_map, look);
        }

        if (input::pressed(e.in, e.toggle_debug))
            debug_draw = !debug_draw;

        i32 w = 0;
        i32 h = 0;
        window::framebuffer_size(e.win, &w, &h);
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

        ui::Input ui_in = ui::input_from(e.in);
        ui::begin_frame(&ui_in);

        // Editing holds the level still: no fixed steps are banked or run, so
        // play resumes without a burst of catch-up. The world still draws.
        b32 editing = editor::active(s.ed);
        u32 steps = 0;
        if (!editing) {
            // the frame's input first, so the fixed steps move by it
            script::update(s.scripts, dt);

            steps = entity::begin_frame(s.world, dt);
            for (u32 i = 0; i < steps; i++) {
                script::fixed_update(s.scripts, entity::fixed_dt(s.world));
                entity::step_fixed(s.world, e.assets);
            }

            // after the steps have moved things, before end_frame rebuilds world transforms
            script::late_update(s.scripts, dt);
        }

        entity::end_frame(s.world, e.assets, &e.scratch, dt);

        if (editing)
            editor::frame(s.ed, s.world, e.assets, e.in, e.win, dt, fb_w, fb_h);

        // after end_frame, so the boxes sit on this frame's world transforms
        if (debug_draw) {
            if (view.bounds)
                entity::draw_bounds(s.world, e.assets, bounds_color);
            if (view.colliders && !editing) // the editor draws them itself
                entity::draw_colliders(s.world, collider_color);
            debug_panel(&overlay, &timing, steps, &view, s.scripts);
        }
        ui::end_frame();

        shader::FrameData frame{};
        f32 aspect = (f32)fb_w / (f32)fb_h;
        b32 has_view = editing ? editor::build_frame(s.ed, s.world, aspect, &frame)
                               : entity::build_frame(s.world, aspect, &frame);
        if (!has_view)
            LOG_WARN_ONCE("main: the level has no active camera, nothing will be visible");

        render::begin_frame(&frame, render_cfg.clear_color);
        entity::submit(s.world, e.assets);
        render::flush(e.assets);
        debugdraw::flush();
        canvas::flush();

        window::swap(e.win);

        // what the Project section asked, once the frame is done with the world
        editor::Request request = editor::take_request(s.ed);
        if (request == editor::REQUEST_REOPEN) {
            snprintf(project_dir, sizeof(project_dir), "%s", s.proj.dir);
            if (!open_session(&e, &s, project_dir))
                dashboard::refresh(e.dash, &e.project);
        } else if (request == editor::REQUEST_CLOSE) {
            close_session(&e, &s);
            dashboard::refresh(e.dash, &e.project);
        }
    }

    close_session(&e, &s);

#if CL_MEM_STATS
    memory::log_stats(&e.perm, "perm");
    memory::log_stats(&e.scratch, "scratch");
    memory::log_stats(&e.project, "project");
    memory::log_stats(asset::scope_arena(e.assets, asset::SCOPE_PERSISTENT), "assets-persistent");
    memory::log_stats(asset::scope_arena(e.assets, asset::SCOPE_TRANSIENT), "assets-transient");
#endif

    ui::shutdown();
    font::destroy(ui_font);
    canvas::shutdown();
    debugdraw::shutdown();
    render::shutdown();
    shader::shutdown();
    window::destroy(e.win);
    logger::shutdown();
    return EXIT_SUCCESS;
}
