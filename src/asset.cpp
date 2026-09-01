#include "asset.h"

#include <string.h>

#include "anim.h"
#include "config.h"
#include "file.h"
#include "gltf.h"
#include "logger.h"
#include "path.h"

#define MATERIAL_SIDECAR_EXT ".mat.cfg"

#define ROOT_CAP 512
#define ASSET_DIR "assets"

namespace asset {

struct Slot {
    u8 epoch;
    b8 used;
    u8 scope;

    // A stale handle is usually dereferenced every frame by whatever is holding
    // it, so reporting it every time buries the first occurrence under
    // thousands of copies. One line per slot, per epoch.
    b8 warned;
};

// One implementation of the slot mechanics for all five asset types; only the
// element size differs, so a stride beats five copies of the epoch logic.
struct Pool {
    u8 *items;
    Slot *slots;
    u32 cap;
    u64 stride;
};

struct Manager {
    memory::Arena *mgr;
    memory::Arena *scratch;
    memory::Arena scopes[SCOPE_COUNT];

    char root[ROOT_CAP];
    shader::Handle default_shader;

    Pool models;
    Pool meshes;
    Pool materials;
    Pool skeletons;
    Pool clips;

    strmap::Map by_path; // rel_path -> ModelHandle.v
};

static void *pool_at(Pool *p, u32 index) { return p->items + (u64)index * p->stride; }

static u32 make_handle(u32 index, u8 epoch) { return ((u32)epoch << ASSET_EPOCH_SHIFT) | index; }

static Pool pool_create(memory::Arena *a, u32 cap, u64 stride) {
    ASSERT(cap > 0);
    Pool p = {
        .items = (u8 *)memory::push(a, (u64)cap * stride),
        .slots = push_array(a, Slot, cap),
        .cap = cap,
        .stride = stride,
    };
    p.slots[0].used = true; // slot 0 is the fallback and is never handed out
    return p;
}

// index 0 is reserved, so 0 doubles as the "pool full" answer
static u32 pool_alloc(Pool *p, Scope s) {
    for (u32 i = 1; i < p->cap; i++) {
        if (!p->slots[i].used) {
            p->slots[i].used = true;
            p->slots[i].scope = (u8)s;
            p->slots[i].warned = false;
            memset(pool_at(p, i), 0, p->stride);
            return i;
        }
    }
    return 0;
}

static void pool_release(Pool *p, u32 index) {
    if (index == 0 || index >= p->cap || !p->slots[index].used)
        return;
    p->slots[index].used = false;
    p->slots[index].epoch++; // wraps at 256; stale handles collide only after that many reuses
}

static void pool_release_scope(Pool *p, Scope s) {
    for (u32 i = 1; i < p->cap; i++)
        if (p->slots[i].used && p->slots[i].scope == (u8)s)
            pool_release(p, i);
}

static void *resolve(Pool *p, u32 v, const char *what) {
    u32 idx = handle_index(v);
    if (idx < p->cap && p->slots[idx].used && p->slots[idx].epoch == handle_epoch(v))
        return pool_at(p, idx);

    // handle 0 is the fallback by design and says nothing; anything else is a
    // bug worth one line, not one line per frame for as long as it is held
    if (v != 0 && idx < p->cap && !p->slots[idx].warned) {
        p->slots[idx].warned = true;
        LOG_WARN("asset: stale %s handle %u, substituting fallback", what, v);
    }
    return pool_at(p, 0);
}

static const linalg::Vec3 CUBE_POS[8] = {
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},
};

static const u32 CUBE_IDX[36] = {
    4, 5, 6, 4, 6, 7, // front
    1, 0, 3, 1, 3, 2, // back
    0, 4, 7, 0, 7, 3, // left
    5, 1, 2, 5, 2, 6, // right
    3, 7, 6, 3, 6, 2, // top
    0, 1, 5, 0, 5, 4, // bottom
};

