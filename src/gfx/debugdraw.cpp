#include "gfx/debugdraw.h"

#include <glad/glad.h>
#include <stddef.h> // offsetof
#include <string.h>

#include "core/logger.h"
#include "gfx/shader.h"

#define DEFAULT_MAX_LINES 8192
#define DEFAULT_MAX_RETAINED 4096
#define DEFAULT_SPHERE_SEGMENTS 24

// fewer is not a circle
#define MIN_SPHERE_SEGMENTS 3

#define BOX_CORNERS 8
#define BOX_EDGES 12
#define AXIS_COUNT 3

#define CHANNEL_MAX 255.0f

namespace debugdraw {

static const linalg::Vec4 DEFAULT_AXIS_COLOR[AXIS_COUNT] = {
    {1.0f, 0.2f, 0.2f, 1.0f},
    {0.2f, 1.0f, 0.2f, 1.0f},
    {0.2f, 0.4f, 1.0f, 1.0f},
};

static const char *AXIS_COLOR_KEYS[AXIS_COUNT] = {"axis_x_color", "axis_y_color", "axis_z_color"};

static const linalg::Vec3 WORLD_AXIS[AXIS_COUNT] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
};

// corner i: bit 0 is x at hi, bit 1 y, bit 2 z; an edge joins corners one bit apart
static const u8 BOX_EDGE[BOX_EDGES][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, // along x
    {0, 2}, {1, 3}, {4, 6}, {5, 7}, // along y
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, // along z
};

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;

    Config cfg{
        .max_lines = (u32)config::get_i32(body, "max_lines", DEFAULT_MAX_LINES),
        .max_retained = (u32)config::get_i32(body, "max_retained", DEFAULT_MAX_RETAINED),
        .sphere_segments =
            (u32)config::get_i32(body, "sphere_segments", DEFAULT_SPHERE_SEGMENTS),
    };
    for (u32 i = 0; i < AXIS_COUNT; i++)
        cfg.axis_color[i] =
            config::value_vec4(config::find(body, AXIS_COLOR_KEYS[i]), DEFAULT_AXIS_COLOR[i]);

    if (cfg.sphere_segments < MIN_SPHERE_SEGMENTS) {
        LOG_WARN("debugdraw: sphere_segments %u is below %d, using %d", cfg.sphere_segments,
                 MIN_SPHERE_SEGMENTS, MIN_SPHERE_SEGMENTS);
        cfg.sphere_segments = MIN_SPHERE_SEGMENTS;
    }
    return cfg;
}

// pins the layout ensure_gpu describes
struct Vertex {
    linalg::Vec3 pos;
    u8 rgba[4];
};
static_assert(sizeof(Vertex) == 16, "Vertex layout changed");

struct Line {
    Vertex a, b;
    b32 x_ray;
};

struct Retained {
    Line line;
    f32 remaining; // seconds
};

static struct {
    Config cfg;

    Line *lines; // immediate; NULL until init
    u32 line_count;

    Retained *retained;
    u32 retained_count;

    // both sets, depth-tested first then x-ray, rebuilt every flush
    Vertex *staging;
    u32 staging_cap;

    u32 vao, vbo; // created on the first flush
} g;

void init(memory::Arena *a, Config cfg) {
    ASSERT(a);
    ASSERT_MSG(!g.lines, "debugdraw::init called twice");
    ASSERT_MSG(cfg.max_lines && cfg.max_retained, "debugdraw: zero capacity");

    g.cfg = cfg;
    g.lines = push_array(a, Line, cfg.max_lines);
    g.retained = push_array(a, Retained, cfg.max_retained);
    g.staging_cap = 2 * (cfg.max_lines + cfg.max_retained);
    g.staging = push_array(a, Vertex, g.staging_cap);

    LOG_INFO("debugdraw: %u lines / %u kept (%.2f MiB)", cfg.max_lines, cfg.max_retained,
             (f64)(g.staging_cap * sizeof(Vertex)) / (1024.0 * 1024.0));
}

void shutdown() {
    if (g.vao)
        glDeleteVertexArrays(1, &g.vao);
    if (g.vbo)
        glDeleteBuffers(1, &g.vbo);
    memset(&g, 0, sizeof(g));
}

static u8 to_channel(f32 v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (u8)(v * CHANNEL_MAX + 0.5f);
}

static Vertex vertex(linalg::Vec3 p, linalg::Vec4 c) {
    Vertex v;
    v.pos = p;
    v.rgba[0] = to_channel(c.x);
    v.rgba[1] = to_channel(c.y);
    v.rgba[2] = to_channel(c.z);
    v.rgba[3] = to_channel(c.w);
    return v;
}

static void push(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec4 color, Style s) {
    ASSERT_MSG(g.lines, "debugdraw: init before drawing");

    Line l{vertex(a, color), vertex(b, color), s.x_ray};

    if (s.lifetime > 0.0f) {
        if (g.retained_count == g.cfg.max_retained) {
            LOG_WARN_ONCE("debugdraw: more than %u lines kept at once, the rest are dropped",
                          g.cfg.max_retained);
            return;
        }
        g.retained[g.retained_count++] = Retained{l, s.lifetime};
        return;
    }

    if (g.line_count == g.cfg.max_lines) {
        LOG_WARN_ONCE("debugdraw: more than %u lines in a frame, the rest are dropped",
                      g.cfg.max_lines);
        return;
    }
    g.lines[g.line_count++] = l;
}

void line(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec4 color) { push(a, b, color, Style{}); }
void line(linalg::Vec3 a, linalg::Vec3 b, linalg::Vec4 color, Style s) { push(a, b, color, s); }

