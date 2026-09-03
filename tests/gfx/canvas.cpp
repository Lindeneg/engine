#include "gfx/canvas.h"

#include "suites.h"

static const linalg::Vec4 WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
static const canvas::Rect BOX = {10.0f, 10.0f, 100.0f, 50.0f};

static void setup(memory::Arena *a, u32 quads, u32 batches) {
    canvas::Config cfg = canvas::config_from(NULL);
    cfg.max_quads = quads;
    cfg.max_batches = batches;
    canvas::init(a, cfg);
    canvas::begin_frame(800, 600);
}

static font::Font *shared_font(memory::Arena *scratch) {
    static font::Font *f = NULL;
    if (f)
        return f;

    static memory::Arena arena;
    arena = memory::create(MiB(1));
    font::Config cfg = font::config_from(NULL);
    cfg.file = "fonts/arial.ttf";
    cfg.atlas = 256;
    cfg.oversample = 1;
    f = MUST(font::load(&arena, scratch, CL_ASSET_ROOT, cfg));
    return f;
}

static void fills_share_a_batch(memory::Arena *a) {
    setup(a, 64, 8);

    canvas::rect(BOX, WHITE);
    canvas::rect(BOX, WHITE);
    CHECK(canvas::quad_count() == 2);
    CHECK(canvas::batch_count() == 1);

    canvas::rect_outline(BOX, 2.0f, WHITE);
    CHECK(canvas::quad_count() == 6);
    CHECK(canvas::batch_count() == 1);

    canvas::shutdown();
}

static void textures_and_clips_break_batches(memory::Arena *a) {
    setup(a, 64, 8);

    texture::Texture t{.gl = 1, .width = 1, .height = 1, .format = texture::FORMAT_RGBA8};

    canvas::rect(BOX, WHITE);
    canvas::image(&t, BOX, WHITE);
    canvas::image(&t, BOX, WHITE);
    CHECK(canvas::batch_count() == 2);

    canvas::push_clip(canvas::Rect{0.0f, 0.0f, 400.0f, 300.0f});
    canvas::image(&t, BOX, WHITE);
    CHECK(canvas::batch_count() == 3);
    canvas::pop_clip();

    // back to the outer clip, and the previous batch is no longer the last one
    canvas::image(&t, BOX, WHITE);
    CHECK(canvas::batch_count() == 4);
    CHECK(canvas::quad_count() == 5);

    canvas::shutdown();
}

static void empty_clips_drop_quads(memory::Arena *a) {
    setup(a, 64, 8);

    canvas::push_clip(canvas::Rect{0.0f, 0.0f, 100.0f, 100.0f});
    canvas::push_clip(canvas::Rect{200.0f, 200.0f, 100.0f, 100.0f}); // disjoint
    canvas::rect(BOX, WHITE);
    CHECK(canvas::quad_count() == 0);
    canvas::pop_clip();

    canvas::rect(BOX, WHITE);
    CHECK(canvas::quad_count() == 1);
    canvas::pop_clip();

    canvas::shutdown();
}

static void text_quads_join_fills(memory::Arena *a) {
    setup(a, 64, 8);
    font::Font *f = shared_font(a);

    // a fill first goes to the white texture; the glyphs are a second batch
    canvas::rect(BOX, WHITE);
    f32 advance = canvas::text(f, 0.0f, 20.0f, "ab c", WHITE);
    CHECK(advance == font::width(f, "ab c"));
    CHECK(canvas::quad_count() == 4); // the space has no quad
    CHECK(canvas::batch_count() == 2);

    // a fill after text joins the glyph batch through the atlas's white rows
    canvas::rect(BOX, WHITE);
    CHECK(canvas::quad_count() == 5);
    CHECK(canvas::batch_count() == 2);

    canvas::shutdown();
}

static void overflow_drops_and_warns_once(memory::Arena *a) {
    setup(a, 2, 1);

    test::mute();
    for (u32 i = 0; i < 4; i++)
        canvas::rect(BOX, WHITE);
    CHECK(canvas::quad_count() == 2);
    CHECK(test::unmute() == 1);
    canvas::shutdown();

    setup(a, 64, 1);
    texture::Texture t{.gl = 1, .width = 1, .height = 1, .format = texture::FORMAT_RGBA8};

    test::mute();
    canvas::rect(BOX, WHITE);
    canvas::image(&t, BOX, WHITE);
    canvas::image(&t, BOX, WHITE);
    CHECK(canvas::quad_count() == 1);
    CHECK(canvas::batch_count() == 1);
    CHECK(test::unmute() == 1);

    canvas::shutdown();
}

static void orthographic_maps_pixels_to_clip(memory::Arena *) {
    linalg::Mat4 m = linalg::mat4_orthographic(0.0f, 800.0f, 600.0f, 0.0f, -1.0f, 1.0f);

    linalg::Vec3 tl = linalg::mat4_mul_point(m, linalg::Vec3{0.0f, 0.0f, 0.0f});
    linalg::Vec3 br = linalg::mat4_mul_point(m, linalg::Vec3{800.0f, 600.0f, 0.0f});
    CHECK(tl.x == -1.0f && tl.y == 1.0f);
    CHECK(br.x == 1.0f && br.y == -1.0f);
}

test::Suite canvas_suite() {
    static const test::Case cases[] = {
        CASE(fills_share_a_batch),           CASE(textures_and_clips_break_batches),
        CASE(empty_clips_drop_quads),        CASE(text_quads_join_fills),
        CASE(overflow_drops_and_warns_once), CASE(orthographic_maps_pixels_to_clip),
    };
    return test::Suite{.name = "canvas", .cases = cases, .count = ARRAY_COUNT(cases)};
}
