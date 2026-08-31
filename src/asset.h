#pragma once

#include "common.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"
#include "strmap.h"

#define ASSET_NAME_CAP 64

// Every handle is the same shape on purpose. They stay distinct types so a mesh
// handle cannot be passed where a material handle belongs -- both are opaque
// u32s, so the mix-up is otherwise invisible -- and so get() can be one
// overloaded name instead of five.
#define ASSET_HANDLE(Name)                                                                         \
    struct Name {                                                                                  \
        u32 v;                                                                                     \
    }

namespace asset {

#define ASSET_EPOCH_SHIFT 24
#define ASSET_INDEX_MASK ((1u << ASSET_EPOCH_SHIFT) - 1)

// Packed (epoch << 24) | index. Handle 0 is the fallback asset, never
// "invalid": zeroed memory means "missing", and get() always returns something
// drawable. The encoding is public so handles can be serialized and printed.
inline u32 handle_index(u32 v) { return v & ASSET_INDEX_MASK; }
inline u8 handle_epoch(u32 v) { return (u8)(v >> ASSET_EPOCH_SHIFT); }

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

// Arenas cannot free individual allocations, so lifetime is per-scope: unload
// resets one arena and invalidates every handle that pointed into it.
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

// Union of pointers, not of elements: a union of the structs themselves would
// size every element at the larger one and save nothing.
union VertexArray {
    VertexStatic *statics;
    VertexSkinned *skinned;
};

inline u64 vertex_stride(VertexKind k) {
    return k == VERTEX_SKINNED ? sizeof(VertexSkinned) : sizeof(VertexStatic);
}

// A primitive: a range of the parent mesh's index buffer with one material.
// pistol.glb shares one vertex buffer across 47 primitives, so vertices are
// deduplicated by source accessor and only the index ranges differ.
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

    // owned by the renderer, untouched here -- the manager never calls OpenGL
    u32 gpu_vao, gpu_vbo, gpu_ebo;
};

// Physics raycasts against positions regardless of skinning, and reading through
// the union directly at every call site would be both noisy and type-punning.
inline linalg::Vec3 vertex_pos(const Mesh *m, u32 i) {
    return m->vertex_kind == VERTEX_SKINNED ? m->vertices.skinned[i].pos
                                            : m->vertices.statics[i].pos;
}

struct Material {
    char name[ASSET_NAME_CAP];
    linalg::Vec4 base_color;
    f32 metallic;
    f32 roughness;
};

struct Joint {
    char name[ASSET_NAME_CAP]; // canonicalized via gltf::canonical_name
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

    // World transform of the joint hierarchy's parent node. The skin's joint
    // list often starts below the armature, but the inverse bind matrices were
    // baked against the full chain, so parentless joints need this on the left.
    // It is kept separate rather than folded into a root joint's TRS because
    // animation overwrites that TRS every frame.
    linalg::Mat4 root;
};

enum Path { PATH_TRANSLATION, PATH_ROTATION, PATH_SCALE };
enum Interp { INTERP_LINEAR, INTERP_STEP };

struct Channel {
    char target[ASSET_NAME_CAP]; // canonical node name, kept so a clip can rebind
    i32 joint;                   // resolved against one skeleton; -1 when unmatched
    Path path;
    Interp interp;
    f32 *times;
    f32 *values; // 3 floats per key, 4 for rotation
    u32 key_count;
};

struct Clip {
    char name[ASSET_NAME_CAP];
    f32 duration;
    Channel *channels;
    u32 channel_count;
    u32 bound_count; // channels that matched a joint; 0 means the bind failed

    // A clip whose file carries no skin was authored against some other rig, so
    // playing it here is retargeting. Only rotations transfer: translation and
    // scale encode the source rig's bone lengths in the source file's units,
    // and applying those to a different skeleton tears it apart.
    b32 rotation_only;
};

// Per-asset import settings. A hand-run FBX->glB pipeline produces files in
// whatever units and up-axis the source tool used, and no two exporters agree.
struct ImportOptions {
    f32 scale; // 0 means "as authored"
    b32 z_up;  // rotate -90 about X to bring a Z-up asset into our Y-up world
};

