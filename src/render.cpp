#include "render.h"

#include <glad/glad.h>

#include <stddef.h> // offsetof
#include <string.h>

#include "logger.h"

namespace render {

// The vertex layouts the attribute setup below describes. A field reorder in
// asset.h would otherwise mis-feed the GPU with no compiler in the loop.
static_assert(sizeof(asset::VertexStatic) == 32, "VertexStatic layout changed");
static_assert(sizeof(asset::VertexSkinned) == 56, "VertexSkinned layout changed");
static_assert(offsetof(asset::VertexStatic, pos) == offsetof(asset::VertexSkinned, pos) &&
                  offsetof(asset::VertexStatic, normal) == offsetof(asset::VertexSkinned, normal) &&
                  offsetof(asset::VertexStatic, uv) == offsetof(asset::VertexSkinned, uv),
              "static and skinned vertices must share their leading layout");

// The std140 block in common.glsl is laid out to exactly this size.
static_assert(sizeof(shader::FrameData) == 112, "FrameData no longer matches the Frame block");

// The sort keys below give mesh and material indices 16 bits; pool indices
// have 24, so this is the cap on those pools that keeps ordering exact.
#define KEY_INDEX_BITS 16
#define KEY_INDEX_MAX ((1u << KEY_INDEX_BITS) - 1)

// Opaque keys bucket depth in whole units up to this, as an early-Z tiebreaker
#define KEY_DEPTH_MAX 255u

// A driver-neutral fallback if GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT cannot be read
#define UBO_ALIGN_FALLBACK 256

// "nothing bound yet" for the per-pass state caches; no real value is ever this
#define UNBOUND 0xFFFFFFFFu

// Opaque draws group by the most expensive state change first; depth lands in
// the low bits so it only breaks ties, front-to-back, which feeds early-Z.
// Transparent draws sort on distance above everything else, because ordering
// there is correctness rather than performance.
enum Pass { PASS_OPAQUE, PASS_TRANSPARENT, PASS_COUNT };

struct DrawItem {
    u32 object; // index into g.objects; every submesh of one model shares it
    shader::Program *program;
    u32 vao;
    u32 index_offset;
    u32 index_count;
    asset::MaterialHandle material;
    gfx::RenderState state;
};

// Per submit_model, not per submesh: the pistol's 47 primitives would otherwise
// carry 47 copies of the same matrices.
struct ObjectData {
    linalg::Mat4 world;
    linalg::Mat3 normal;
    u32 joint_offset; // byte offset into the joint UBO
    b32 skinned;
};

struct SortEntry {
    u64 key;
    u32 item;
};

// The GPU side of one mesh slot. epoch is the handle generation it was
// uploaded for, so a recycled slot re-uploads instead of drawing stale buffers.
struct GpuMesh {
    u32 vao, vbo, ebo;
    u8 epoch;
};

static struct {
    Config cfg;

    GpuMesh *gpu; // indexed by mesh slot; NULL until attach_assets
    u32 gpu_cap;

    DrawItem *items;
    u32 item_count;

    ObjectData *objects;
    u32 object_count;

    SortEntry *pass[PASS_COUNT];
    u32 pass_count[PASS_COUNT];
    SortEntry *sort_tmp;

    u32 frame_ubo;
    u32 joint_ubo;

    // Staged on the CPU and uploaded once in flush, so submit_model issues no
    // GL calls at all and the caller's palette buffer is free immediately.
    u8 *joint_staging;
    u32 joint_stride; // bytes per slot, aligned to the driver's requirement
    u32 joint_count;  // slots used this frame

