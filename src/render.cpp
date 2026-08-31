#include "render.h"

#include <glad/glad.h>

#include "logger.h"

namespace render {

void init(i32 fb_width, i32 fb_height) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW); // glTF defines counter-clockwise as front-facing

    resize(fb_width, fb_height);
    LOG_INFO("render: initialised at %dx%d", fb_width, fb_height);
}

void resize(i32 fb_width, i32 fb_height) { glViewport(0, 0, fb_width, fb_height); }

void begin_frame(linalg::Vec4 clear_color) {
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

} // namespace render