// Slot 0 of every pool: a magenta cube, so a missing or stale asset is loud in
// the world instead of a crash or a silent nothing.
static void build_fallbacks(Manager *m) {
    memory::Arena *a = m->mgr;

    Material *mat = (Material *)pool_at(&m->materials, 0);
    snprintf(mat->name, ASSET_NAME_CAP, "<fallback>");
    mat->shader = m->default_shader;
    mat->base_color = linalg::Vec4{1.0f, 0.0f, 1.0f, 1.0f};
    mat->roughness = 1.0f;
    mat->alpha_mode = ALPHA_OPAQUE;
    mat->alpha_cutoff = 0.5f;
    mat->state = gfx::state_default();

    Mesh *mesh = (Mesh *)pool_at(&m->meshes, 0);
    mesh->vertex_kind = VERTEX_STATIC;
    mesh->vertices.statics = push_array(a, VertexStatic, ARRAY_COUNT(CUBE_POS));
    mesh->vertex_count = ARRAY_COUNT(CUBE_POS);
    for (u32 i = 0; i < mesh->vertex_count; i++) {
        mesh->vertices.statics[i].pos = CUBE_POS[i];
        mesh->vertices.statics[i].normal = linalg::vec3_normalize(CUBE_POS[i]);
    }
    mesh->indices = push_array(a, u32, ARRAY_COUNT(CUBE_IDX));
    mesh->index_count = ARRAY_COUNT(CUBE_IDX);
    memcpy(mesh->indices, CUBE_IDX, sizeof(CUBE_IDX));
    mesh->submeshes = push_array(a, Submesh, 1);
    mesh->submesh_count = 1;
    mesh->submeshes[0] = Submesh{.index_count = mesh->index_count, .material = MATERIAL_FALLBACK};

    Skeleton *skel = (Skeleton *)pool_at(&m->skeletons, 0);
    skel->joints = push_array(a, Joint, 1);
    skel->joint_count = 1;
    skel->root = linalg::mat4_identity();
    skel->joints[0].parent = -1;
    skel->joints[0].inverse_bind = linalg::mat4_identity();
    skel->joints[0].bind_r = linalg::quat_identity();
    skel->joints[0].bind_s = linalg::Vec3{1.0f, 1.0f, 1.0f};
    snprintf(skel->joints[0].name, ASSET_NAME_CAP, "<root>");
    skel->by_name = strmap::create(a, 1);
    strmap::insert(&skel->by_name, skel->joints[0].name, 0);

    Clip *clip = (Clip *)pool_at(&m->clips, 0);
    snprintf(clip->name, ASSET_NAME_CAP, "<fallback>");

    Model *model = (Model *)pool_at(&m->models, 0);
    snprintf(model->name, ASSET_NAME_CAP, "<fallback>");
    model->path = "<fallback>";
    model->import = linalg::mat4_identity();
    model->bounds_min = linalg::Vec3{-0.5f, -0.5f, -0.5f};
    model->bounds_max = linalg::Vec3{0.5f, 0.5f, 0.5f};
    model->meshes = push_array(a, MeshHandle, 1);
    model->mesh_count = 1;
    model->meshes[0] = MESH_FALLBACK;
    model->skeleton = SKELETON_FALLBACK;
}

