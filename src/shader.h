#pragma once

#include "common.h"
#include "config.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"
#include "strmap.h"

// GPU programs. Kept out of the asset manager on purpose: asset:: never calls
// OpenGL, and a linked program has no meaningful CPU-side payload.
//
// Which shaders exist is data. shaders.cfg names the sources, the features each
// one compiles with, and which one a material gets by default -- so adding or
// repointing a shader is an edit-time action, not a rebuild. What the engine
// defines is the *meaning* of a feature, because each is backed by C++ that has
// to agree with it: SKINNED needs the joint palette bound and the skinned
// vertex layout, ALPHA_MASK needs the cutoff uniform. The editor picks which
// features a shader claims, from the list the engine publishes here.
//
// There is no fallback program. A shader that will not build is a broken
// deployment, not a runtime condition, so init fails and the caller aborts.
// Content can fall back at runtime; infrastructure cannot.

#define SHADER_NAME_CAP 32
#define SHADER_MAX_COUNT 32

// Must match MAX_JOINTS in the shader, which is generated from this into the
// preamble -- so the palette size has exactly one definition instead of a
// number repeated in a .vert where it can silently drift.
#define SHADER_MAX_JOINTS 128

// std140 binding points, assigned at link for every program that declares the
// block. Each buffer is bound once and never rebound per draw.
#define SHADER_FRAME_BINDING 0
#define SHADER_JOINT_BINDING 1

namespace shader {

// One source pair covers every combination of these: the loader injects them as
// #define lines ahead of the source, so a new feature is an #ifdef rather than
// another file. Variants are compiled eagerly and their count doubles per bit,
// so this list stays short and every entry is backed by engine code.
enum Feature {
    FEATURE_NONE = 0,
    FEATURE_SKINNED = 1u << 0,    // vertex carries JOINTS_0/WEIGHTS_0; skin via the palette
    FEATURE_ALPHA_MASK = 1u << 1, // glTF alphaMode MASK: discard below the cutoff
};

#define SHADER_FEATURE_BITS 2
#define SHADER_VARIANT_COUNT (1u << SHADER_FEATURE_BITS)

// Uniforms the engine sets itself on every draw. They resolve once at link into
// a flat array, so the draw loop never hashes a string. Everything else goes
// through uniform_loc by name: adding a uniform is a shader edit, not a C++
// struct edit.
enum Builtin {
    BUILTIN_MODEL,        // mat4  u_model
    BUILTIN_NORMAL_MAT,   // mat3  u_normal_mat
    BUILTIN_BASE_COLOR,   // vec4  u_base_color
    BUILTIN_ALPHA_CUTOFF, // float u_alpha_cutoff
    BUILTIN_COUNT,
};

// Per-frame constants, std140. The GLSL side lives in assets/shaders/common.glsl
// and is #included by every stage; these two are siblings and change together.
// Laid out so std140 needs no explicit padding: mat4 is four vec4s, vec4 is
// 16-byte aligned.
struct FrameData {
    linalg::Mat4 view_proj;
    linalg::Vec4 camera_pos;   // xyz; w unused
    linalg::Vec4 light_dir;    // xyz, normalized, pointing toward the light
    linalg::Vec4 light_color;  // rgb; w is ambient strength
};

struct Handle {
    u32 v;
};

struct Program {
    u32 gl;
    i32 builtin[BUILTIN_COUNT];
    strmap::Map uniforms; // uniform name -> location, for everything else
};

RESULT_TYPE(Handle, HandleResult);

// One shader block of the manifest, checked but not built. Strings point into
// the document. Exposed so the manifest rules are testable without a context.
struct Decl {
    const char *name;
    const char *vert;
    const char *frag;
    u32 supported;
};

// Every shader block plus default_shader, which must name one of them.
result::Error parse_manifest(config::Doc *doc, Decl *out, u32 cap, u32 *out_count,
                             const char **out_default);

// Reads shaders.cfg and builds every declared variant. Any failure is returned:
// the caller is expected to abort on it. Requires a current GL context.
result::Error init(memory::Arena *a, memory::Arena *scratch, const char *root);
void shutdown();

// A name that is not declared is a content error -- a material referring to a
// shader nobody defined -- so this reports rather than substituting.
HandleResult find(const char *name);

// The shader a material gets when nothing has assigned it one, named by the
// default_shader key in shaders.cfg so the editor can repoint it.
Handle default_handle();

// Never NULL. Features outside what the shader claims are masked off, and init
// guaranteed every claimed combination built, so an unresolvable program here
// is a bug rather than a condition -- it asserts.
Program *program(Handle h, u32 features);

u32 supported_features(Handle h);

// Compact, dense identifier for a resolved variant -- not a GL name. The draw
// queue packs this into a sort key, where small and dense is what matters and a
// driver-assigned program name is neither.
u32 program_id(Handle h, u32 features);

// Escape hatch for uniforms the engine does not know about. -1 when absent,
// which every glUniform* call accepts as a no-op.
i32 uniform_loc(Program *p, const char *name);

// Enumeration, so an editor populates a shader picker and a feature checklist
// from the engine instead of a hardcoded copy of this list.
u32 count();
const char *name(Handle h);
u32 feature_from_name(const char *name); // FEATURE_NONE when unknown
const char *feature_name(u32 bit_index); // NULL past SHADER_FEATURE_BITS

} // namespace shader