struct Model {
    char name[ASSET_NAME_CAP];
    const char *path;   // lives in the scope arena; the dedup map keys on it
    MeshHandle *meshes; // array, not a range: ranges would forbid interleaved loads
    u32 mesh_count;
    SkeletonHandle skeleton; // SKELETON_FALLBACK (identity) for static models

    // Applied on top of the model matrix rather than baked, so skinning stays
    // in native space and the joint palette needs no adjustment.
    linalg::Mat4 import;
};

struct Config {
    u32 max_models;
    u32 max_meshes;
    u32 max_materials;
    u32 max_skeletons;
    u32 max_clips;
    u64 scope_bytes[SCOPE_COUNT];
};

struct Manager;

RESULT_TYPE(Manager *, ManagerResult);
RESULT_TYPE(ModelHandle, ModelResult);
RESULT_TYPE(MeshHandle, MeshResult);
RESULT_TYPE(MaterialHandle, MaterialResult);
RESULT_TYPE(SkeletonHandle, SkeletonResult);
RESULT_TYPE(ClipHandle, ClipResult);

// Resolves the asset root from the executable's own path rather than the
// working directory, and fills slot 0 of every array with its fallback asset.
ManagerResult init(memory::Arena *mgr_arena, memory::Arena *scratch, Config cfg);

// The arena a scope's payloads live in. Fill a freshly created asset by pushing
// into this; it dies with unload_scope.
memory::Arena *scope_arena(Manager *m, Scope s);

// Shared parse/staging space. Anything pushed here must be inside a temp scope.
memory::Arena *scratch_arena(Manager *m);

// The resolved executable directory that asset paths hang off.
const char *root(Manager *m);

// Axis-aligned bounds over every vertex of every mesh, in model space. Used to
// frame a camera now, and by physics broadphase later.
void model_bounds(Manager *m, ModelHandle h, linalg::Vec3 *out_min, linalg::Vec3 *out_max);

// Claim an empty slot. These are what load_model is built from, and they are
// also the entry point for procedural geometry and for an editor creating an
// asset from nothing. ERR_EXHAUSTED when the pool is full.
ModelResult create_model(Manager *m, Scope s, const char *name);
MeshResult create_mesh(Manager *m, Scope s);
MaterialResult create_material(Manager *m, Scope s);
SkeletonResult create_skeleton(Manager *m, Scope s);
ClipResult create_clip(Manager *m, Scope s);

// Dedups on path: loading the same file twice returns the same handle.
// Pair with OR_ELSE(..., MODEL_FALLBACK) to keep a missing asset visible in the
// world instead of fatal; read .err directly when the reason matters.
ModelResult load_model(Manager *m, Scope s, const char *rel_path, ImportOptions opts);

// Builds the matrix ImportOptions describes. Exposed so physics and the
// renderer agree on it rather than each deriving their own.
linalg::Mat4 import_matrix(ImportOptions opts);

// A clip binds to one skeleton at load, matching channels to joints by
// canonical name. Records the bind rate on the Clip; a rate of 0 warns loudly,
// since an unbound clip plays silently and animates nothing.
ClipResult load_clip(Manager *m, Scope s, const char *rel_path, SkeletonHandle target);

// Never NULL, and deliberately not a Result: this runs per-object per-frame and
// a bad handle means a bug elsewhere, not a condition the caller can act on. A
// stale or out-of-range handle yields the fallback asset and warns.
Model *get(Manager *m, ModelHandle h);
Mesh *get(Manager *m, MeshHandle h);
Material *get(Manager *m, MaterialHandle h);
Skeleton *get(Manager *m, SkeletonHandle h);
Clip *get(Manager *m, ClipHandle h);

// Catalog-only, for the editor: frees the slot, bumps its epoch and drops the
// path entry, but leaves the bytes until the scope resets. Makes undo cheap.
void remove(Manager *m, ModelHandle h);

// Resets the scope's arena and invalidates every handle it issued.
void unload_scope(Manager *m, Scope s);

} // namespace asset
