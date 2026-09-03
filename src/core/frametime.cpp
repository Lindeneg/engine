#include "core/frametime.h"

namespace frametime {

Stats create(f32 smoothing, f64 window, f64 now) {
    ASSERT_MSG(smoothing > 0.0f && smoothing <= 1.0f, "frametime: smoothing outside 0..1");
    ASSERT_MSG(window > 0.0, "frametime: zero window");

    Stats s{};
    s.smoothing = smoothing;
    s.window = window;
    s.window_start = now;
    return s;
}

void tick(Stats *s, f64 now, f32 dt) {
    ASSERT(s);

    s->smoothed_dt += (dt - s->smoothed_dt) * s->smoothing;

    s->frames++;
    s->window_worst = MAX(s->window_worst, dt);

    f64 elapsed = now - s->window_start;
    if (elapsed < s->window)
        return;

    s->fps = (f32)((f64)s->frames / elapsed);
    s->worst_dt = s->window_worst;

    s->window_start = now;
    s->frames = 0;
    s->window_worst = 0.0f;
}

} // namespace frametime
