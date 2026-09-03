#include "asset/asset.h"

#include <string.h>

#include "asset/anim.h"
#include "asset/gltf.h"
#include "core/config.h"
#include "core/logger.h"
#include "platform/file.h"
#include "platform/path.h"

#define MATERIAL_SIDECAR_EXT ".mat.cfg"
#define IMPORT_SIDECAR_EXT ".import.cfg"

#define ROOT_CAP 512

// glTF's default alphaCutoff, for materials created outside a file
#define GLTF_DEFAULT_ALPHA_CUTOFF 0.5f

// weights summing below this measure as unskinned rather than collapsing to the origin
#define SKIN_WEIGHT_EPSILON 1e-4f

// Starting extents for a bounds walk: anything real is inside them
#define BOUNDS_EMPTY 1e30f

#define DEFAULT_MAX_MODELS 64
#define DEFAULT_MAX_MESHES 256
#define DEFAULT_MAX_MATERIALS 256
#define DEFAULT_MAX_SKELETONS 32
#define DEFAULT_MAX_CLIPS 128
#define DEFAULT_SCOPE_MIB 128

namespace asset {

Config config_from(config::Node *block, const char *root, shader::Handle default_shader) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .root = root,
        .default_shader = default_shader,
        .max_models = (u32)config::get_i32(body, "max_models", DEFAULT_MAX_MODELS),
        .max_meshes = (u32)config::get_i32(body, "max_meshes", DEFAULT_MAX_MESHES),
        .max_materials = (u32)config::get_i32(body, "max_materials", DEFAULT_MAX_MATERIALS),
        .max_skeletons = (u32)config::get_i32(body, "max_skeletons", DEFAULT_MAX_SKELETONS),
        .max_clips = (u32)config::get_i32(body, "max_clips", DEFAULT_MAX_CLIPS),
        .scope_bytes =
            {
                MiB(config::get_i32(body, "persistent", DEFAULT_SCOPE_MIB)),
                MiB(config::get_i32(body, "transient", DEFAULT_SCOPE_MIB)),
            },
    };
}

enum Kind { KIND_MODEL, KIND_MESH, KIND_MATERIAL, KIND_SKELETON, KIND_CLIP, KIND_COUNT };

static const u64 KIND_STRIDE[KIND_COUNT] = {
    sizeof(Model), sizeof(Mesh), sizeof(Material), sizeof(Skeleton), sizeof(Clip),
};

static const char *KIND_NAME[KIND_COUNT] = {"model", "mesh", "material", "skeleton", "clip"};

struct Manager {
    memory::Arena *mgr;
    memory::Arena *scratch;
    memory::Arena scopes[SCOPE_COUNT];

    char root[ROOT_CAP];
    shader::Handle default_shader;

    pool::Pool pools[KIND_COUNT];

    MeshReleaseFn mesh_release;
    void *mesh_release_user;

    // per scope, keyed by strings in that scope's arena, so unload is a clear()
    strmap::Map by_path[SCOPE_COUNT];      // rel_path -> ModelHandle.v
    strmap::Map clips_by_key[SCOPE_COUNT]; // "<path>@<skeleton>" -> ClipHandle.v
    strmap::Map failed[SCOPE_COUNT];       // rel_path -> result::Error of the first attempt
};

// A fresh slot as a packed handle, or 0 when the pool is full.
static u32 claim(Manager *m, Kind k, Scope s) {
    u32 idx = pool::alloc(&m->pools[k], (u8)s);
    return idx ? pool::make_handle(&m->pools[k], idx) : 0;
}

static void *payload(Manager *m, Kind k, u32 handle) {
    return pool::at(&m->pools[k], handle_index(handle));
}

static const char *abs_path(Manager *m, const char *rel) { return FPATH(m->scratch, m->root, rel); }

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

