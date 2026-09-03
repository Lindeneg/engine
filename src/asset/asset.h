#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/pool.h"
#include "core/result.h"
#include "core/strmap.h"
#include "gfx/gfx.h"
#include "gfx/shader.h"
#include "math/linalg.h"

#define ASSET_NAME_CAP 64

// distinct types over the same u32, so a mesh handle cannot be passed as a
// material handle and get() overloads on them
#define ASSET_HANDLE(Name)                                                                         \
    struct Name {                                                                                  \
        u32 v;                                                                                     \
    }

namespace asset {

// (epoch << 24) | index, from pool::. Handle 0 is the fallback asset, never
// "invalid": zeroed memory reads as missing and get() always returns something drawable.
inline u32 handle_index(u32 v) { return pool::handle_index(v); }
inline u8 handle_epoch(u32 v) { return pool::handle_epoch(v); }

ASSET_HANDLE(ModelHandle);
ASSET_HANDLE(MeshHandle);
ASSET_HANDLE(MaterialHandle);
ASSET_HANDLE(SkeletonHandle);
ASSET_HANDLE(ClipHandle);

constexpr ModelHandle MODEL_FALLBACK = {0};
constexpr MeshHandle MESH_FALLBACK = {0};
constexpr MaterialHandle MATERIAL_FALLBACK = {0};
constexpr SkeletonHandle SKELETON_FALLBACK = {0};
constexpr ClipHandle CLIP_FALLBACK = {0};

// lifetime is per scope: unload_scope resets one arena and invalidates every handle into it
enum Scope { SCOPE_PERSISTENT, SCOPE_TRANSIENT, SCOPE_COUNT };

struct VertexStatic {
    linalg::Vec3 pos;
    linalg::Vec3 normal;
    linalg::Vec2 uv;
};

struct VertexSkinned {
    linalg::Vec3 pos;
    linalg::Vec3 normal;
    linalg::Vec2 uv;
    u16 joints[4]; // JOINTS_0 is u16 in our assets, not u8
    f32 weights[4];
};

enum VertexKind { VERTEX_STATIC, VERTEX_SKINNED };

// pointers, not elements: a union of the structs would size every element at the larger
union VertexArray {
    VertexStatic *statics;
    VertexSkinned *skinned;
};

inline u64 vertex_stride(VertexKind k) {
    return k == VERTEX_SKINNED ? sizeof(VertexSkinned) : sizeof(VertexStatic);
}

// one primitive: an index range of the parent mesh with one material
struct Submesh {
    u32 index_offset;
    u32 index_count;
    MaterialHandle material;
};

struct Mesh {
    VertexKind vertex_kind;
    VertexArray vertices;
    u32 vertex_count;
    u32 *indices; // always u32; u16 sources widen on load
    u32 index_count;
    Submesh *submeshes;
    u32 submesh_count;
};

// position regardless of skinning, for raycasts
inline linalg::Vec3 vertex_pos(const Mesh *m, u32 i) {
    return m->vertex_kind == VERTEX_SKINNED ? m->vertices.skinned[i].pos
                                            : m->vertices.statics[i].pos;
}

// glTF alphaMode; picks the shader variant, so it is kept rather than baked into the colour
enum AlphaMode { ALPHA_OPAQUE, ALPHA_MASK, ALPHA_BLEND };

struct Material {
    char name[ASSET_NAME_CAP];

    // the manager's default at load; a sidecar or an editor reassigns it per material
    shader::Handle shader;

    linalg::Vec4 base_color;
    f32 metallic;
    f32 roughness;
    AlphaMode alpha_mode;
    f32 alpha_cutoff;

    // defaults from the glTF (doubleSided, alphaMode); the .mat.cfg sidecar overrides
    gfx::RenderState state;
};

struct Joint {
    char name[ASSET_NAME_CAP]; // canonical, see str::canonical_name
    i32 parent;                // -1 at the root
    linalg::Mat4 inverse_bind;
    linalg::Vec3 bind_t;
    linalg::Quat bind_r;
    linalg::Vec3 bind_s;
};

struct Skeleton {
    Joint *joints;
    u32 joint_count;
    strmap::Map by_name; // canonical joint name -> index into joints

