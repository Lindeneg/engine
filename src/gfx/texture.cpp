#include "gfx/texture.h"

#include <glad/glad.h>

namespace texture {

Texture create(u32 width, u32 height, Format format, Filter filter, const void *pixels) {
    ASSERT_MSG(width && height, "texture: zero size");
    ASSERT(format < FORMAT_COUNT && filter < FILTER_COUNT);

    Texture t{};
    t.width = width;
    t.height = height;
    t.format = format;

    glGenTextures(1, &t.gl);
    glBindTexture(GL_TEXTURE_2D, t.gl);

    // rows are packed as tightly as the caller laid them out
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (format == FORMAT_RGBA8) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (i32)width, (i32)height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (i32)width, (i32)height, 0, GL_RED, GL_UNSIGNED_BYTE,
                     pixels);
    }

    if (format == FORMAT_COVERAGE) {
        const i32 swizzle[4] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    }

    i32 gl_filter = filter == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

void destroy(Texture *t) {
    ASSERT(t);
    if (t->gl)
        glDeleteTextures(1, &t->gl);
    *t = Texture{};
}

void bind(const Texture *t, u32 unit) {
    ASSERT(t);
    ASSERT_MSG(t->gl, "texture: binding one that was never created");
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t->gl);
}

} // namespace texture