// slot 0 of every pool: a magenta cube, so a missing asset is loud rather than a crash
static void build_fallbacks(Manager *m) {
    memory::Arena *a = m->mgr;

    Material *mat = (Material *)payload(m, KIND_MATERIAL, 0);
    snprintf(mat->name, ASSET_NAME_CAP, "<fallback>");
    mat->shader = m->default_shader;
    mat->base_color = linalg::Vec4{1.0f, 0.0f, 1.0f, 1.0f};
    mat->roughness = 1.0f;
    mat->alpha_mode = ALPHA_OPAQUE;
    mat->alpha_cutoff = GLTF_DEFAULT_ALPHA_CUTOFF;
    mat->state = gfx::state_default();

    Mesh *mesh = (Mesh *)payload(m, KIND_MESH, 0);
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

    Skeleton *skel = (Skeleton *)payload(m, KIND_SKELETON, 0);
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

    Clip *clip = (Clip *)payload(m, KIND_CLIP, 0);
    snprintf(clip->name, ASSET_NAME_CAP, "<fallback>");

    Model *model = (Model *)payload(m, KIND_MODEL, 0);
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

    u32 caps[KIND_COUNT] = {
        cfg.max_models, cfg.max_meshes, cfg.max_materials, cfg.max_skeletons, cfg.max_clips,
    };
    for (u32 k = 0; k < KIND_COUNT; k++)
        m->pools[k] = pool::create(mgr_arena, caps[k], KIND_STRIDE[k]);

    for (u32 s = 0; s < SCOPE_COUNT; s++) {
        m->by_path[s] = strmap::create(mgr_arena, cfg.max_models);
        m->clips_by_key[s] = strmap::create(mgr_arena, cfg.max_clips);
        m->failed[s] = strmap::create(mgr_arena, cfg.max_models);
    }

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

// skinned meshes are measured in the bind pose: stored positions are pre-skin,
// and raw a 1.8 m soldier reads as 0.14 m
static void compute_bounds(Manager *m, ModelHandle h) {
    Model *model = get(m, h);

    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    Skeleton *skel = get(m, model->skeleton);
    linalg::Mat4 *palette = NULL;
    if (model->skeleton.v != 0 && skel->joint_count > 0) {
        palette = push_array(m->scratch, linalg::Mat4, skel->joint_count);
        anim::bind_pose(m->scratch, skel, palette);
    }

    linalg::Vec3 lo{BOUNDS_EMPTY, BOUNDS_EMPTY, BOUNDS_EMPTY};
    linalg::Vec3 hi{-BOUNDS_EMPTY, -BOUNDS_EMPTY, -BOUNDS_EMPTY};
    b32 any = false;

    for (u32 i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = get(m, model->meshes[i]);

        for (u32 v = 0; v < mesh->vertex_count; v++) {
            linalg::Vec4 h4 = linalg::vec4_from(vertex_pos(mesh, v), 1.0f);

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
                if (wsum > SKIN_WEIGHT_EPSILON)
                    h4 = acc;
            }

            grow(&lo, &hi, linalg::vec3_from(h4));
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

    // all eight corners: import can rotate, and rotating min/max alone gives a box that misses the
    // model
    linalg::Vec3 out_lo{BOUNDS_EMPTY, BOUNDS_EMPTY, BOUNDS_EMPTY};
    linalg::Vec3 out_hi{-BOUNDS_EMPTY, -BOUNDS_EMPTY, -BOUNDS_EMPTY};

    for (u32 c = 0; c < 8; c++) {
        linalg::Vec3 corner{
            (c & 1) ? hi.x : lo.x,
            (c & 2) ? hi.y : lo.y,
            (c & 4) ? hi.z : lo.z,
        };
        grow(&out_lo, &out_hi, linalg::mat4_mul_point(model->import, corner));
    }

    *out_min = out_lo;
    *out_max = out_hi;
}

ModelResult create_model(Manager *m, Scope s, const char *name) {
    ASSERT(m);

    u32 v = claim(m, KIND_MODEL, s);
    if (!v)
        return ModelResult{.err = result::ERR_EXHAUSTED};

    Model *model = (Model *)payload(m, KIND_MODEL, v);
    snprintf(model->name, ASSET_NAME_CAP, "%s", name ? name : "<unnamed>");
    model->skeleton = SKELETON_FALLBACK;
    model->import = linalg::mat4_identity(); // zeroed memory would be degenerate

    return ModelResult{.val = ModelHandle{v}};
}

MeshResult create_mesh(Manager *m, Scope s) {
    ASSERT(m);
    u32 v = claim(m, KIND_MESH, s);
    return v ? MeshResult{.val = MeshHandle{v}} : MeshResult{.err = result::ERR_EXHAUSTED};
}

MaterialResult create_material(Manager *m, Scope s) {
    ASSERT(m);

    u32 v = claim(m, KIND_MATERIAL, s);
    if (!v)
        return MaterialResult{.err = result::ERR_EXHAUSTED};

    Material *mat = (Material *)payload(m, KIND_MATERIAL, v);
    mat->shader = m->default_shader;
    mat->alpha_cutoff = GLTF_DEFAULT_ALPHA_CUTOFF;
    mat->state = gfx::state_default(); // zeroed would disable depth entirely

    return MaterialResult{.val = MaterialHandle{v}};
}

SkeletonResult create_skeleton(Manager *m, Scope s) {
    ASSERT(m);
    u32 v = claim(m, KIND_SKELETON, s);
    return v ? SkeletonResult{.val = SkeletonHandle{v}}
             : SkeletonResult{.err = result::ERR_EXHAUSTED};
}

ClipResult create_clip(Manager *m, Scope s) {
    ASSERT(m);
    u32 v = claim(m, KIND_CLIP, s);
    return v ? ClipResult{.val = ClipHandle{v}} : ClipResult{.err = result::ERR_EXHAUSTED};
}

linalg::Mat4 import_matrix(ImportOptions opts) {
    // zeroed (no sidecar) means as authored
    f32 scale = opts.scale > 0.0f ? opts.scale : 1.0f;

    // -90 degrees about X sends (x, y, z) to (x, z, -y), i.e. Z-up to Y-up
    linalg::Quat r = opts.z_up ? linalg::quat_from_euler(linalg::Vec3{-90.0f, 0.0f, 0.0f})
                               : linalg::quat_identity();

    return linalg::mat4_from_trs(linalg::Vec3{}, r, linalg::Vec3{scale, scale, scale});
}

static config::Node *material_block(config::Node *first, const char *name) {
    for (config::Node *n = config::find(first, "material"); n;
         n = config::find_next(n, "material")) {
        const char *who = config::value(n, 0, NULL);
        if (who && strcmp(who, name) == 0)
            return n;
    }
    return NULL;
}

static void apply_override(Material *mat, config::Node *decl) {
    config::Node *body = decl->first_child;

    const char *shader_name = config::get(body, "shader", NULL);
    if (shader_name) {
        // find() already reported; keep the current handle
        shader::HandleResult h = shader::find(shader_name);
        if (h.err == result::OK)
            mat->shader = h.val;
    }

    const char *cull = config::get(body, "cull", NULL);
    if (cull && !gfx::cull_from_name(cull, &mat->state.cull))
        LOG_WARN("asset: material \"%s\" has unknown cull mode '%s'", mat->name, cull);

    const char *blend = config::get(body, "blend", NULL);
    if (blend && !gfx::blend_from_name(blend, &mat->state.blend))
        LOG_WARN("asset: material \"%s\" has unknown blend mode '%s'", mat->name, blend);

    mat->state.depth_test = (b8)config::get_b32(body, "depth_test", mat->state.depth_test);
    mat->state.depth_write = (b8)config::get_b32(body, "depth_write", mat->state.depth_write);
    mat->base_color = config::value_vec4(config::find(body, "base_color"), mat->base_color);
    mat->metallic = config::get_f32(body, "metallic", mat->metallic);
    mat->roughness = config::get_f32(body, "roughness", mat->roughness);
    mat->alpha_cutoff = config::get_f32(body, "alpha_cutoff", mat->alpha_cutoff);
}

// the config beside an asset (soldier.glb -> soldier<ext>), or NULL when absent
// or broken; the caller owns the temp scope
static config::Doc *load_sidecar(Manager *m, const char *rel_path, const char *ext) {
    const char *abs = abs_path(m, path::swap_ext(m->scratch, rel_path, ext));
    if (!file::exists(abs))
        return NULL;

    config::DocResult doc = config::load(m->scratch, abs);
    return doc.err == result::OK ? doc.val : NULL;
}

// the .glb is the author's intent; the .mat.cfg sidecar is the user's and the editor's
static void apply_material_sidecar(Manager *m, ModelHandle h, const char *rel_path) {
    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    config::Doc *doc = load_sidecar(m, rel_path, MATERIAL_SIDECAR_EXT);
    if (!doc) {
        memory::temp_end(t);
        return;
    }

    Model *model = get(m, h);
    u32 applied = 0;

    // via submeshes, where materials are referenced; a shared one is written twice, harmlessly
    for (u32 i = 0; i < model->mesh_count; i++) {
        Mesh *mesh = get(m, model->meshes[i]);
        for (u32 sm = 0; sm < mesh->submesh_count; sm++) {
            Material *mat = get(m, mesh->submeshes[sm].material);
            config::Node *decl = material_block(doc->first, mat->name);
            if (!decl)
                continue;
            apply_override(mat, decl);
            applied++;
        }
    }

    LOG_INFO("asset: material overrides for %s applied to %u submesh material(s)", rel_path,
             applied);
    memory::temp_end(t);
}

// One block of the .import.cfg sidecar, or NULL when there is no sidecar.
static config::Node *import_block(Manager *m, const char *rel_path, const char *block) {
    config::Doc *doc = load_sidecar(m, rel_path, IMPORT_SIDECAR_EXT);
    if (!doc)
        return NULL;

    config::Node *n = config::find(doc->first, block);
    if (!n)
        LOG_WARN("asset: the import sidecar of %s has no '%s' block", rel_path, block);
    return n;
}

ImportOptions import_options(Manager *m, const char *rel_path) {
    ASSERT(m && rel_path);

    ImportOptions opts{.scale = 1.0f, .z_up = false};

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    config::Node *n = import_block(m, rel_path, "import");
    if (n) {
        opts.scale = config::get_f32(n->first_child, "scale", opts.scale);
        opts.z_up = config::get_b32(n->first_child, "z_up", opts.z_up);
    }
    memory::temp_end(t);

    return opts;
}

ClipOptions clip_options(Manager *m, const char *rel_path) {
    ASSERT(m && rel_path);

    ClipOptions opts{};

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    config::Node *n = import_block(m, rel_path, "clip");
    if (n)
        opts.rotation_only = config::get_b32(n->first_child, "rotation_only", opts.rotation_only);
    memory::temp_end(t);

    return opts;
}

b32 exists(Manager *m, const char *rel_path) {
    ASSERT(m && rel_path);
    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    b32 found = file::exists(abs_path(m, rel_path));
    memory::temp_end(t);
    return found;
}

b32 has_import_sidecar(Manager *m, const char *rel_path) {
    ASSERT(m && rel_path);
    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    b32 found = file::exists(abs_path(m, path::swap_ext(m->scratch, rel_path, IMPORT_SIDECAR_EXT)));
    memory::temp_end(t);
    return found;
}

// a block with one key per setting, each carrying its meaning as a trailing comment
static config::Node *sidecar_block(memory::Arena *a, config::Doc *doc, const char *key,
                                   const char *comment) {
    config::Node *block = config::append(a, doc, NULL, key, NULL, 0);
    block->is_block = true;
    block->comment = comment;
    return block;
}

static void sidecar_key(memory::Arena *a, config::Doc *doc, config::Node *block, const char *key,
                        const char *value, const char *comment) {
    const char *vals[] = {value};
    config::Node *n = config::append(a, doc, block, key, vals, 1);
    n->comment = comment;
}

static result::Error save_sidecar(Manager *m, const char *rel_path, config::Doc *doc,
                                  const char *abs) {
    result::Error e = config::save(m->scratch, doc, abs);
    if (e == result::OK)
        LOG_INFO("asset: wrote %s", abs);
    else
        LOG_ERR("asset: cannot write the import sidecar of %s: %s", rel_path, result::str(e));
    return e;
}

result::Error write_import_options(Manager *m, const char *rel_path, ImportOptions opts) {
    ASSERT(m && rel_path);
    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    const char *abs = abs_path(m, path::swap_ext(m->scratch, rel_path, IMPORT_SIDECAR_EXT));
    config::Doc *doc = config::create(m->scratch, abs);
    config::Node *block =
        sidecar_block(m->scratch, doc, "import",
                      "how this file enters world space; the asset's, not the loader's");
    sidecar_key(m->scratch, doc, block, "scale", memory::push_fmt(m->scratch, "%g", (f64)opts.scale),
                "multiplier on the authored units");
    sidecar_key(m->scratch, doc, block, "z_up", opts.z_up ? "true" : "false",
                "authored Z-up: rotate into the Y-up world");

    result::Error e = save_sidecar(m, rel_path, doc, abs);
    memory::temp_end(t);
    return e;
}

result::Error write_clip_options(Manager *m, const char *rel_path, ClipOptions opts) {
    ASSERT(m && rel_path);
    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    const char *abs = abs_path(m, path::swap_ext(m->scratch, rel_path, IMPORT_SIDECAR_EXT));
    config::Doc *doc = config::create(m->scratch, abs);
    config::Node *block = sidecar_block(m->scratch, doc, "clip",
                                        "which rig this clip was made for is not in the file");
    sidecar_key(m->scratch, doc, block, "rotation_only", opts.rotation_only ? "true" : "false",
                "true when authored for another rig: drops its bone lengths, costs root motion");

    result::Error e = save_sidecar(m, rel_path, doc, abs);
    memory::temp_end(t);
    return e;
}

u32 apply_import_options(Manager *m, const char *rel_path, ImportOptions opts) {
    ASSERT(m && rel_path);

    u32 applied = 0;
    for (u32 s = 0; s < SCOPE_COUNT; s++) {
        u32 *h = strmap::find(&m->by_path[s], rel_path);
        if (!h)
            continue;
        get(m, ModelHandle{*h})->import = import_matrix(opts);
        applied++;
    }
    return applied;
}

u32 apply_clip_options(Manager *m, const char *rel_path, ClipOptions opts) {
    ASSERT(m && rel_path);

    // clips are keyed "<path>@<skeleton>"; every binding of the file takes the setting
    u64 len = strlen(rel_path);
    u32 applied = 0;
    pool::Pool *p = &m->pools[KIND_CLIP];
    for (u32 i = pool::first(p); i; i = pool::next(p, i)) {
        Clip *clip = (Clip *)pool::at(p, i);
        if (!clip->key || strncmp(clip->key, rel_path, len) != 0 || clip->key[len] != '@')
            continue;
        clip->rotation_only = opts.rotation_only;
        applied++;
    }
    return applied;
}

ModelResult load_model(Manager *m, Scope s, const char *rel_path) {
    return load_model_with(m, s, rel_path, import_options(m, rel_path));
}

ClipResult load_clip(Manager *m, Scope s, const char *rel_path, SkeletonHandle target) {
    return load_clip_with(m, s, rel_path, target, clip_options(m, rel_path));
}

ModelResult load_model_with(Manager *m, Scope s, const char *rel_path, ImportOptions opts) {
    ASSERT(m && rel_path && s < SCOPE_COUNT);

    // dedup on path only: a second load with other options gets the first import
    u32 *existing = strmap::find(&m->by_path[s], rel_path);
    if (existing)
        return ModelResult{.val = ModelHandle{*existing}};

    // a failed load fails again without re-reading or re-logging
    u32 *failed = strmap::find(&m->failed[s], rel_path);
    if (failed)
        return ModelResult{.err = (result::Error)*failed};

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    ModelResult r = gltf::load_model(m, s, abs_path(m, rel_path), rel_path);
    memory::temp_end(t);

    // the key must outlive its entry, so it lives in the scope arena
    memory::Arena *out = scope_arena(m, s);
    if (r.err != result::OK) {
        strmap::insert(&m->failed[s], memory::push_str(out, rel_path), (u32)r.err);
        return r;
    }

    const char *stored = memory::push_str(out, rel_path);
    Model *model = get(m, r.val);
    model->path = stored;
    model->import = import_matrix(opts);
    strmap::insert(&m->by_path[s], stored, r.val.v);

    // before anyone can draw it: overrides can move a material between passes,
    // and bounds feed the camera and the transparent sort
    apply_material_sidecar(m, r.val, rel_path);
    compute_bounds(m, r.val);

    return r;
}

ClipResult load_clip_with(Manager *m, Scope s, const char *rel_path, SkeletonHandle target,
                          ClipOptions opts) {
    ASSERT(m && rel_path && s < SCOPE_COUNT);

    memory::ArenaTemp t = memory::temp_begin(m->scratch);

    // keyed on the target too: one file bound to two skeletons is two clips
    const char *key = memory::push_fmt(m->scratch, "%s@%u", rel_path, target.v);
    u32 *existing = strmap::find(&m->clips_by_key[s], key);
    if (existing) {
        memory::temp_end(t);
        return ClipResult{.val = ClipHandle{*existing}};
    }

    ClipResult r = gltf::load_clip(m, s, abs_path(m, rel_path), target, opts);
    if (r.err == result::OK) {
        const char *stored = memory::push_str(scope_arena(m, s), key);
        get(m, r.val)->key = stored;
        strmap::insert(&m->clips_by_key[s], stored, r.val.v);
    }

    memory::temp_end(t);
    return r;
}

static void *resolve(Manager *m, Kind k, u32 handle) {
    return pool::resolve_or_zero(&m->pools[k], handle, KIND_NAME[k]);
}

Model *get(Manager *m, ModelHandle h) { return (Model *)resolve(m, KIND_MODEL, h.v); }
Mesh *get(Manager *m, MeshHandle h) { return (Mesh *)resolve(m, KIND_MESH, h.v); }
Material *get(Manager *m, MaterialHandle h) { return (Material *)resolve(m, KIND_MATERIAL, h.v); }
Skeleton *get(Manager *m, SkeletonHandle h) { return (Skeleton *)resolve(m, KIND_SKELETON, h.v); }
Clip *get(Manager *m, ClipHandle h) { return (Clip *)resolve(m, KIND_CLIP, h.v); }

void remove(Manager *m, ModelHandle h) {
    ASSERT(m);

    pool::Pool *models = &m->pools[KIND_MODEL];
    if (!pool::alive(models, h.v))
        return;

    u32 idx = handle_index(h.v);
    Model *model = (Model *)pool::at(models, idx);
    Scope s = (Scope)models->slots[idx].scope;
    if (model->path)
        strmap::remove(&m->by_path[s], model->path);

    // catalog-only; the bytes stay until unload_scope
    pool::release(models, idx);
}

void unload_scope(Manager *m, Scope s) {
    ASSERT(m && s < SCOPE_COUNT);

    // map keys point into the arena we are about to reset, so drop them first
    strmap::clear(&m->by_path[s]);
    strmap::clear(&m->clips_by_key[s]);
    strmap::clear(&m->failed[s]);

    pool::Pool *meshes = &m->pools[KIND_MESH];
    if (m->mesh_release) {
        for (u32 i = pool::first(meshes); i; i = pool::next(meshes, i))
            if (meshes->slots[i].scope == (u8)s)
                m->mesh_release(m->mesh_release_user, MeshHandle{pool::make_handle(meshes, i)});
    }

    for (u32 k = 0; k < KIND_COUNT; k++)
        pool::release_scope(&m->pools[k], (u8)s);

    memory::reset(&m->scopes[s]);
    LOG_INFO("asset: unloaded scope %u", (u32)s);
}

void on_mesh_release(Manager *m, MeshReleaseFn fn, void *user) {
    ASSERT(m);
    m->mesh_release = fn;
    m->mesh_release_user = user;
}

u32 mesh_capacity(Manager *m) {
    ASSERT(m);
    return m->pools[KIND_MESH].cap;
}

} // namespace asset
