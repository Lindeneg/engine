#include "core/frametime.h"

#include <math.h>

#include "suites.h"

static b32 near(f32 a, f32 b) { return fabsf(a - b) < 1e-4f; }

static void window_yields_fps_and_worst(memory::Arena *) {
    frametime::Stats s = frametime::create(0.5f, 1.0, 0.0);

    // 125 ms is exact in binary, so eight of them reach the window without
    // rounding short of it. Seven leave it open; nothing is reported yet
    const f32 step = 0.125f;
    f64 now = 0.0;
    for (u32 i = 0; i < 7; i++) {
        now += (f64)step;
        frametime::tick(&s, now, step);
    }
    CHECK(s.fps == 0.0f && s.worst_dt == 0.0f);
    CHECK(s.frames == 7);

    // the eighth closes it, with one spike in it
    now += (f64)step;
    frametime::tick(&s, now, 0.25f);
    CHECK(near(s.fps, 8.0f));
    CHECK(near(s.worst_dt, 0.25f));
    CHECK(s.frames == 0 && s.window_worst == 0.0f);

    // the numbers hold until the next window closes
    now += (f64)step;
    frametime::tick(&s, now, 0.01f);
    CHECK(near(s.fps, 8.0f));
    CHECK(near(s.worst_dt, 0.25f));
}

static void smoothing_approaches_the_frame_time(memory::Arena *) {
    frametime::Stats s = frametime::create(0.5f, 10.0, 0.0);

    frametime::tick(&s, 0.1, 0.1f);
    CHECK(near(s.smoothed_dt, 0.05f));
    frametime::tick(&s, 0.2, 0.1f);
    CHECK(near(s.smoothed_dt, 0.075f));

    // smoothing 1 is no smoothing
    frametime::Stats raw = frametime::create(1.0f, 10.0, 0.0);
    frametime::tick(&raw, 0.1, 0.1f);
    CHECK(raw.smoothed_dt == 0.1f);
}

test::Suite frametime_suite() {
    static const test::Case cases[] = {
        CASE(window_yields_fps_and_worst),
        CASE(smoothing_approaches_the_frame_time),
    };
    return test::Suite{.name = "frametime", .cases = cases, .count = ARRAY_COUNT(cases)};
}