    // transform of the node above the topmost joints; the inverse binds were
    // baked against it, so parentless joints sit under it
    linalg::Mat4 root;
};

enum Path { PATH_TRANSLATION, PATH_ROTATION, PATH_SCALE };
enum Interp { INTERP_LINEAR, INTERP_STEP };

struct Channel {
    char target[ASSET_NAME_CAP]; // canonical node name
    i32 joint;                   // -1 when unmatched
    Path path;
    Interp interp;
    f32 *times;
    f32 *values; // 3 floats per key, 4 for rotation
    u32 key_count;
};

struct Clip {
    char name[ASSET_NAME_CAP];
    const char *key; // "<path>@<skeleton>" in the scope arena; the dedup key
    f32 duration;
    Channel *channels;
    u32 channel_count;
    u32 bound_count;   // channels that matched a joint; 0 means the bind failed
    b32 rotation_only; // from ClipOptions, never inferred
    b32 loop;          // likewise; off, time holds at the end
};

// Per-clip import settings, the author's call: which rig a clip was made for is
// not in the file, and neither is whether it cycles. The loader warns when a
// setting looks wrong; it never decides. clip_options() supplies the defaults.
struct ClipOptions {
    // rotations only: translation/scale channels carry the source rig's bone
    // lengths and tear a different skeleton apart. Costs root motion.
    b32 rotation_only;

    // an idle cycles; a fire does not, and holds its last frame
    b32 loop;
};

// Per-asset import settings; hand-converted files disagree on units and up-axis.
struct ImportOptions {
    f32 scale; // multiplier on the authored units; the sidecar default is 1
    b32 z_up;  // rotate -90 about X to bring a Z-up asset into our Y-up world
};

struct Model {
    char name[ASSET_NAME_CAP];
    const char *path;   // in the scope arena; the dedup key
    MeshHandle *meshes; // array, not a range: ranges would forbid interleaved loads
    u32 mesh_count;
    SkeletonHandle skeleton; // SKELETON_FALLBACK (identity) for static models

    // applied on top of the model matrix, not baked, so skinning stays in native space
    linalg::Mat4 import;

    // local-space AABB, pre-import, measured at load in the bind pose (raw
    // skinned positions are pre-skin and not the model's shape)
    linalg::Vec3 bounds_min;
    linalg::Vec3 bounds_max;
};

struct Config {
    // the project folder every relative path is under; passed in because
    // shader:: needs it before a manager exists
    const char *root;

    // what a loaded material binds to; a handle so asset:: never asks the shader module anything
    shader::Handle default_shader;

