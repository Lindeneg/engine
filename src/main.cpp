#include "asset.h"
#include "common.h"
#include "logger.h"
#include "memory.h"
#include "render.h"
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

    asset::ModelHandle soldier{
        OR_ELSE(asset::load_model(assets, asset::SCOPE_PERSISTENT, "characters/soldier.glb"),
                asset::MODEL_FALLBACK)};
    asset::Model *s = asset::get(assets, soldier);

    asset::ClipHandle idle{
        OR_ELSE(asset::load_clip(assets, asset::SCOPE_PERSISTENT, "animations/pistol_idle.glb",
                                 s->skeleton),
                asset::CLIP_FALLBACK)};
    (void)idle;

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

        render::begin_frame(linalg::Vec4{0.10f, 0.11f, 0.13f, 1.0f});

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