    linalg::Vec3 camera;
} g;

static u32 make_buffer(u32 target, u64 bytes, const void *data, u32 usage) {
    u32 id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(target, id);
    glBufferData(target, (GLsizeiptr)bytes, data, usage);
    return id;
}

static u32 make_ubo(u64 bytes, u32 binding) {
    u32 ubo = make_buffer(GL_UNIFORM_BUFFER, bytes, NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, binding, ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return ubo;
}

static void release_gpu(GpuMesh *e) {
    if (e->vao)
        glDeleteVertexArrays(1, &e->vao);
    if (e->vbo)
        glDeleteBuffers(1, &e->vbo);
    if (e->ebo)
        glDeleteBuffers(1, &e->ebo);
    *e = GpuMesh{};
}

static void on_mesh_release(void *, asset::MeshHandle h) {
    u32 idx = asset::handle_index(h.v);
    if (idx < g.gpu_cap)
        release_gpu(&g.gpu[idx]);
}

void init(memory::Arena *a, Config cfg, i32 fb_width, i32 fb_height) {
    ASSERT(a);
    ASSERT_MSG(cfg.max_draw_items && cfg.max_objects, "render: zero queue capacity");

    g.cfg = cfg;

    g.items = push_array(a, DrawItem, cfg.max_draw_items);
    g.objects = push_array(a, ObjectData, cfg.max_objects);
    for (u32 p = 0; p < PASS_COUNT; p++)
        g.pass[p] = push_array(a, SortEntry, cfg.max_draw_items);
    g.sort_tmp = push_array(a, SortEntry, cfg.max_draw_items);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glFrontFace(GL_CCW); // glTF defines counter-clockwise as front-facing

    // GL 3.3 requires a bound range to cover the block's declared size, and the
    // block is a fixed MAX_JOINTS array, so every slot costs the full palette
    // regardless of how many joints the rig actually has. A texture buffer
    // would avoid that; it costs four texelFetches per vertex instead.
    i32 align = UBO_ALIGN_FALLBACK;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    u32 slot_bytes = (u32)(sizeof(linalg::Mat4) * SHADER_MAX_JOINTS);
    g.joint_stride = (u32)ALIGN_UP_2(slot_bytes, (u64)align);

    // zeroed, not push_array_nz: only the first joint_count matrices of a slot
    // are written each frame, and the tail is uploaded either way
    u64 joint_bytes = (u64)g.joint_stride * MAX(cfg.max_skinned_draws, 1u);
    g.joint_staging = push_array(a, u8, joint_bytes);

    g.frame_ubo = make_ubo(sizeof(shader::FrameData), SHADER_FRAME_BINDING);
    g.joint_ubo = make_ubo(joint_bytes, SHADER_JOINT_BINDING);

    resize(fb_width, fb_height);
    LOG_INFO("render: %dx%d, queue %u items / %u objects / %u palettes (%.2f MiB)", fb_width,
             fb_height, cfg.max_draw_items, cfg.max_objects, cfg.max_skinned_draws,
             (f64)joint_bytes / (1024.0 * 1024.0));
}

void shutdown() {
    for (u32 i = 0; i < g.gpu_cap; i++)
        release_gpu(&g.gpu[i]);
    g.gpu = NULL;
    g.gpu_cap = 0;

    glDeleteBuffers(1, &g.frame_ubo);
    glDeleteBuffers(1, &g.joint_ubo);
    g.frame_ubo = 0;
    g.joint_ubo = 0;
}

void attach_assets(memory::Arena *a, asset::Manager *m) {
    ASSERT(a && m);
    ASSERT_MSG(!g.gpu, "render: attach_assets called twice");

    g.gpu_cap = asset::mesh_capacity(m);
    g.gpu = push_array(a, GpuMesh, g.gpu_cap);
    asset::on_mesh_release(m, on_mesh_release, NULL);
}

void resize(i32 fb_width, i32 fb_height) { glViewport(0, 0, fb_width, fb_height); }

void begin_frame(const shader::FrameData *frame, linalg::Vec4 clear_color) {
    ASSERT(frame);

    glBindBuffer(GL_UNIFORM_BUFFER, g.frame_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(shader::FrameData), frame);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    g.camera = linalg::Vec3{frame->camera_pos.x, frame->camera_pos.y, frame->camera_pos.z};

    g.item_count = 0;
    g.object_count = 0;
    g.joint_count = 0;
    for (u32 p = 0; p < PASS_COUNT; p++)
        g.pass_count[p] = 0;

    // depth writes must be on to clear the depth buffer, and a transparent
    // material at the end of the last frame will have turned them off
    glDepthMask(GL_TRUE);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void setup_attribs(asset::VertexKind kind) {
    u32 stride = (u32)asset::vertex_stride(kind);

    // offsets from the structs themselves; the static_asserts at the top pin
    // the two layouts to agreeing on the shared leading fields
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(asset::VertexStatic, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(asset::VertexStatic, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(asset::VertexStatic, uv));

    if (kind != asset::VERTEX_SKINNED)
        return;

    // glVertexAttribIPointer, not the float-converting variant: joint indices
    // are integers and must arrive in the shader as integers
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_SHORT, stride,
                           (const void *)offsetof(asset::VertexSkinned, joints));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(asset::VertexSkinned, weights));
}

// The GPU copy for a mesh handle, uploading on first sight or after the slot
// was recycled. NULL for a mesh with no vertices.
static GpuMesh *ensure_uploaded(asset::Manager *m, asset::MeshHandle h) {
    ASSERT_MSG(g.gpu, "render: attach_assets before drawing");

    u32 idx = asset::handle_index(h.v);
    u8 epoch = asset::handle_epoch(h.v);
    asset::Mesh *mesh = asset::get(m, h);
    if (mesh->vertex_count == 0)
        return NULL;

    // get() substitutes the fallback for a stale handle; upload that under its
    // own slot rather than the stale one's
    if (mesh == asset::get(m, asset::MESH_FALLBACK)) {
        idx = 0;
        epoch = 0;
    }

    GpuMesh *e = &g.gpu[idx];
    if (e->vao && e->epoch == epoch)
        return e;
    if (e->vao)
        release_gpu(e); // recycled slot the release hook did not see

    const void *verts = mesh->vertex_kind == asset::VERTEX_SKINNED
                            ? (const void *)mesh->vertices.skinned
                            : (const void *)mesh->vertices.statics;
    u64 vert_bytes = asset::vertex_stride(mesh->vertex_kind) * mesh->vertex_count;

    glGenVertexArrays(1, &e->vao);
    glBindVertexArray(e->vao);
    e->vbo = make_buffer(GL_ARRAY_BUFFER, vert_bytes, verts, GL_STATIC_DRAW);
    e->ebo = make_buffer(GL_ELEMENT_ARRAY_BUFFER, mesh->index_count * sizeof(u32), mesh->indices,
                         GL_STATIC_DRAW);
    setup_attribs(mesh->vertex_kind);
    glBindVertexArray(0);

    e->epoch = epoch;
    return e;
}

void upload_model(asset::Manager *m, asset::ModelHandle h) {
    asset::Model *model = asset::get(m, h);
    for (u32 i = 0; i < model->mesh_count; i++)
        ensure_uploaded(m, model->meshes[i]);
}

// What the geometry requires of a program, as opposed to what the material asks
// for. A skinned vertex layout leaves attributes 3 and 4 bound, and a program
// built without SKINNED would ignore them and draw the bind pose.
static u32 mesh_features(const asset::Mesh *mesh) {
    return mesh->vertex_kind == asset::VERTEX_SKINNED ? shader::FEATURE_SKINNED
                                                      : shader::FEATURE_NONE;
}

static u32 material_features(const asset::Material *mat) {
    return mat->alpha_mode == asset::ALPHA_MASK ? shader::FEATURE_ALPHA_MASK : shader::FEATURE_NONE;
}

// Positive floats order the same way as their bit patterns read as integers, so
// this is an exact, range-free ordering -- no far plane to normalise against.
static u32 depth_bits(f32 d) {
    u32 bits;
    memcpy(&bits, &d, sizeof(bits));
    return d > 0.0f ? bits : 0;
}

static u64 opaque_key(u32 program, u32 state, u32 mesh, u32 material, f32 dist) {
    ASSERT_MSG(mesh <= KEY_INDEX_MAX && material <= KEY_INDEX_MAX,
               "render: pool index past the %d bits the sort key holds", KEY_INDEX_BITS);

    // one-unit buckets out to KEY_DEPTH_MAX: a tiebreaker for early-Z, not a sort
    u64 depth = (u64)MIN((u32)MAX(dist, 0.0f), KEY_DEPTH_MAX);
    return ((u64)(program & 0xFFFF) << 48) | ((u64)(state & 0xFF) << 40) |
           ((u64)(mesh & KEY_INDEX_MAX) << 24) | ((u64)(material & KEY_INDEX_MAX) << 8) | depth;
}

static u64 transparent_key(u32 program, u32 mesh, f32 dist) {
    ASSERT_MSG(mesh <= KEY_INDEX_MAX, "render: pool index past the %d bits the sort key holds",
               KEY_INDEX_BITS);

    // inverted so ascending order comes out far-to-near
    u64 depth = ~(u64)depth_bits(dist) & 0xFFFFFFFFull;
    return (depth << 32) | ((u64)(program & 0xFFFF) << 16) | (u64)(mesh & KEY_INDEX_MAX);
}

void submit_model(asset::Manager *m, asset::ModelHandle h, linalg::Mat4 model_mat,
                  const linalg::Mat4 *palette, u32 joint_count) {
    asset::Model *model = asset::get(m, h);

    if (g.object_count == g.cfg.max_objects) {
        LOG_WARN_ONCE("render: more than %u objects in a frame, the rest are dropped",
                      g.cfg.max_objects);
        return;
    }

    u32 object = g.object_count++;
    ObjectData *o = &g.objects[object];

    // the import transform sits between world placement and the asset's own
    // native space, so it applies to normals as well as positions
    o->world = model_mat * model->import;
    o->normal = linalg::mat3_normal_matrix(o->world);
    o->joint_offset = 0;
    o->skinned = false;

    if (palette && joint_count > 0) {
        if (g.joint_count < g.cfg.max_skinned_draws) {
            if (joint_count > SHADER_MAX_JOINTS) {
                LOG_WARN_ONCE("render: %u joints exceeds the %d the palette holds", joint_count,
                              SHADER_MAX_JOINTS);
                joint_count = SHADER_MAX_JOINTS;
            }

            o->joint_offset = g.joint_count++ * g.joint_stride;
            o->skinned = true;
            memcpy(g.joint_staging + o->joint_offset, palette,
                   sizeof(linalg::Mat4) * joint_count);
        } else {
            LOG_WARN_ONCE("render: more than %u skinned draws in a frame, the rest draw unskinned",
                          g.cfg.max_skinned_draws);
        }
    }

    // One distance per object, from the bounds centre rather than the origin:
    // a model whose pivot sits at its feet would otherwise sort against a point
    // that is not where the geometry is.
    linalg::Vec3 lo, hi;
    asset::model_bounds(m, h, &lo, &hi);
    linalg::Vec3 center = linalg::mat4_mul_point(model_mat, (lo + hi) * 0.5f);
    f32 dist = linalg::vec3_length(center - g.camera);

    for (u32 i = 0; i < model->mesh_count; i++) {
        GpuMesh *gm = ensure_uploaded(m, model->meshes[i]);
        if (!gm)
            continue;

        asset::Mesh *mesh = asset::get(m, model->meshes[i]);
        u32 geometry = mesh_features(mesh);

        for (u32 sm = 0; sm < mesh->submesh_count; sm++) {
            const asset::Submesh *sub = &mesh->submeshes[sm];
            asset::Material *mat = asset::get(m, sub->material);

            if (g.item_count == g.cfg.max_draw_items) {
                LOG_WARN_ONCE("render: more than %u draw items in a frame, the rest are dropped",
                              g.cfg.max_draw_items);
                return;
            }

            // A skinned mesh whose palette did not fit must draw unskinned, not
            // with whatever palette the previous object left behind.
            u32 features = geometry | material_features(mat);
            if ((features & shader::FEATURE_SKINNED) && !o->skinned)
                features &= ~(u32)shader::FEATURE_SKINNED;

            DrawItem *it = &g.items[g.item_count];
            it->object = object;
            it->program = shader::program(mat->shader, features);
            it->vao = gm->vao;
            it->index_offset = sub->index_offset;
            it->index_count = sub->index_count;
            it->material = sub->material;
            it->state = mat->state;

            u32 pid = shader::program_id(mat->shader, features);
            u32 pass = mat->state.blend == gfx::BLEND_NONE ? PASS_OPAQUE : PASS_TRANSPARENT;

            u64 key = pass == PASS_OPAQUE
                          ? opaque_key(pid, gfx::state_key(it->state),
                                       asset::handle_index(model->meshes[i].v),
                                       asset::handle_index(sub->material.v), dist)
                          : transparent_key(pid, asset::handle_index(model->meshes[i].v), dist);

            g.pass[pass][g.pass_count[pass]++] = SortEntry{.key = key, .item = g.item_count};
            g.item_count++;
        }
    }
}

// LSD radix on the full 64-bit key: linear, stable, and it needs no comparator.
// Byte passes whose values are all identical are skipped, which is the common
// case for the high bytes.
static void radix_sort(SortEntry *a, SortEntry *tmp, u32 n) {
    if (n < 2)
        return;

    SortEntry *src = a;
    SortEntry *dst = tmp;

    for (u32 shift = 0; shift < 64; shift += 8) {
        u32 counts[256] = {};
        for (u32 i = 0; i < n; i++)
            counts[(src[i].key >> shift) & 0xFF]++;

        // every key shares this byte, so the pass would only copy
        if (counts[(src[0].key >> shift) & 0xFF] == n)
            continue;

        u32 sum = 0;
        for (u32 b = 0; b < 256; b++) {
            u32 c = counts[b];
            counts[b] = sum;
            sum += c;
        }

        for (u32 i = 0; i < n; i++)
            dst[counts[(src[i].key >> shift) & 0xFF]++] = src[i];

        SortEntry *swap = src;
        src = dst;
        dst = swap;
    }

    // an odd number of passes leaves the result in tmp
    if (src != a)
        memcpy(a, src, sizeof(SortEntry) * n);
}

static void apply_state(gfx::RenderState s) {
    switch (s.cull) {
    case gfx::CULL_NONE:
        glDisable(GL_CULL_FACE);
        break;
    case gfx::CULL_FRONT:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    default:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    }

    switch (s.blend) {
    case gfx::BLEND_ALPHA:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case gfx::BLEND_ADD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    default:
        glDisable(GL_BLEND);
        break;
    }

    if (s.depth_test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);

    glDepthMask(s.depth_write ? GL_TRUE : GL_FALSE);
}

static void flush_pass(asset::Manager *m, u32 pass) {
    u32 n = g.pass_count[pass];
    if (n == 0)
        return;

    radix_sort(g.pass[pass], g.sort_tmp, n);

    u32 bound_program = 0;
    u32 bound_vao = 0;
    u32 bound_state = UNBOUND;
    u32 bound_object = UNBOUND;
    u32 bound_material = UNBOUND;

    for (u32 i = 0; i < n; i++) {
        const DrawItem *it = &g.items[g.pass[pass][i].item];
        shader::Program *p = it->program;

        if (bound_program != p->gl) {
            glUseProgram(p->gl);
            bound_program = p->gl;

            // Uniform values live in the program object, so everything set
            // through a location is stale now even though the object and the
            // material have not changed. Getting this wrong shows up as one
            // model wearing another's colour.
            bound_object = UNBOUND;
            bound_material = UNBOUND;
        }

        u32 state = gfx::state_key(it->state);
        if (bound_state != state) {
            apply_state(it->state);
            bound_state = state;
        }

        if (bound_vao != it->vao) {
            glBindVertexArray(it->vao);
            bound_vao = it->vao;
        }

        if (bound_object != it->object) {
            const ObjectData *o = &g.objects[it->object];
            glUniformMatrix4fv(p->builtin[shader::BUILTIN_MODEL], 1, GL_FALSE, o->world.m);
            glUniformMatrix3fv(p->builtin[shader::BUILTIN_NORMAL_MAT], 1, GL_FALSE, o->normal.m);

            // Buffer bindings are context state, not program state, so this
            // survives a program change -- but the object cache was just
            // invalidated by one, so it is rebound harmlessly.
            if (o->skinned)
                glBindBufferRange(GL_UNIFORM_BUFFER, SHADER_JOINT_BINDING, g.joint_ubo,
                                  o->joint_offset, g.joint_stride);

            bound_object = it->object;
        }

        if (bound_material != it->material.v) {
            const asset::Material *mat = asset::get(m, it->material);
            glUniform4fv(p->builtin[shader::BUILTIN_BASE_COLOR], 1, &mat->base_color.x);
            glUniform1f(p->builtin[shader::BUILTIN_ALPHA_CUTOFF], mat->alpha_cutoff);
            bound_material = it->material.v;
        }

        glDrawElements(GL_TRIANGLES, (i32)it->index_count, GL_UNSIGNED_INT,
                       (const void *)((u64)it->index_offset * sizeof(u32)));
    }
}

void flush(asset::Manager *m) {
    if (g.joint_count > 0) {
        glBindBuffer(GL_UNIFORM_BUFFER, g.joint_ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(g.joint_count * g.joint_stride),
                        g.joint_staging);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    for (u32 p = 0; p < PASS_COUNT; p++)
        flush_pass(m, p);

    glBindVertexArray(0);
}

} // namespace render