    u32 max_models;
    u32 max_meshes;
    u32 max_materials;
    u32 max_skeletons;
    u32 max_clips;
    u64 scope_bytes[SCOPE_COUNT];
};

// The `assets` block of engine.cfg; NULL or missing keys take the defaults.
// root and default_shader are not file settings and come from the caller.
Config config_from(config::Node *block, const char *root, shader::Handle default_shader);

struct Manager;

RESULT_TYPE(Manager *, ManagerResult);
RESULT_TYPE(ModelHandle, ModelResult);
RESULT_TYPE(MeshHandle, MeshResult);
RESULT_TYPE(MaterialHandle, MaterialResult);
RESULT_TYPE(SkeletonHandle, SkeletonResult);
RESULT_TYPE(ClipHandle, ClipResult);

// Fills slot 0 of every pool with its fallback asset.
ManagerResult init(memory::Arena *mgr_arena, memory::Arena *scratch, Config cfg);

// Where a scope's payloads live; dies with unload_scope.
memory::Arena *scope_arena(Manager *m, Scope s);

// Shared parse/staging space; anything pushed here must be inside a temp scope.
memory::Arena *scratch_arena(Manager *m);

const char *root(Manager *m);
shader::Handle default_shader(Manager *m);

// The bounds with the import transform applied, in world units. All eight
// corners are transformed, since import can rotate.
void model_bounds(Manager *m, ModelHandle h, linalg::Vec3 *out_min, linalg::Vec3 *out_max);

// Claim an empty slot; the entry point for procedural geometry and for an
// editor. ERR_EXHAUSTED when the pool is full.
ModelResult create_model(Manager *m, Scope s, const char *name);
MeshResult create_mesh(Manager *m, Scope s);
MaterialResult create_material(Manager *m, Scope s);
SkeletonResult create_skeleton(Manager *m, Scope s);
ClipResult create_clip(Manager *m, Scope s);

// Dedups on path within a scope; scopes never share. A failed load fails again
// from a cache, without re-reading or re-logging, until the scope unloads.
// Import settings come from the sidecar beside the asset (soldier.glb reads
// soldier.import.cfg): they belong to the file, not to whoever loads it.
ModelResult load_model(Manager *m, Scope s, const char *rel_path);

// The same with settings supplied directly, for procedural assets and tests.
ModelResult load_model_with(Manager *m, Scope s, const char *rel_path, ImportOptions opts);

// The sidecar's settings, or the defaults when there is none.
ImportOptions import_options(Manager *m, const char *rel_path);
ClipOptions clip_options(Manager *m, const char *rel_path);

// Whether the file, and the .import.cfg beside it, exist under the root.
b32 exists(Manager *m, const char *rel_path);
b32 has_import_sidecar(Manager *m, const char *rel_path);

// Writes the sidecar wholesale, each key with a note of what it means. The
// editor's half of the contract: it writes, the engine reads.
result::Error write_import_options(Manager *m, const char *rel_path, ImportOptions opts);
result::Error write_clip_options(Manager *m, const char *rel_path, ClipOptions opts);

// Applies settings to what is already loaded, in every scope: the model's
// import matrix, or every clip of that file whatever skeleton it was bound to.
// Neither is baked at load, so this is the whole change. Returns how many
// assets took it. Separate from writing, so a change can be seen before it is
// kept.
u32 apply_import_options(Manager *m, const char *rel_path, ImportOptions opts);
u32 apply_clip_options(Manager *m, const char *rel_path, ClipOptions opts);

// The matrix ImportOptions describes; exposed so physics and the renderer agree on it.
linalg::Mat4 import_matrix(ImportOptions opts);

// Binds channels to target's joints by canonical name. Dedups on (path, target) within a scope.
ClipResult load_clip(Manager *m, Scope s, const char *rel_path, SkeletonHandle target);
ClipResult load_clip_with(Manager *m, Scope s, const char *rel_path, SkeletonHandle target,
                          ClipOptions opts);

// Never NULL and not a Result: this runs per draw, and a stale handle is a bug,
// so it yields the fallback asset and warns once.
Model *get(Manager *m, ModelHandle h);
Mesh *get(Manager *m, MeshHandle h);
Material *get(Manager *m, MaterialHandle h);
Skeleton *get(Manager *m, SkeletonHandle h);
Clip *get(Manager *m, ClipHandle h);

// Catalog-only: frees the slot and drops the path entry; the bytes stay until the scope resets.
void remove(Manager *m, ModelHandle h);

// Resets the scope's arena, invalidating its handles; meshes go through the release hook first.
void unload_scope(Manager *m, Scope s);

// How the renderer learns a mesh is going away; the manager never calls GL itself.
typedef void (*MeshReleaseFn)(void *user, MeshHandle h);
void on_mesh_release(Manager *m, MeshReleaseFn fn, void *user);

// Pool capacity, for a side table indexed by mesh slot.
u32 mesh_capacity(Manager *m);

} // namespace asset