ManagerResult init(memory::Arena *mgr_arena, memory::Arena *scratch, Config cfg) {
    ASSERT(mgr_arena && scratch);

    if (!cfg.root) {
        LOG_ERR("asset: init needs a root directory");
        return ManagerResult{.err = result::ERR_INVALID};
    }

    Manager *m = push_struct(mgr_arena, Manager);
    m->mgr = mgr_arena;
    m->scratch = scratch;
    m->default_shader = cfg.default_shader;

    i32 root_len = snprintf(m->root, ROOT_CAP, "%s", cfg.root);
    if (root_len < 0 || root_len >= ROOT_CAP) {
        LOG_ERR("asset: root path longer than %d bytes: %s", ROOT_CAP, cfg.root);
        return ManagerResult{.err = result::ERR_INVALID};
    }

    for (u32 i = 0; i < SCOPE_COUNT; i++)
        m->scopes[i] = memory::create(cfg.scope_bytes[i]);

    m->models = pool_create(mgr_arena, cfg.max_models, sizeof(Model));
    m->meshes = pool_create(mgr_arena, cfg.max_meshes, sizeof(Mesh));
    m->materials = pool_create(mgr_arena, cfg.max_materials, sizeof(Material));
    m->skeletons = pool_create(mgr_arena, cfg.max_skeletons, sizeof(Skeleton));
    m->clips = pool_create(mgr_arena, cfg.max_clips, sizeof(Clip));
    m->by_path = strmap::create(mgr_arena, cfg.max_models);

    build_fallbacks(m);

    LOG_INFO("asset: root %s", m->root);
    return ManagerResult{.val = m};
}

memory::Arena *scope_arena(Manager *m, Scope s) {
    ASSERT(m && s < SCOPE_COUNT);
    return &m->scopes[s];
}

memory::Arena *scratch_arena(Manager *m) {
    ASSERT(m);
    return m->scratch;
}

const char *root(Manager *m) {
    ASSERT(m);
    return m->root;
}

shader::Handle default_shader(Manager *m) {
    ASSERT(m);
    return m->default_shader;
}

static void grow(linalg::Vec3 *lo, linalg::Vec3 *hi, linalg::Vec3 p) {
    lo->x = MIN(lo->x, p.x);
    lo->y = MIN(lo->y, p.y);
    lo->z = MIN(lo->z, p.z);
    hi->x = MAX(hi->x, p.x);
    hi->y = MAX(hi->y, p.y);
    hi->z = MAX(hi->z, p.z);
}

// Walks every vertex once, at load. A skinned mesh is measured with the bind
// palette applied, because its stored positions are pre-skin: measuring them
// raw is how a 1.8 m soldier reads as 0.14 m tall.
static void compute_bounds(Manager *m, ModelHandle h) {
    Model *model = get(m, h);

    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    Skeleton *skel = get(m, model->skeleton);
    linalg::Mat4 *palette = NULL;
    if (model->skeleton.v != 0 && skel->joint_count > 0) {
        palette = push_array(m->scratch, linalg::Mat4, skel->joint_count);
        anim::bind_pose(m->scratch, skel, palette);
    }

    linalg::Vec3 lo{1e30f, 1e30f, 1e30f};
    linalg::Vec3 hi{-1e30f, -1e30f, -1e30f};
    b32 any = false;

    for (u32 i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = get(m, model->meshes[i]);

        for (u32 v = 0; v < mesh->vertex_count; v++) {
            linalg::Vec3 p = vertex_pos(mesh, v);
            linalg::Vec4 h4{p.x, p.y, p.z, 1.0f};

            if (palette && mesh->vertex_kind == VERTEX_SKINNED) {
                const VertexSkinned *sv = &mesh->vertices.skinned[v];
                linalg::Vec4 acc{};
                f32 wsum = 0.0f;
                for (u32 k = 0; k < 4; k++) {
                    f32 w = sv->weights[k];
                    if (w <= 0.0f || sv->joints[k] >= skel->joint_count)
                        continue;
                    wsum += w;
                    acc = acc + linalg::mat4_mul_vec4(palette[sv->joints[k]], h4) * w;
                }
                if (wsum > 1e-4f)
                    h4 = acc;
            }

            grow(&lo, &hi, linalg::Vec3{h4.x, h4.y, h4.z});
            any = true;
        }
    }

    memory::temp_end(t);

    if (!any) {
        LOG_WARN("asset: model \"%s\" has no vertices, bounds are empty", model->name);
        lo = linalg::Vec3{};
        hi = linalg::Vec3{};
    }

    model->bounds_min = lo;
    model->bounds_max = hi;
}

