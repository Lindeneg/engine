#include "asset.h"

#include <string.h>

#include "gltf.h"
#include "logger.h"
#include "path.h"

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif PLATFORM_LINUX
#include <unistd.h>
#endif

#define ROOT_CAP 512
#define ASSET_DIR "assets"

namespace asset {

struct Slot {
    u8 epoch;
    b8 used;
    u8 scope;
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
    if (idx >= p->cap || !p->slots[idx].used || p->slots[idx].epoch != handle_epoch(v)) {
        if (v != 0)
            LOG_WARN("asset: stale %s handle %u, substituting fallback", what, v);
        return pool_at(p, 0);
    }
    return pool_at(p, idx);
}

// Assets resolve against the executable's directory, never the working
// directory: running from an IDE or debugger otherwise breaks every path.
static b32 resolve_root(char *dst, u64 cap) {
#if PLATFORM_WINDOWS
    DWORD n = GetModuleFileNameA(NULL, dst, (DWORD)cap);
    if (n == 0 || n >= cap)
        return false;
#elif PLATFORM_LINUX
    i64 n = readlink("/proc/self/exe", dst, cap - 1);
    if (n <= 0 || (u64)n >= cap - 1)
        return false;
    dst[n] = 0;
#endif

    for (i64 i = (i64)strlen(dst) - 1; i >= 0; i--) {
        if (dst[i] == '/' || dst[i] == '\\') {
            dst[i] = 0;
            return true;
        }
    }
    return false;
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
    mat->base_color = linalg::Vec4{1.0f, 0.0f, 1.0f, 1.0f};
    mat->roughness = 1.0f;

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
    model->meshes = push_array(a, MeshHandle, 1);
    model->mesh_count = 1;
    model->meshes[0] = MESH_FALLBACK;
    model->skeleton = SKELETON_FALLBACK;
}

ManagerResult init(memory::Arena *mgr_arena, memory::Arena *scratch, Config cfg) {
    ASSERT(mgr_arena && scratch);

    Manager *m = push_struct(mgr_arena, Manager);
    m->mgr = mgr_arena;
    m->scratch = scratch;

    if (!resolve_root(m->root, ROOT_CAP)) {
        LOG_ERR("asset: cannot resolve executable directory");
        return ManagerResult{.err = result::ERR_IO};
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

ModelResult create_model(Manager *m, Scope s, const char *name) {
    ASSERT(m);

    u32 idx = pool_alloc(&m->models, s);
    if (!idx)
        return ModelResult{.err = result::ERR_EXHAUSTED};

    Model *model = (Model *)pool_at(&m->models, idx);
    snprintf(model->name, ASSET_NAME_CAP, "%s", name ? name : "<unnamed>");
    model->skeleton = SKELETON_FALLBACK;

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

ModelResult load_model(Manager *m, Scope s, const char *rel_path) {
    ASSERT(m && rel_path);

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

    get(m, r.val)->path = stored;
    strmap::insert(&m->by_path, stored, r.val.v);

    return r;
}

ClipResult load_clip(Manager *m, Scope s, const char *rel_path, SkeletonHandle target) {
    ASSERT(m && rel_path);

    memory::ArenaTemp t = memory::temp_begin(m->scratch);
    const char *abs = FPATH(m->scratch, m->root, ASSET_DIR, rel_path);
    ClipResult r = gltf::load_clip(m, s, abs, target);
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
