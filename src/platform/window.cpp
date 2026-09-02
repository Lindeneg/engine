#include "platform/window.h"

// glad must come before any GLFW header, and GLFW_INCLUDE_NONE stops GLFW
// pulling in a second, conflicting set of GL declarations.
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "core/logger.h"
#include "core/str.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_TITLE "explore"
#define DEFAULT_VSYNC true

namespace window {

struct Window {
    GLFWwindow *handle;

    // wheel ticks since the last take_scroll; GLFW only reports these as events
    f32 scroll;

    b32 captured;
};

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .width = config::get_i32(body, "width", DEFAULT_WIDTH),
        .height = config::get_i32(body, "height", DEFAULT_HEIGHT),
        .title = config::get(body, "title", DEFAULT_TITLE),
        .vsync = config::get_b32(body, "vsync", DEFAULT_VSYNC),
    };
}

// Pasted from the same list as the enum, so a key can only ever be added to both.
static const i32 KEY_CODES[] = {
#define WINDOW_KEY_CODE(ID, name) GLFW_KEY_##ID,
    WINDOW_KEY_LIST(WINDOW_KEY_CODE)
#undef WINDOW_KEY_CODE
};
static_assert(ARRAY_COUNT(KEY_CODES) == KEY_COUNT, "key codes and key ids disagree");

static const char *KEY_NAMES[] = {
#define WINDOW_KEY_NAME(ID, name) name,
    WINDOW_KEY_LIST(WINDOW_KEY_NAME)
#undef WINDOW_KEY_NAME
};
static_assert(ARRAY_COUNT(KEY_NAMES) == KEY_COUNT, "key names and key ids disagree");

static const i32 BUTTON_CODES[] = {
#define WINDOW_BUTTON_CODE(ID, name) GLFW_MOUSE_BUTTON_##ID,
    WINDOW_BUTTON_LIST(WINDOW_BUTTON_CODE)
#undef WINDOW_BUTTON_CODE
};
static_assert(ARRAY_COUNT(BUTTON_CODES) == BUTTON_COUNT, "button codes and button ids disagree");

static const char *BUTTON_NAMES[] = {
#define WINDOW_BUTTON_NAME(ID, name) name,
    WINDOW_BUTTON_LIST(WINDOW_BUTTON_NAME)
#undef WINDOW_BUTTON_NAME
};
static_assert(ARRAY_COUNT(BUTTON_NAMES) == BUTTON_COUNT, "button names and button ids disagree");

const char *key_name(Key k) {
    ASSERT(k < KEY_COUNT);
    return KEY_NAMES[k];
}

const char *button_name(Button b) {
    ASSERT(b < BUTTON_COUNT);
    return BUTTON_NAMES[b];
}

b32 key_from_name(const char *s, Key *out) {
    i32 i = str::name_index(KEY_NAMES, KEY_COUNT, s);
    if (i < 0)
        return false;
    if (out)
        *out = (Key)i;
    return true;
}

b32 button_from_name(const char *s, Button *out) {
    i32 i = str::name_index(BUTTON_NAMES, BUTTON_COUNT, s);
    if (i < 0)
        return false;
    if (out)
        *out = (Button)i;
    return true;
}

static void on_error(int code, const char *desc) { LOG_ERR("glfw: %s (%d)", desc, code); }

static void on_scroll(GLFWwindow *h, double, double dy) {
    Window *w = (Window *)glfwGetWindowUserPointer(h);
    if (w)
        w->scroll += (f32)dy;
}

WindowResult create(memory::Arena *a, Config cfg) {
    ASSERT(a);

    glfwSetErrorCallback(on_error);

    if (!glfwInit()) {
        LOG_ERR("window: glfwInit failed");
        return WindowResult{.err = result::ERR_IO};
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *h = glfwCreateWindow(cfg.width, cfg.height, cfg.title, NULL, NULL);
    if (!h) {
        LOG_ERR("window: no GL 3.3 core context available");
        glfwTerminate();
        return WindowResult{.err = result::ERR_UNSUPPORTED};
    }

    glfwMakeContextCurrent(h);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        LOG_ERR("window: failed to load GL entry points");
        glfwDestroyWindow(h);
        glfwTerminate();
        return WindowResult{.err = result::ERR_IO};
    }

    glfwSwapInterval(cfg.vsync ? 1 : 0);

    Window *w = push_struct(a, Window);
    w->handle = h;

    glfwSetWindowUserPointer(h, w);
    glfwSetScrollCallback(h, on_scroll);

    LOG_INFO("window: %dx%d vsync=%s", cfg.width, cfg.height, cfg.vsync ? "on" : "off");
    LOG_INFO("gl: %s", (const char *)glGetString(GL_VERSION));
    LOG_INFO("gl: %s", (const char *)glGetString(GL_RENDERER));

    return WindowResult{.val = w};
}

void destroy(Window *w) {
    if (!w || !w->handle)
        return;
    glfwDestroyWindow(w->handle);
    glfwTerminate();
    w->handle = NULL;
}

b32 should_close(Window *w) {
    ASSERT(w);
    return glfwWindowShouldClose(w->handle);
}

void poll(Window *) { glfwPollEvents(); }

void swap(Window *w) {
    ASSERT(w);
    glfwSwapBuffers(w->handle);
}

b32 key_down(Window *w, Key k) {
    ASSERT(w && k < KEY_COUNT);
    return glfwGetKey(w->handle, KEY_CODES[k]) == GLFW_PRESS;
}

b32 button_down(Window *w, Button b) {
    ASSERT(w && b < BUTTON_COUNT);
    return glfwGetMouseButton(w->handle, BUTTON_CODES[b]) == GLFW_PRESS;
}

void cursor_position(Window *w, f32 *out_x, f32 *out_y) {
    ASSERT(w && out_x && out_y);

    f64 cx = 0.0;
    f64 cy = 0.0;
    glfwGetCursorPos(w->handle, &cx, &cy);

    i32 win_w = 0;
    i32 win_h = 0;
    i32 fb_w = 0;
    i32 fb_h = 0;
    glfwGetWindowSize(w->handle, &win_w, &win_h);
    glfwGetFramebufferSize(w->handle, &fb_w, &fb_h);

    *out_x = (f32)cx * (win_w > 0 ? (f32)fb_w / (f32)win_w : 1.0f);
    *out_y = (f32)cy * (win_h > 0 ? (f32)fb_h / (f32)win_h : 1.0f);
}

f32 take_scroll(Window *w) {
    ASSERT(w);
    f32 s = w->scroll;
    w->scroll = 0.0f;
    return s;
}

void set_cursor_captured(Window *w, b32 captured) {
    ASSERT(w);

    b32 want = captured ? 1 : 0;
    if (w->captured == want)
        return;
    w->captured = want;

    glfwSetInputMode(w->handle, GLFW_CURSOR, want ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

    // Raw motion only applies while the cursor is disabled, and only exists on
    // some platforms; without it the pointer has already been accelerated and
    // clamped by the desktop before we see it.
#ifdef GLFW_RAW_MOUSE_MOTION
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(w->handle, GLFW_RAW_MOUSE_MOTION, want ? GLFW_TRUE : GLFW_FALSE);
#endif
}

b32 cursor_captured(Window *w) {
    ASSERT(w);
    return w->captured;
}

void framebuffer_size(Window *w, i32 *out_w, i32 *out_h) {
    ASSERT(w && out_w && out_h);
    glfwGetFramebufferSize(w->handle, out_w, out_h);
}

f64 time() { return glfwGetTime(); }

} // namespace window