void model_bounds(Manager *m, ModelHandle h, linalg::Vec3 *out_min, linalg::Vec3 *out_max) {
    ASSERT(m && out_min && out_max);

    Model *model = get(m, h);
    linalg::Vec3 lo = model->bounds_min;
    linalg::Vec3 hi = model->bounds_max;

    // All eight corners, not the two extremes: import can rotate, and rotating
    // min and max alone produces a box that no longer contains the model.
    linalg::Vec3 out_lo{1e30f, 1e30f, 1e30f};
    linalg::Vec3 out_hi{-1e30f, -1e30f, -1e30f};

    for (u32 c = 0; c < 8; c++) {
        linalg::Vec4 corner{
            (c & 1) ? hi.x : lo.x,
            (c & 2) ? hi.y : lo.y,
            (c & 4) ? hi.z : lo.z,
            1.0f,
        };
        linalg::Vec4 t4 = linalg::mat4_mul_vec4(model->import, corner);
        grow(&out_lo, &out_hi, linalg::Vec3{t4.x, t4.y, t4.z});
    }

    *out_min = out_lo;
    *out_max = out_hi;
}

ModelResult create_model(Manager *m, Scope s, const char *name) {
    ASSERT(m);

    u32 idx = pool_alloc(&m->models, s);
    if (!idx)
        return ModelResult{.err = result::ERR_EXHAUSTED};

    Model *model = (Model *)pool_at(&m->models, idx);
    snprintf(model->name, ASSET_NAME_CAP, "%s", name ? name : "<unnamed>");
    model->skeleton = SKELETON_FALLBACK;
    model->import = linalg::mat4_identity(); // zeroed memory would be degenerate

    return ModelResult{.val = ModelHandle{make_handle(idx, m->models.slots[idx].epoch)}};
}

MeshResult create_mesh(Manager *m, Scope s) {
    ASSERT(m);
    u32 idx = pool_alloc(&m->meshes, s);
    if (!idx)
        return MeshResult{.err = result::ERR_EXHAUSTED};
    return MeshResult{.val = MeshHandle{make_handle(idx, m->meshes.slots[idx].epoch)}};
}

MaterialResult create_material(Manager *m, Scope s) {
    ASSERT(m);
    u32 idx = pool_alloc(&m->materials, s);
    if (!idx)
        return MaterialResult{.err = result::ERR_EXHAUSTED};

    Material *mat = (Material *)pool_at(&m->materials, idx);
    mat->shader = m->default_shader;
    mat->alpha_cutoff = 0.5f;                 // the glTF default
    mat->state = gfx::state_default();        // zeroed would disable depth entirely

    return MaterialResult{.val = MaterialHandle{make_handle(idx, m->materials.slots[idx].epoch)}};
}

SkeletonResult create_skeleton(Manager *m, Scope s) {
    ASSERT(m);
    u32 idx = pool_alloc(&m->skeletons, s);
    if (!idx)
        return SkeletonResult{.err = result::ERR_EXHAUSTED};
    return SkeletonResult{.val = SkeletonHandle{make_handle(idx, m->skeletons.slots[idx].epoch)}};
}

ClipResult create_clip(Manager *m, Scope s) {
    ASSERT(m);
    u32 idx = pool_alloc(&m->clips, s);
    if (!idx)
        return ClipResult{.err = result::ERR_EXHAUSTED};
    return ClipResult{.val = ClipHandle{make_handle(idx, m->clips.slots[idx].epoch)}};
}

