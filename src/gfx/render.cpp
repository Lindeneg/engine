#include "gfx/render.h"

#include <glad/glad.h>
#include <stddef.h> // offsetof
#include <string.h>

#include "core/logger.h"
#include "gfx/drawkey.h"

namespace render {

// pins the layouts setup_attribs describes
static_assert(sizeof(asset::VertexStatic) == 32, "VertexStatic layout changed");
static_assert(sizeof(asset::VertexSkinned) == 56, "VertexSkinned layout changed");
static_assert(offsetof(asset::VertexStatic, pos) == offsetof(asset::VertexSkinned, pos) &&
                  offsetof(asset::VertexStatic, normal) == offsetof(asset::VertexSkinned, normal) &&
                  offsetof(asset::VertexStatic, uv) == offsetof(asset::VertexSkinned, uv),
              "static and skinned vertices must share their leading layout");

// The std140 block in common.glsl is laid out to exactly this size.
static_assert(sizeof(shader::FrameData) == 112, "FrameData no longer matches the Frame block");

// if GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT cannot be read
#define UBO_ALIGN_FALLBACK 256

// "nothing bound yet" for the per-pass state caches; no real value is ever this
#define UNBOUND 0xFFFFFFFFu

#define DEFAULT_MAX_DRAW_ITEMS 4096
#define DEFAULT_MAX_OBJECTS 512
#define DEFAULT_MAX_SKINNED_DRAWS 32
#define DEFAULT_CLEAR_COLOR linalg::Vec4{0.10f, 0.11f, 0.13f, 1.0f}

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    linalg::Vec4 dflt = DEFAULT_CLEAR_COLOR;
    linalg::Vec3 rgb =
        config::value_vec3(config::find(body, "clear_color"), linalg::vec3_from(dflt));
    return Config{
        .max_draw_items = (u32)config::get_i32(body, "max_draw_items", DEFAULT_MAX_DRAW_ITEMS),
        .max_objects = (u32)config::get_i32(body, "max_objects", DEFAULT_MAX_OBJECTS),
        .max_skinned_draws =
            (u32)config::get_i32(body, "max_skinned_draws", DEFAULT_MAX_SKINNED_DRAWS),
        .clear_color = linalg::vec4_from(rgb, 1.0f),
    };
}

// opaque sorts for state grouping, transparent back-to-front; see drawkey.h
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

// per submit_model, not per submesh
struct ObjectData {
    linalg::Mat4 world;
    linalg::Mat3 normal;
    u32 joint_offset; // byte offset into the joint UBO
    b32 skinned;
};

// GPU side of one mesh slot; epoch detects a recycled slot
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

    drawkey::Entry *pass[PASS_COUNT];
    u32 pass_count[PASS_COUNT];
    drawkey::Entry *sort_tmp;

    u32 frame_ubo;
    u32 joint_ubo;

    // staged on the CPU and uploaded once in flush, so submit_model issues no GL calls
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
        g.pass[p] = push_array(a, drawkey::Entry, cfg.max_draw_items);
    g.sort_tmp = push_array(a, drawkey::Entry, cfg.max_draw_items);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glFrontFace(GL_CCW); // glTF defines counter-clockwise as front-facing

    // GL 3.3 needs the bound range to cover the block's declared size, so every
    // slot costs the full MAX_JOINTS palette
    i32 align = UBO_ALIGN_FALLBACK;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    u32 slot_bytes = (u32)(sizeof(linalg::Mat4) * SHADER_MAX_JOINTS);
    g.joint_stride = (u32)ALIGN_UP_2(slot_bytes, (u64)align);

    // zeroed: only the first joint_count matrices of a slot are written per frame
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

    // depth writes must be on to clear depth; a transparent material may have left them off
    glDepthMask(GL_TRUE);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void setup_attribs(asset::VertexKind kind) {
    u32 stride = (u32)asset::vertex_stride(kind);

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

    // IPointer: joint indices must arrive in the shader as integers
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_SHORT, stride,
                           (const void *)offsetof(asset::VertexSkinned, joints));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(asset::VertexSkinned, weights));
}

// uploads on first sight or after the slot was recycled; NULL for an empty mesh
static GpuMesh *ensure_uploaded(asset::Manager *m, asset::MeshHandle h) {
    ASSERT_MSG(g.gpu, "render: attach_assets before drawing");

    u32 idx = asset::handle_index(h.v);
    u8 epoch = asset::handle_epoch(h.v);
    asset::Mesh *mesh = asset::get(m, h);
    if (mesh->vertex_count == 0)
        return NULL;

    // a stale handle resolved to the fallback; upload that under slot 0, not the stale slot
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

// what the geometry requires of a program, as opposed to what the material asks for
static u32 mesh_features(const asset::Mesh *mesh) {
    return mesh->vertex_kind == asset::VERTEX_SKINNED ? shader::FEATURE_SKINNED
                                                      : shader::FEATURE_NONE;
}

static u32 material_features(const asset::Material *mat) {
    return mat->alpha_mode == asset::ALPHA_MASK ? shader::FEATURE_ALPHA_MASK : shader::FEATURE_NONE;
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

    // import sits between world placement and native space, so it applies to normals too
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
            memcpy(g.joint_staging + o->joint_offset, palette, sizeof(linalg::Mat4) * joint_count);
        } else {
            LOG_WARN_ONCE("render: more than %u skinned draws in a frame, the rest draw unskinned",
                          g.cfg.max_skinned_draws);
        }
    }

    // distance from the bounds centre, not the origin: a pivot at the feet would sort wrong
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

            // a palette that did not fit draws unskinned, not with the previous object's
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

            u64 key =
                pass == PASS_OPAQUE
                    ? drawkey::opaque(pid, gfx::state_key(it->state),
                                      asset::handle_index(model->meshes[i].v),
                                      asset::handle_index(sub->material.v), dist)
                    : drawkey::transparent(pid, asset::handle_index(model->meshes[i].v), dist);

            g.pass[pass][g.pass_count[pass]++] = drawkey::Entry{.key = key, .item = g.item_count};
            g.item_count++;
        }
    }
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

    drawkey::sort(g.pass[pass], g.sort_tmp, n);

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

            // uniform values live in the program object, so a program change invalidates both
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

            // buffer bindings are context state, so this survives a program change
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
