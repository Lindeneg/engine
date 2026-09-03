#include "gfx/debugdraw.h"

#include "suites.h"

static const linalg::Vec4 WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
static const linalg::Vec3 LO = {-1.0f, -1.0f, -1.0f};
static const linalg::Vec3 HI = {1.0f, 1.0f, 1.0f};

static void setup(memory::Arena *a, u32 lines, u32 retained, u32 segments) {
    debugdraw::Config cfg = debugdraw::config_from(NULL);
    cfg.max_lines = lines;
    cfg.max_retained = retained;
    cfg.sphere_segments = segments;
    debugdraw::init(a, cfg);
}

static void boxes_are_twelve_lines(memory::Arena *a) {
    setup(a, 64, 8, 8);

    debugdraw::box(LO, HI, WHITE);
    CHECK(debugdraw::line_count() == 12);

    debugdraw::box(linalg::mat4_from_translation(linalg::Vec3{5.0f, 0.0f, 0.0f}), LO, HI, WHITE);
    CHECK(debugdraw::line_count() == 24);

    debugdraw::shutdown();
}

static void spheres_axes_and_crosses(memory::Arena *a) {
    setup(a, 64, 8, 8);

    debugdraw::sphere(LO, 1.0f, WHITE);
    CHECK(debugdraw::line_count() == 3 * 8);

    debugdraw::axes(linalg::mat4_identity(), 1.0f);
    CHECK(debugdraw::line_count() == 3 * 8 + 3);

    debugdraw::cross(HI, 0.5f, WHITE);
    CHECK(debugdraw::line_count() == 3 * 8 + 6);

    debugdraw::shutdown();
}

static void lifetime_outlives_the_frame(memory::Arena *a) {
    setup(a, 64, 8, 8);

    debugdraw::line(LO, HI, WHITE);
    debugdraw::line(LO, HI, WHITE, debugdraw::Style{1.0f, false});
    CHECK(debugdraw::line_count() == 1);
    CHECK(debugdraw::retained_count() == 1);

    debugdraw::begin_frame(0.5f);
    CHECK(debugdraw::line_count() == 0);
    CHECK(debugdraw::retained_count() == 1);

    debugdraw::begin_frame(0.6f);
    CHECK(debugdraw::retained_count() == 0);

    debugdraw::shutdown();
}

static void expiry_keeps_the_survivors(memory::Arena *a) {
    setup(a, 64, 8, 8);

    // the short ones sit between the long ones, so removing one moves a long
    // one into its place and that one must still be aged
    const f32 lifetimes[] = {0.2f, 1.0f, 0.2f, 1.0f};
    for (u32 i = 0; i < ARRAY_COUNT(lifetimes); i++)
        debugdraw::line(LO, HI, WHITE, debugdraw::Style{lifetimes[i], false});
    CHECK(debugdraw::retained_count() == 4);

    debugdraw::begin_frame(0.3f);
    CHECK(debugdraw::retained_count() == 2);

    debugdraw::begin_frame(0.8f);
    CHECK(debugdraw::retained_count() == 0);

    debugdraw::shutdown();
}

static void overflow_drops_and_warns_once(memory::Arena *a) {
    setup(a, 2, 1, 8);

    test::mute();
    for (u32 i = 0; i < 4; i++)
        debugdraw::line(LO, HI, WHITE);
    CHECK(debugdraw::line_count() == 2);
    CHECK(test::unmute() == 1);

    test::mute();
    for (u32 i = 0; i < 3; i++)
        debugdraw::line(LO, HI, WHITE, debugdraw::Style{1.0f, false});
    CHECK(debugdraw::retained_count() == 1);
    CHECK(test::unmute() == 1);

    debugdraw::shutdown();
}

static void config_refuses_a_degenerate_circle(memory::Arena *a) {
    config::Doc *doc =
        MUST(config::parse(a, memory::push_str(a, "debug_draw {\n    sphere_segments 1\n}\n"),
                           "engine.cfg"));

    test::mute();
    debugdraw::Config cfg = debugdraw::config_from(doc->first);
    CHECK(test::unmute() == 1);
    CHECK(cfg.sphere_segments == 3);

    // and the defaults stand on their own
    debugdraw::Config dflt = debugdraw::config_from(NULL);
    CHECK(dflt.max_lines > 0 && dflt.max_retained > 0 && dflt.sphere_segments >= 3);
    CHECK(dflt.axis_color[0].w == 1.0f);
}

test::Suite debugdraw_suite() {
    static const test::Case cases[] = {
        CASE(boxes_are_twelve_lines),
        CASE(spheres_axes_and_crosses),
        CASE(lifetime_outlives_the_frame),
        CASE(expiry_keeps_the_survivors),
        CASE(overflow_drops_and_warns_once),
        CASE(config_refuses_a_degenerate_circle),
    };
    return test::Suite{.name = "debugdraw", .cases = cases, .count = ARRAY_COUNT(cases)};
}