linalg::Mat4 import_matrix(ImportOptions opts) {
    f32 scale = opts.scale > 0.0f ? opts.scale : 1.0f;

    // -90 degrees about X sends (x, y, z) to (x, z, -y), i.e. Z-up to Y-up
    linalg::Quat r = opts.z_up ? linalg::Quat{-0.70710678f, 0.0f, 0.0f, 0.70710678f}
                               : linalg::quat_identity();

    return linalg::mat4_from_trs(linalg::Vec3{}, r, linalg::Vec3{scale, scale, scale});
}

static config::Node *material_block(config::Node *first, const char *name) {
    for (config::Node *n = config::find(first, "material"); n; n = config::find_next(n, "material")) {
        const char *who = config::value(n, 0, NULL);
        if (who && strcmp(who, name) == 0)
            return n;
    }
    return NULL;
}

static void apply_override(Material *mat, config::Node *decl) {
    config::Node *body = decl->first_child;

    const char *shader_name = config::value(config::find(body, "shader"), 0, NULL);
    if (shader_name) {
        // find() already reports an unknown name; keeping the current handle is
        // the only sane recovery, and it is a content error either way
        shader::HandleResult h = shader::find(shader_name);
        if (h.err == result::OK)
            mat->shader = h.val;
    }

    const char *cull = config::value(config::find(body, "cull"), 0, NULL);
    if (cull && !gfx::cull_from_name(cull, &mat->state.cull))
        LOG_WARN("asset: material \"%s\" has unknown cull mode '%s'", mat->name, cull);

    const char *blend = config::value(config::find(body, "blend"), 0, NULL);
    if (blend && !gfx::blend_from_name(blend, &mat->state.blend))
        LOG_WARN("asset: material \"%s\" has unknown blend mode '%s'", mat->name, blend);

    mat->state.depth_test =
        (b8)config::value_b32(config::find(body, "depth_test"), 0, mat->state.depth_test);
    mat->state.depth_write =
        (b8)config::value_b32(config::find(body, "depth_write"), 0, mat->state.depth_write);

    config::Node *color = config::find(body, "base_color");
    if (color) {
        mat->base_color = linalg::Vec4{
            config::value_f32(color, 0, mat->base_color.x),
            config::value_f32(color, 1, mat->base_color.y),
            config::value_f32(color, 2, mat->base_color.z),
            config::value_f32(color, 3, mat->base_color.w),
        };
    }

    mat->metallic = config::value_f32(config::find(body, "metallic"), 0, mat->metallic);
    mat->roughness = config::value_f32(config::find(body, "roughness"), 0, mat->roughness);
    mat->alpha_cutoff = config::value_f32(config::find(body, "alpha_cutoff"), 0, mat->alpha_cutoff);
}

// A sidecar beside the model -- characters/soldier.glb -> soldier.mat.cfg --
// overriding what the glTF said. Absent is the normal case and says nothing.
// This is the editor's channel into a material: the .glb is the author's
// intent, this file is the user's.
static void apply_material_sidecar(Manager *m, ModelHandle h, const char *rel_path) {
    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    const char *rel_cfg = path::swap_ext(m->scratch, rel_path, MATERIAL_SIDECAR_EXT);
    const char *abs = FPATH(m->scratch, m->root, ASSET_DIR, rel_cfg);
    if (!file::exists(abs)) {
        memory::temp_end(t);
        return;
    }

    config::DocResult doc = config::load(m->scratch, abs);
    if (doc.err != result::OK) {
        memory::temp_end(t);
        return; // load already logged why
    }

    Model *model = get(m, h);
    u32 applied = 0;

    // Reached through the submeshes because that is where materials are
    // referenced; one shared by several submeshes is written more than once,
    // which is harmless and cheaper than building a unique set.
    for (u32 i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = get(m, model->meshes[i]);
        for (u32 sm = 0; sm < mesh->submesh_count; sm++) {
            Material *mat = get(m, mesh->submeshes[sm].material);
            config::Node *decl = material_block(doc.val->first, mat->name);
            if (!decl)
                continue;
            apply_override(mat, decl);
            applied++;
        }
    }

    LOG_INFO("asset: %s applied to %u submesh material(s)", rel_cfg, applied);
    memory::temp_end(t);
}

