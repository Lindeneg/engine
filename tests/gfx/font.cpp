#include "gfx/font.h"

#include <string.h>

#include "suites.h"

#define TEST_FONT "fonts/arial.ttf"

static font::Config small(const char *file) {
    font::Config cfg = font::config_from(NULL);
    cfg.file = file;
    cfg.atlas = 256;
    cfg.oversample = 1;
    return cfg;
}

// one font for the suite: the atlas outlives any case's temp scope
static font::Font *shared_font(memory::Arena *scratch) {
    static font::Font *f = NULL;
    if (f)
        return f;

    static memory::Arena arena;
    arena = memory::create(MiB(1));
    f = MUST(font::load(&arena, scratch, CL_ASSET_ROOT, small(TEST_FONT)));
    return f;
}

static void metrics_are_sane(memory::Arena *a) {
    font::Font *f = shared_font(a);

    CHECK(f->ascent > 0.0f && f->descent > 0.0f);
    CHECK(f->line_height >= f->ascent + f->descent);
    CHECK(f->ascent + f->descent <= f->size * 1.5f);

    // the white rows are there, and the first glyph row is not white
    CHECK(f->pixels[0] == 0xFF && f->pixels[f->atlas - 1] == 0xFF);
    CHECK(f->white_u > 0.0f && f->white_u < 1.0f / 64.0f);
}

static void glyphs_have_boxes_and_advances(memory::Arena *a) {
    font::Font *f = shared_font(a);

    const font::Glyph *upper = font::glyph(f, 'A');
    CHECK(upper->x1 > upper->x0 && upper->y1 > upper->y0);
    CHECK(upper->u1 > upper->u0 && upper->v1 > upper->v0);
    CHECK(upper->advance > 0.0f);
    CHECK(upper->y0 < 0.0f); // sits above the baseline

    const font::Glyph *space = font::glyph(f, ' ');
    CHECK(space->advance > 0.0f);
    CHECK(space->x1 <= space->x0 || space->y1 <= space->y0);

    // control characters draw nothing at all
    CHECK(font::glyph(f, '\n') == NULL);
}

static void width_sums_advances(memory::Arena *a) {
    font::Font *f = shared_font(a);

    f32 one = font::glyph(f, 'W')->advance;
    CHECK(font::width(f, "") == 0.0f);
    CHECK(font::width(f, "W") == one);
    CHECK(font::width(f, "WWW") == 3.0f * one);
    CHECK(font::width(f, "W\nW") == 2.0f * one);

    // a prefix, in bytes, stops on a codepoint boundary
    CHECK(font::width(f, "WWW", 2) == 2.0f * one);
    CHECK(font::width(f, "WWW", 0) == 0.0f);
}

static void outside_the_range_draws_the_fallback(memory::Arena *a) {
    font::Font *f = shared_font(a);

    test::mute();
    const font::Glyph *g = font::glyph(f, 0x4E2D);
    u32 warned = test::unmute();

    CHECK(g == font::glyph(f, '?'));
    CHECK(warned == 1);
}

static void bad_requests_are_refused(memory::Arena *a) {
    memory::Arena arena = memory::create(MiB(1));

    test::mute();
    CHECK(font::load(&arena, a, CL_ASSET_ROOT, small(NULL)).err == result::ERR_INVALID);
    CHECK(font::load(&arena, a, CL_ASSET_ROOT, small("fonts/nope.ttf")).err != result::OK);
    CHECK(font::load(&arena, a, CL_ASSET_ROOT, small("engine.cfg")).err == result::ERR_PARSE);

    font::Config tiny = small(TEST_FONT);
    tiny.atlas = 16;
    CHECK(font::load(&arena, a, CL_ASSET_ROOT, tiny).err == result::ERR_EXHAUSTED);
    CHECK(test::unmute() == 4);
}

static void config_reads_the_block(memory::Arena *a) {
    config::Doc *doc = MUST(config::parse(
        a, memory::push_str(a, "font {\n    file fonts/x.ttf\n    size 24\n    count 10\n}\n"),
        "engine.cfg"));

    font::Config cfg = font::config_from(doc->first);
    CHECK(strcmp(cfg.file, "fonts/x.ttf") == 0);
    CHECK(cfg.size == 24.0f);
    CHECK(cfg.count == 10);
    CHECK(cfg.atlas > 0 && cfg.oversample > 0);

    CHECK(font::config_from(NULL).file == NULL);
}

test::Suite font_suite() {
    static const test::Case cases[] = {
        CASE(metrics_are_sane),
        CASE(glyphs_have_boxes_and_advances),
        CASE(width_sums_advances),
        CASE(outside_the_range_draws_the_fallback),
        CASE(bad_requests_are_refused),
        CASE(config_reads_the_block),
    };
    return test::Suite{.name = "font", .cases = cases, .count = ARRAY_COUNT(cases)};
}
