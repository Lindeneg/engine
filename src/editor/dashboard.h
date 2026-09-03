#pragma once

#include "core/common.h"
#include "core/memory.h"

// The project screen: what the binary shows when it starts without a project.
// Recent projects to reopen, a folder to open, or a name and a location to
// create one from the engine's template. It draws through the widget set into
// a frame the caller runs, and answers when a project has been chosen.

#define DASHBOARD_STATUS_CAP 256

namespace dashboard {

struct Config {
    f32 width;      // of the panel, centred in the window
    f32 height;
    u32 recent_cap; // projects listed
};

struct Dashboard;

Dashboard *init(memory::Arena *a, const char *engine_root, Config cfg);

enum Result {
    RESULT_NONE, // still choosing
    RESULT_OPEN, // out_dir names the project
    RESULT_QUIT,
};

// One frame, inside ui's frame. `a` receives a created project's document
// briefly; the caller reopens the project properly.
Result frame(Dashboard *d, memory::Arena *a, memory::Arena *scratch, i32 fb_width, i32 fb_height,
             char *out_dir, u32 cap);

} // namespace dashboard