ModelResult load_model(Manager *m, Scope s, const char *rel_path, ImportOptions opts) {
    ASSERT(m && rel_path);

    // dedup is on path only: a second load with different options returns the
    // first import rather than a second copy
    u32 *existing = strmap::find(&m->by_path, rel_path);
    if (existing)
        return ModelResult{.val = ModelHandle{*existing}};

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    const char *abs = FPATH(m->scratch, m->root, ASSET_DIR, rel_path);
    ModelResult r = gltf::load_model(m, s, abs, rel_path);
    memory::temp_end(t);

    if (r.err != result::OK)
        return r;

    // the dedup key has to outlive its entry, so it lives in the scope arena
    // beside the model and dies with it
    memory::Arena *out = scope_arena(m, s);
    u64 len = strlen(rel_path);
    char *stored = push_array(out, char, len + 1);
    memcpy(stored, rel_path, len + 1);

    Model *model = get(m, r.val);
    model->path = stored;
    model->import = import_matrix(opts);
    strmap::insert(&m->by_path, stored, r.val.v);

    // after the model exists and before anyone can draw it: overrides can move
    // a material between passes, and bounds are what the camera and the
    // transparent sort both read
    apply_material_sidecar(m, r.val, rel_path);
    compute_bounds(m, r.val);

    return r;
}

ClipResult load_clip(Manager *m, Scope s, const char *rel_path, SkeletonHandle target,
                     ClipOptions opts) {
    ASSERT(m && rel_path);

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    const char *abs = FPATH(m->scratch, m->root, ASSET_DIR, rel_path);
    ClipResult r = gltf::load_clip(m, s, abs, target, opts);
    memory::temp_end(t);

    return r;
}

Model *get(Manager *m, ModelHandle h) { return (Model *)resolve(&m->models, h.v, "model"); }
Mesh *get(Manager *m, MeshHandle h) { return (Mesh *)resolve(&m->meshes, h.v, "mesh"); }
Material *get(Manager *m, MaterialHandle h) {
    return (Material *)resolve(&m->materials, h.v, "material");
}
Skeleton *get(Manager *m, SkeletonHandle h) {
    return (Skeleton *)resolve(&m->skeletons, h.v, "skeleton");
}
Clip *get(Manager *m, ClipHandle h) { return (Clip *)resolve(&m->clips, h.v, "clip"); }

void remove(Manager *m, ModelHandle h) {
    ASSERT(m);

    u32 idx = handle_index(h.v);
    if (idx == 0 || idx >= m->models.cap || !m->models.slots[idx].used ||
        m->models.slots[idx].epoch != handle_epoch(h.v))
        return;

    Model *model = (Model *)pool_at(&m->models, idx);
    if (model->path)
        strmap::remove(&m->by_path, model->path);

    // catalog-only: the bytes stay in the scope arena until unload_scope, which
    // is what makes undo in an editor cheap
    pool_release(&m->models, idx);
}

void unload_scope(Manager *m, Scope s) {
    ASSERT(m && s < SCOPE_COUNT);

    // map keys point into the arena we are about to reset, so drop them first
    for (u32 i = 1; i < m->models.cap; i++) {
        if (!m->models.slots[i].used || m->models.slots[i].scope != (u8)s)
            continue;
        Model *model = (Model *)pool_at(&m->models, i);
        if (model->path)
            strmap::remove(&m->by_path, model->path);
    }

    pool_release_scope(&m->models, s);
    pool_release_scope(&m->meshes, s);
    pool_release_scope(&m->materials, s);
    pool_release_scope(&m->skeletons, s);
    pool_release_scope(&m->clips, s);

    memory::reset(&m->scopes[s]);
    LOG_INFO("asset: unloaded scope %u", (u32)s);
}

} // namespace asset