// NULL transform for axis-aligned
static void box_lines(const linalg::Mat4 *transform, linalg::Vec3 lo, linalg::Vec3 hi,
                      linalg::Vec4 color, Style s) {
    linalg::Vec3 c[BOX_CORNERS];
    for (u32 i = 0; i < BOX_CORNERS; i++) {
        linalg::Vec3 p{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        c[i] = transform ? linalg::mat4_mul_point(*transform, p) : p;
    }
    for (u32 e = 0; e < BOX_EDGES; e++)
        push(c[BOX_EDGE[e][0]], c[BOX_EDGE[e][1]], color, s);
}

void box(linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color) {
    box_lines(NULL, lo, hi, color, Style{});
}
void box(linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color, Style s) {
    box_lines(NULL, lo, hi, color, s);
}
void box(linalg::Mat4 transform, linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color) {
    box_lines(&transform, lo, hi, color, Style{});
}
void box(linalg::Mat4 transform, linalg::Vec3 lo, linalg::Vec3 hi, linalg::Vec4 color, Style s) {
    box_lines(&transform, lo, hi, color, s);
}

// in the plane spanned by u and v, both unit
static void circle(linalg::Vec3 center, linalg::Vec3 u, linalg::Vec3 v, f32 radius,
                   linalg::Vec4 color, Style s) {
    u32 n = g.cfg.sphere_segments;
    linalg::Vec3 prev = center + u * radius;
    for (u32 i = 1; i <= n; i++) {
        f32 t = (f32)i / (f32)n * 2.0f * PI_F32;
        linalg::Vec3 p = center + u * (cosf(t) * radius) + v * (sinf(t) * radius);
        push(prev, p, color, s);
        prev = p;
    }
}

void sphere(linalg::Vec3 center, f32 radius, linalg::Vec4 color, Style s) {
    circle(center, WORLD_AXIS[0], WORLD_AXIS[1], radius, color, s);
    circle(center, WORLD_AXIS[1], WORLD_AXIS[2], radius, color, s);
    circle(center, WORLD_AXIS[0], WORLD_AXIS[2], radius, color, s);
}
void sphere(linalg::Vec3 center, f32 radius, linalg::Vec4 color) {
    sphere(center, radius, color, Style{});
}

void axes(linalg::Mat4 transform, f32 length, Style s) {
    linalg::Vec3 o = linalg::mat4_translation(transform);
    for (u32 i = 0; i < AXIS_COUNT; i++) {
        // a zero axis (zero scale) draws nothing rather than asserting
        linalg::Vec3 d = linalg::vec3_normalize_or(linalg::mat4_axis(transform, i),
                                                   linalg::Vec3{0.0f, 0.0f, 0.0f});
        push(o, o + d * length, g.cfg.axis_color[i], s);
    }
}
void axes(linalg::Mat4 transform, f32 length) { axes(transform, length, Style{}); }

void cross(linalg::Vec3 p, f32 size, linalg::Vec4 color, Style s) {
    f32 h = size * 0.5f;
    for (u32 i = 0; i < AXIS_COUNT; i++)
        push(p - WORLD_AXIS[i] * h, p + WORLD_AXIS[i] * h, color, s);
}
void cross(linalg::Vec3 p, f32 size, linalg::Vec4 color) { cross(p, size, color, Style{}); }

void begin_frame(f32 dt) {
    ASSERT_MSG(g.lines, "debugdraw: init before begin_frame");

    g.line_count = 0;

    // swap-remove; the line moved into the hole is aged on the next pass of the loop
    for (u32 i = 0; i < g.retained_count;) {
        g.retained[i].remaining -= dt;
        if (g.retained[i].remaining <= 0.0f)
            g.retained[i] = g.retained[--g.retained_count];
        else
            i++;
    }
}

static u32 stage(b32 x_ray, u32 at) {
    for (u32 i = 0; i < g.line_count; i++) {
        if (g.lines[i].x_ray != x_ray)
            continue;
        g.staging[at++] = g.lines[i].a;
        g.staging[at++] = g.lines[i].b;
    }
    for (u32 i = 0; i < g.retained_count; i++) {
        if (g.retained[i].line.x_ray != x_ray)
            continue;
        g.staging[at++] = g.retained[i].line.a;
        g.staging[at++] = g.retained[i].line.b;
    }
    return at;
}

static void ensure_gpu() {
    if (g.vao)
        return;

    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g.staging_cap * sizeof(Vertex)), NULL,
                 GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, rgba));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void flush() {
    ASSERT_MSG(g.lines, "debugdraw: init before flush");

    u32 depth_end = stage(false, 0);
    u32 end = stage(true, depth_end);
    if (end == 0)
        return;

    ensure_gpu();

    shader::Program *p =
        shader::program(shader::role_handle(shader::ROLE_DEBUG), shader::FEATURE_NONE);
    glUseProgram(p->gl);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);

    // orphan, then fill: the driver need not wait on last frame's draw
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g.staging_cap * sizeof(Vertex)), NULL,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(end * sizeof(Vertex)), g.staging);

    // lines are read against the scene's depth, never written into it
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glEnable(GL_DEPTH_TEST);
    if (depth_end)
        glDrawArrays(GL_LINES, 0, (i32)depth_end);

    glDisable(GL_DEPTH_TEST);
    if (end > depth_end)
        glDrawArrays(GL_LINES, (i32)depth_end, (i32)(end - depth_end));

    // back to what render::begin_frame expects to find
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

u32 line_count() { return g.line_count; }
u32 retained_count() { return g.retained_count; }

} // namespace debugdraw
