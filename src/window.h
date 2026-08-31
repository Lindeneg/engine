#pragma once

#include "common.h"
#include "memory.h"
#include "result.h"

namespace window {

// Our own key ids so GLFW's codes stay inside window.cpp.
enum Key { KEY_ESCAPE, KEY_W, KEY_A, KEY_S, KEY_D, KEY_SPACE, KEY_COUNT };

struct Config {
    i32 width;
    i32 height;
    const char *title;
    b32 vsync;
};

struct Window;

RESULT_TYPE(Window *, WindowResult);

// Creates the window and a GL 3.3 core context, and loads the GL entry points.
WindowResult create(memory::Arena *a, Config cfg);
void destroy(Window *w);

b32 should_close(Window *w);
void poll(Window *w);
void swap(Window *w);

b32 key_down(Window *w, Key k);

// Framebuffer size, not window size: they differ on scaled displays and it is
// the framebuffer that the GL viewport wants.
void framebuffer_size(Window *w, i32 *out_w, i32 *out_h);

f64 time();

} // namespace window
