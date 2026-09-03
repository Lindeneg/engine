#include "gfx/canvas.h"

#include <glad/glad.h>
#include <math.h>
#include <stddef.h> // offsetof
#include <string.h>

#include "core/logger.h"
#include "gfx/shader.h"

#define DEFAULT_MAX_QUADS 16384
#define DEFAULT_MAX_BATCHES 256
#define DEFAULT_MAX_CLIP_DEPTH 16

#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

#define CHANNEL_MAX 255.0f

#define PROJECTION_UNIFORM "u_projection"
#define TEXTURE_UNIFORM "u_texture"
#define TEXTURE_UNIT 0

// the orthographic depth range; nothing here has depth
#define CLIP_NEAR -1.0f
#define CLIP_FAR 1.0f

namespace canvas {

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .max_quads = (u32)config::get_i32(body, "max_quads", DEFAULT_MAX_QUADS),
        .max_batches = (u32)config::get_i32(body, "max_batches", DEFAULT_MAX_BATCHES),
        .max_clip_depth = (u32)config::get_i32(body, "max_clip_depth", DEFAULT_MAX_CLIP_DEPTH),
    };
}

// pins the layout ensure_gpu describes
struct Vertex {
    linalg::Vec2 pos;
    linalg::Vec2 uv;
    u8 rgba[4];
};
static_assert(sizeof(Vertex) == 20, "Vertex layout changed");

// one draw: quads sharing a texture and a clip
struct Batch {
    const texture::Texture *texture; // NULL when font is set
    font::Font *font;
    Rect clip;
    u32 first; // quad index
    u32 count;
};

static const u8 WHITE_PIXEL[4] = {0xFF, 0xFF, 0xFF, 0xFF};

static struct {
    Config cfg;

    Vertex *vertices; // VERTICES_PER_QUAD per quad; NULL until init
    u32 quad_count;

    Batch *batches;
    u32 batch_count;

    Rect *clips; // a stack; [0] is the framebuffer
    u32 clip_depth;

    // the same six indices per quad, built once, uploaded with the buffers
    u32 *indices;

    i32 fb_w, fb_h;
    b32 in_frame;

    // created on the first flush
    u32 vao, vbo, ebo;
    texture::Texture white; // for fills with no glyph batch to join
} g;

void init(memory::Arena *a, Config cfg) {
    ASSERT(a);
    ASSERT_MSG(!g.vertices, "canvas::init called twice");
    ASSERT_MSG(cfg.max_quads && cfg.max_batches, "canvas: zero capacity");

    g.cfg = cfg;
    g.vertices = push_array(a, Vertex, (u64)cfg.max_quads * VERTICES_PER_QUAD);
    g.batches = push_array(a, Batch, cfg.max_batches);
    g.clips = push_array(a, Rect, cfg.max_clip_depth + 1);

    g.indices = push_array(a, u32, (u64)cfg.max_quads * INDICES_PER_QUAD);
    for (u32 q = 0; q < cfg.max_quads; q++) {
        u32 v = q * VERTICES_PER_QUAD;
        u32 *i = &g.indices[q * INDICES_PER_QUAD];
        // vertices go top-left, top-right, bottom-left, bottom-right
        i[0] = v + 0;
        i[1] = v + 2;
        i[2] = v + 1;
        i[3] = v + 1;
        i[4] = v + 2;
        i[5] = v + 3;
    }

    LOG_INFO("canvas: %u quads / %u batches (%.2f MiB)", cfg.max_quads, cfg.max_batches,
             (f64)(cfg.max_quads *
                   (VERTICES_PER_QUAD * sizeof(Vertex) + INDICES_PER_QUAD * sizeof(u32))) /
                 (1024.0 * 1024.0));
}

void shutdown() {
    if (g.vao)
        glDeleteVertexArrays(1, &g.vao);
    if (g.vbo)
        glDeleteBuffers(1, &g.vbo);
    if (g.ebo)
        glDeleteBuffers(1, &g.ebo);
    if (g.white.gl)
        texture::destroy(&g.white);
    memset(&g, 0, sizeof(g));
}

void begin_frame(i32 fb_width, i32 fb_height) {
    ASSERT_MSG(g.vertices, "canvas: init before begin_frame");

    g.fb_w = fb_width;
    g.fb_h = fb_height;
    g.quad_count = 0;
    g.batch_count = 0;
    g.clip_depth = 0;
    g.clips[0] = Rect{0.0f, 0.0f, (f32)fb_width, (f32)fb_height};
    g.in_frame = true;
}

