#pragma once

#include "core/common.h"

// Frame timing for a readout: a smoothed frame time for a number that can be
// read while it moves, and, over a fixed window, the frame rate and the worst
// frame. The window numbers hold still between windows, so a spike stays on
// screen long enough to be seen. Nothing here reads a clock; the caller passes
// the time it already has.

namespace frametime {

struct Stats {
    f32 smoothing; // per-frame blend toward the new frame time, 0..1
    f64 window;    // seconds

    f32 smoothed_dt;
    f32 fps;      // frames over the last completed window; 0 until one completes
    f32 worst_dt; // the longest frame in it

    f64 window_start;
    u32 frames; // in the window so far
    f32 window_worst;
};

Stats create(f32 smoothing, f64 window, f64 now);

// Once per frame, with the frame's dt in seconds.
void tick(Stats *s, f64 now, f32 dt);

} // namespace frametime
