#include "render.h"

#include <glad/glad.h>

#include "file.h"
#include "logger.h"
#include "path.h"

#define SHADER_DIR "assets/shaders"

namespace render {

struct Shader {
    u32 program;
    i32 u_mvp;
    i32 u_model;
    i32 u_base_color;
};

static Shader g_shaders[SHADER_COUNT];
static u32 g_joint_ubo;

#define JOINT_BINDING 0

// both programs share one fragment stage; only the vertex stage differs
struct ShaderDef {
    const char *vert;
    const char *frag;
};

static const ShaderDef SHADER_DEFS[SHADER_COUNT] = {
    {"static", "static"},
    {"skinned", "static"},
};

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

static u32 compile_stage(u32 type, const char *src, const char *label) {
    u32 id = glCreateShader(type);
    glShaderSource(id, 1, &src, NULL);
    glCompileShader(id);

    i32 ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[2048];
        glGetShaderInfoLog(id, sizeof(info), NULL, info);
        LOG_ERR("shader: %s failed to compile\n%s", label, info);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static result::Error load_program(memory::Arena *scratch, const char *root, ShaderDef def,
                                  Shader *out) {
    memory::ArenaTemp t = memory::temp_begin(scratch);

    char vert_name[64];
    char frag_name[64];
    snprintf(vert_name, sizeof(vert_name), "%s.vert", def.vert);
    snprintf(frag_name, sizeof(frag_name), "%s.frag", def.frag);

    file::BlobResult vs = file::read(scratch, FPATH(scratch, root, SHADER_DIR, vert_name));
    file::BlobResult fs = file::read(scratch, FPATH(scratch, root, SHADER_DIR, frag_name));
    if (vs.err != result::OK || fs.err != result::OK) {
        memory::temp_end(t);
        return result::ERR_NOT_FOUND;
    }

    u32 v = compile_stage(GL_VERTEX_SHADER, (const char *)vs.val.data, vert_name);
    u32 f = compile_stage(GL_FRAGMENT_SHADER, (const char *)fs.val.data, frag_name);
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    u32 prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);

    // the stages are reference-counted by the program, so drop ours either way
    glDeleteShader(v);
    glDeleteShader(f);

    i32 ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[2048];
        glGetProgramInfoLog(prog, sizeof(info), NULL, info);
        LOG_ERR("shader: %s failed to link\n%s", def.vert, info);
        glDeleteProgram(prog);
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    out->program = prog;
    out->u_mvp = glGetUniformLocation(prog, "u_mvp");
    out->u_model = glGetUniformLocation(prog, "u_model");
    out->u_base_color = glGetUniformLocation(prog, "u_base_color");

    u32 block = glGetUniformBlockIndex(prog, "Joints");
    if (block != GL_INVALID_INDEX)
        glUniformBlockBinding(prog, block, JOINT_BINDING);

    memory::temp_end(t);
    LOG_INFO("shader: linked %s (%s.vert + %s.frag)", def.vert, def.vert, def.frag);
    return result::OK;
}

result::Error load_shaders(memory::Arena *scratch, const char *root) {
    for (u32 i = 0; i < SHADER_COUNT; i++) {
        result::Error e = load_program(scratch, root, SHADER_DEFS[i], &g_shaders[i]);
        if (e != result::OK)
            return e;
    }

    glGenBuffers(1, &g_joint_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_joint_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(linalg::Mat4) * RENDER_MAX_JOINTS, NULL,
                 GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, JOINT_BINDING, g_joint_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    return result::OK;
}

void set_joint_palette(const linalg::Mat4 *joints, u32 count) {
    ASSERT(joints);

    if (count > RENDER_MAX_JOINTS) {
        LOG_WARN("render: %u joints exceeds the %d the palette holds", count, RENDER_MAX_JOINTS);
        count = RENDER_MAX_JOINTS;
    }

    glBindBuffer(GL_UNIFORM_BUFFER, g_joint_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(sizeof(linalg::Mat4) * count), joints);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

static void setup_attribs(asset::VertexKind kind) {
    u32 stride = (u32)asset::vertex_stride(kind);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void *)24);

    if (kind != asset::VERTEX_SKINNED)
        return;

    // glVertexAttribIPointer, not the float-converting variant: joint indices
    // are integers and must arrive in the shader as integers
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_SHORT, stride, (const void *)32);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void *)40);
}

static void upload_mesh(asset::Mesh *mesh) {
    if (mesh->gpu_vao || mesh->vertex_count == 0)
        return;

    const void *verts = mesh->vertex_kind == asset::VERTEX_SKINNED
                            ? (const void *)mesh->vertices.skinned
                            : (const void *)mesh->vertices.statics;
    u64 vert_bytes = asset::vertex_stride(mesh->vertex_kind) * mesh->vertex_count;

    glGenVertexArrays(1, &mesh->gpu_vao);
    glBindVertexArray(mesh->gpu_vao);

    glGenBuffers(1, &mesh->gpu_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->gpu_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vert_bytes, verts, GL_STATIC_DRAW);

    glGenBuffers(1, &mesh->gpu_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->gpu_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(mesh->index_count * sizeof(u32)),
                 mesh->indices, GL_STATIC_DRAW);

    setup_attribs(mesh->vertex_kind);

    glBindVertexArray(0);
}

void upload_model(asset::Manager *m, asset::ModelHandle h) {
    asset::Model *model = asset::get(m, h);
    for (u32 i = 0; i < model->mesh_count; i++)
        upload_mesh(asset::get(m, model->meshes[i]));
}

void draw_model(asset::Manager *m, asset::ModelHandle h, linalg::Mat4 model_mat,
                linalg::Mat4 view_proj) {
    asset::Model *model = asset::get(m, h);

    // the import transform sits between world placement and the asset's own
    // native space, so it applies to normals as well as positions
    linalg::Mat4 world = model_mat * model->import;
    linalg::Mat4 mvp = view_proj * world;

    for (u32 i = 0; i < model->mesh_count; i++) {
        asset::Mesh *mesh = asset::get(m, model->meshes[i]);
        if (!mesh->gpu_vao)
            continue;

        // the vertex layout decides the program, so it is picked per mesh
        Shader *sh = &g_shaders[mesh->vertex_kind == asset::VERTEX_SKINNED ? SHADER_SKINNED
                                                                           : SHADER_STATIC];
        if (!sh->program)
            continue;

        glUseProgram(sh->program);
        glUniformMatrix4fv(sh->u_mvp, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(sh->u_model, 1, GL_FALSE, world.m);

        glBindVertexArray(mesh->gpu_vao);

        for (u32 s = 0; s < mesh->submesh_count; s++) {
            const asset::Submesh *sub = &mesh->submeshes[s];
            asset::Material *mat = asset::get(m, sub->material);

            glUniform4fv(sh->u_base_color, 1, &mat->base_color.x);
            glDrawElements(GL_TRIANGLES, (i32)sub->index_count, GL_UNSIGNED_INT,
                           (const void *)((u64)sub->index_offset * sizeof(u32)));
        }
    }

    glBindVertexArray(0);
}

} // namespace render