static u8 to_channel(f32 v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (u8)(v * CHANNEL_MAX + 0.5f);
}

static void pack(linalg::Vec4 c, u8 *rgba) {
    rgba[0] = to_channel(c.x);
    rgba[1] = to_channel(c.y);
    rgba[2] = to_channel(c.z);
    rgba[3] = to_channel(c.w);
}

static b32 same_batch(const Batch *b, const texture::Texture *t, font::Font *f, Rect clip) {
    return b->texture == t && b->font == f && memcmp(&b->clip, &clip, sizeof(clip)) == 0;
}

// the four vertices of a new quad in the batch for (t, f), or NULL when it was dropped
static Vertex *push_quad(const texture::Texture *t, font::Font *f) {
    ASSERT_MSG(g.vertices, "canvas: init before drawing");
    ASSERT_MSG(g.in_frame, "canvas: begin_frame before drawing");

    Rect clip = g.clips[g.clip_depth];
    if (clip.w <= 0.0f || clip.h <= 0.0f)
        return NULL;

    if (g.quad_count == g.cfg.max_quads) {
        LOG_WARN_ONCE("canvas: more than %u quads in a frame, the rest are dropped",
                      g.cfg.max_quads);
        return NULL;
    }

    Batch *b = g.batch_count ? &g.batches[g.batch_count - 1] : NULL;
    if (!b || !same_batch(b, t, f, clip)) {
        if (g.batch_count == g.cfg.max_batches) {
            LOG_WARN_ONCE("canvas: more than %u batches in a frame, the rest are dropped",
                          g.cfg.max_batches);
            return NULL;
        }
        b = &g.batches[g.batch_count++];
        *b = Batch{t, f, clip, g.quad_count, 0};
    }

    b->count++;
    return &g.vertices[g.quad_count++ * VERTICES_PER_QUAD];
}

static void write_quad(Vertex *v, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1,
                       const u8 *rgba) {
    v[0].pos = linalg::Vec2{x0, y0};
    v[0].uv = linalg::Vec2{u0, v0};
    v[1].pos = linalg::Vec2{x1, y0};
    v[1].uv = linalg::Vec2{u1, v0};
    v[2].pos = linalg::Vec2{x0, y1};
    v[2].uv = linalg::Vec2{u0, v1};
    v[3].pos = linalg::Vec2{x1, y1};
    v[3].uv = linalg::Vec2{u1, v1};
    for (u32 i = 0; i < VERTICES_PER_QUAD; i++)
        memcpy(v[i].rgba, rgba, sizeof(v[i].rgba));
}

void rect(Rect r, linalg::Vec4 color) {
    u8 rgba[4];
    pack(color, rgba);

    // join the glyph batch in progress through its white rows, else the white texture
    Batch *last = g.batch_count ? &g.batches[g.batch_count - 1] : NULL;
    font::Font *f = last ? last->font : NULL;

    Vertex *v = f ? push_quad(NULL, f) : push_quad(&g.white, NULL);
    if (!v)
        return;

    f32 u = f ? f->white_u : 0.5f;
    f32 w = f ? f->white_v : 0.5f;
    write_quad(v, r.x, r.y, r.x + r.w, r.y + r.h, u, w, u, w, rgba);
}

void rect_outline(Rect r, f32 thickness, linalg::Vec4 color) {
    f32 t = thickness;
    rect(Rect{r.x, r.y, r.w, t}, color);
    rect(Rect{r.x, r.y + r.h - t, r.w, t}, color);
    rect(Rect{r.x, r.y + t, t, r.h - 2.0f * t}, color);
    rect(Rect{r.x + r.w - t, r.y + t, t, r.h - 2.0f * t}, color);
}

void image(const texture::Texture *t, Rect r, linalg::Vec4 tint) {
    ASSERT(t);

    u8 rgba[4];
    pack(tint, rgba);

    Vertex *v = push_quad(t, NULL);
    if (!v)
        return;
    write_quad(v, r.x, r.y, r.x + r.w, r.y + r.h, 0.0f, 0.0f, 1.0f, 1.0f, rgba);
}

