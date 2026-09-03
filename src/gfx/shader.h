#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"
#include "core/strmap.h"
#include "math/linalg.h"

// GPU programs, kept out of asset:: since it never calls GL. Which shaders exist
// is data: shaders.cfg names the sources, the features each claims, and which
// shader fills each role the engine asks for by itself. The engine defines only
// what a feature *means*, since each is backed by C++ (SKINNED binds the
// palette, ALPHA_MASK the cutoff), and what a role is for. There is no fallback
// program: a shader that will not build is a broken deployment.

#define SHADER_NAME_CAP 32
#define SHADER_MAX_COUNT 32

// generated into the GLSL preamble as MAX_JOINTS, so the palette size has one definition
#define SHADER_MAX_JOINTS 128

// std140 binding points; each buffer is bound once, never per draw
#define SHADER_FRAME_BINDING 0
#define SHADER_JOINT_BINDING 1

namespace shader {

// injected as #define lines ahead of the source; variants double per bit, so this stays short
enum Feature {
    FEATURE_NONE = 0,
    FEATURE_SKINNED = 1u << 0,    // vertex carries JOINTS_0/WEIGHTS_0; skin via the palette
    FEATURE_ALPHA_MASK = 1u << 1, // glTF alphaMode MASK: discard below the cutoff
};

#define SHADER_FEATURE_BITS 2
#define SHADER_VARIANT_COUNT (1u << SHADER_FEATURE_BITS)

// programs the engine reaches for without a material in between; the manifest
// names one shader per role, and a role it leaves out is a hard startup failure
enum Role {
    ROLE_DEFAULT, // default_shader: what a material gets when nothing assigned it one
    ROLE_DEBUG,   // debug_shader: position and colour, for gfx/debugdraw's lines
    ROLE_CANVAS,  // canvas_shader: textured quads in pixel space, for gfx/canvas
    ROLE_COUNT,
};

// uniforms the engine sets every draw, resolved once at link; everything else goes through
// uniform_loc
enum Builtin {
    BUILTIN_MODEL,        // mat4  u_model
    BUILTIN_NORMAL_MAT,   // mat3  u_normal_mat
    BUILTIN_BASE_COLOR,   // vec4  u_base_color
    BUILTIN_ALPHA_CUTOFF, // float u_alpha_cutoff
    BUILTIN_COUNT,
};

// std140 per-frame constants; the GLSL side is assets/shaders/common.glsl and changes with this
struct FrameData {
    linalg::Mat4 view_proj;
    linalg::Vec4 camera_pos;  // xyz; w unused
    linalg::Vec4 light_dir;   // xyz, normalized, pointing toward the light
    linalg::Vec4 light_color; // rgb; w is ambient strength
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

// one manifest block, unbuilt; exposed so the manifest rules are testable without a context
struct Decl {
    const char *name;
    const char *vert;
    const char *frag;
    u32 supported;
};

// Every shader block plus the name filling each role, which must be one of them.
// out_roles holds ROLE_COUNT entries.
result::Error parse_manifest(config::Doc *doc, Decl *out, u32 cap, u32 *out_count,
                             const char **out_roles);

// Reads shaders.cfg and builds every declared variant; any failure is fatal to
// the caller. Needs a current GL context.
result::Error init(memory::Arena *a, memory::Arena *scratch, const char *root);
void shutdown();

// An undeclared name is a content error, reported rather than substituted.
HandleResult find(const char *name);

// The shader shaders.cfg named for a role.
Handle role_handle(Role r);

// Never NULL; features the shader does not claim are masked off. An
// unresolvable program is a bug and asserts.
Program *program(Handle h, u32 features);

u32 supported_features(Handle h);

// Small, dense id for a variant, for the draw queue's sort key; not a GL name.
u32 program_id(Handle h, u32 features);

// -1 when absent, which glUniform* accepts as a no-op.
i32 uniform_loc(Program *p, const char *name);

// Enumeration, for an editor's shader picker and feature checklist.
u32 count();
const char *name(Handle h);
u32 feature_from_name(const char *name); // FEATURE_NONE when unknown
const char *feature_name(u32 bit_index); // NULL past SHADER_FEATURE_BITS
const char *role_key(Role r);            // the manifest key, "default_shader"

} // namespace shader
