#include "platform/window.h"

// glad must come before any GLFW header, and GLFW_INCLUDE_NONE stops GLFW
// pulling in a second, conflicting set of GL declarations.
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "core/logger.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_TITLE "explore"
#define DEFAULT_VSYNC true

namespace window {

struct Window {
    GLFWwindow *handle;
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

static const i32 KEY_CODES[KEY_COUNT] = {
    GLFW_KEY_ESCAPE, GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D, GLFW_KEY_SPACE,
};

static void on_error(int code, const char *desc) { LOG_ERR("glfw: %s (%d)", desc, code); }

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

void framebuffer_size(Window *w, i32 *out_w, i32 *out_h) {
    ASSERT(w && out_w && out_h);
    glfwGetFramebufferSize(w->handle, out_w, out_h);
}

f64 time() { return glfwGetTime(); }

} // namespace window