f32 text(font::Font *f, f32 x, f32 y, const char *s, linalg::Vec4 color) {
    ASSERT(f && s);

    u8 rgba[4];
    pack(color, rgba);

    f32 pen = x;
    for (u32 cp = font::next_codepoint(&s); cp; cp = font::next_codepoint(&s)) {
        const font::Glyph *gl = font::glyph(f, cp);
        if (!gl)
            continue;

        // a space has an advance and no quad
        if (gl->x1 > gl->x0 && gl->y1 > gl->y0) {
            Vertex *v = push_quad(NULL, f);
            if (v)
                write_quad(v, pen + gl->x0, y + gl->y0, pen + gl->x1, y + gl->y1, gl->u0, gl->v0,
                           gl->u1, gl->v1, rgba);
        }
        pen += gl->advance;
    }
    return pen - x;
}

static Rect intersect(Rect a, Rect b) {
    f32 x0 = MAX(a.x, b.x);
    f32 y0 = MAX(a.y, b.y);
    f32 x1 = MIN(a.x + a.w, b.x + b.w);
    f32 y1 = MIN(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0}; // empty when w or h is not positive
}

void push_clip(Rect r) {
    ASSERT_MSG(g.in_frame, "canvas: begin_frame before push_clip");
    ASSERT_MSG(g.clip_depth < g.cfg.max_clip_depth, "canvas: clips nested deeper than %u",
               g.cfg.max_clip_depth);
    g.clips[g.clip_depth + 1] = intersect(g.clips[g.clip_depth], r);
    g.clip_depth++;
}

void pop_clip() {
    ASSERT_MSG(g.clip_depth > 0, "canvas: pop_clip without a push");
    g.clip_depth--;
}

static void ensure_gpu() {
    if (g.vao)
        return;

    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((u64)g.cfg.max_quads * VERTICES_PER_QUAD * sizeof(Vertex)), NULL,
                 GL_STREAM_DRAW);

    glGenBuffers(1, &g.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((u64)g.cfg.max_quads * INDICES_PER_QUAD * sizeof(u32)), g.indices,
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, rgba));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g.white = texture::create(1, 1, texture::FORMAT_RGBA8, texture::FILTER_NEAREST, WHITE_PIXEL);
}

// GL's scissor counts from the bottom, in whole pixels; round outward
static void apply_clip(Rect c) {
    i32 x0 = (i32)floorf(c.x);
    i32 y0 = (i32)floorf(c.y);
    i32 x1 = (i32)ceilf(c.x + c.w);
    i32 y1 = (i32)ceilf(c.y + c.h);
    glScissor(x0, g.fb_h - y1, x1 - x0, y1 - y0);
}

void flush() {
    ASSERT_MSG(g.in_frame, "canvas: begin_frame before flush");
    ASSERT_MSG(g.clip_depth == 0, "canvas: a clip was pushed and never popped");
    g.in_frame = false;

    if (g.quad_count == 0)
        return;

    ensure_gpu();

    shader::Program *p =
        shader::program(shader::role_handle(shader::ROLE_CANVAS), shader::FEATURE_NONE);
    glUseProgram(p->gl);

    linalg::Mat4 projection =
        linalg::mat4_orthographic(0.0f, (f32)g.fb_w, (f32)g.fb_h, 0.0f, CLIP_NEAR, CLIP_FAR);
    glUniformMatrix4fv(shader::uniform_loc(p, PROJECTION_UNIFORM), 1, GL_FALSE, projection.m);
    glUniform1i(shader::uniform_loc(p, TEXTURE_UNIFORM), TEXTURE_UNIT);

    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);

    // orphan, then fill: the driver need not wait on last frame's draw
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((u64)g.cfg.max_quads * VERTICES_PER_QUAD * sizeof(Vertex)), NULL,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)((u64)g.quad_count * VERTICES_PER_QUAD * sizeof(Vertex)),
                    g.vertices);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);

    for (u32 i = 0; i < g.batch_count; i++) {
        const Batch *b = &g.batches[i];
        const texture::Texture *t = b->font ? font::texture(b->font) : b->texture;
        texture::bind(t, TEXTURE_UNIT);
        apply_clip(b->clip);
        glDrawElements(GL_TRIANGLES, (i32)(b->count * INDICES_PER_QUAD), GL_UNSIGNED_INT,
                       (const void *)((u64)b->first * INDICES_PER_QUAD * sizeof(u32)));
    }

    // back to what render::begin_frame expects to find
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

u32 quad_count() { return g.quad_count; }
u32 batch_count() { return g.batch_count; }

} // namespace canvas
