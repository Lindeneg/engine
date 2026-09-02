#include "shader.h"

#include <glad/glad.h>

#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "file.h"
#include "logger.h"
#include "path.h"
#include "str.h"

#define SHADER_DIR "shaders"
#define SHADER_MANIFEST "shaders.cfg"
#define UNIFORM_NAME_CAP 96
#define PREAMBLE_CAP 512
#define MAX_CHUNKS 64
#define MAX_INCLUDE_DEPTH 8
#define FMT_CAP 128                            // one generated #line or #define
#define INFO_LOG_CAP 2048                      // driver compile/link messages
#define VARIANT_LABEL_CAP (SHADER_NAME_CAP + 64) // "name[FEATURE|FEATURE]"

namespace shader {

// Bit index -> the #define the preamble emits. Order matches the Feature enum;
// feature_name/feature_from_name are the editor's view of this table.
static const char *FEATURE_DEFINES[SHADER_FEATURE_BITS] = {
    "SKINNED",
    "ALPHA_MASK",
};

static const char *BUILTIN_NAMES[BUILTIN_COUNT] = {
    "u_model",
    "u_normal_mat",
    "u_base_color",
    "u_alpha_cutoff",
};

struct Shader {
    char name[SHADER_NAME_CAP];
    u32 supported;
    Program variants[SHADER_VARIANT_COUNT]; // indexed by feature mask
};

struct Registry {
    memory::Arena *arena;
    Shader items[SHADER_MAX_COUNT];
    u32 count;
    Handle dflt; // default_shader from the manifest
};

static Registry g_reg;

// A shader stage assembled from pieces: the generated preamble, spans of each
// file's text, and the #line directives between them. glShaderSource takes an
// array of strings, so the pieces never have to be concatenated -- which is
// what would otherwise force a growable allocation on top of a bump arena.
struct Chunks {
    const char *text[MAX_CHUNKS];
    i32 len[MAX_CHUNKS];
    u32 count;
    u32 next_source_id;
};

static b32 add_chunk(Chunks *c, const char *text, u64 len) {
    if (len == 0)
        return true;
    if (c->count == MAX_CHUNKS) {
        LOG_ERR("shader: more than %d source chunks; too many #includes", MAX_CHUNKS);
        return false;
    }

    c->text[c->count] = text;
    c->len[c->count] = (i32)len;
    c->count++;
    return true;
}

static b32 add_fmt(Chunks *c, memory::Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *text = memory::push_vfmt(a, fmt, ap);
    va_end(ap);
    return add_chunk(c, text, strlen(text));
}

// Recognises a line whose only content is #include "name". Returns the name
// copied into a, or NULL when the line is anything else.
static const char *parse_include(memory::Arena *a, const char *line, u64 len) {
    u64 i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i == len || line[i] != '#')
        return NULL;
    i++;

    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (len - i < 7 || memcmp(line + i, "include", 7) != 0)
        return NULL;
    i += 7;

    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i == len || line[i] != '"')
        return NULL;
    i++;

    u64 start = i;
    while (i < len && line[i] != '"')
        i++;
    if (i == len)
        return NULL;

    return memory::push_fmt(a, "%.*s", (i32)(i - start), line + start);
}

// Appends one shader file, splicing in whatever it #includes. Each file gets a
// source id and a #line directive, so a driver reports errors as "<id>(line)"
// against the file the author wrote rather than against one flattened blob.
static b32 add_file(Chunks *c, memory::Arena *scratch, const char *root, const char *file,
                    u32 depth) {
    if (depth > MAX_INCLUDE_DEPTH) {
        LOG_ERR("shader: #include nested deeper than %d at '%s' -- a cycle?", MAX_INCLUDE_DEPTH,
                file);
        return false;
    }

    file::BlobResult blob = file::read(scratch, FPATH(scratch, root, SHADER_DIR, file));
    if (blob.err != result::OK) {
        LOG_ERR("shader: cannot read %s", file);
        return false;
    }

    const char *text = (const char *)blob.val.data;
    u64 len = blob.val.size;
    u32 id = c->next_source_id++;

    if (!add_fmt(c, scratch, "#line 1 %u\n", id))
        return false;

    u64 run = 0;  // start of the span not yet emitted
    u32 line = 1; // line number of the span being read
    u64 i = 0;

    while (i < len) {
        u64 line_start = i;
        while (i < len && text[i] != '\n')
            i++;
        u64 line_end = i;
        if (i < len)
            i++;
        line++;

        const char *inc = parse_include(scratch, text + line_start, line_end - line_start);
        if (!inc)
            continue;

        if (!add_chunk(c, text + run, line_start - run))
            return false;
        run = i;

        if (!add_file(c, scratch, root, inc, depth + 1))
            return false;
        if (!add_fmt(c, scratch, "#line %u %u\n", line, id))
            return false;
    }

    return add_chunk(c, text + run, len - run);
}

// #version must be the first thing in the concatenated source, so this is
// always chunk 0. MAX_JOINTS is generated from the C++ constant rather than
// written into a .vert, so the palette size has exactly one definition.
static b32 add_preamble(Chunks *c, memory::Arena *a, u32 features) {
    char buf[PREAMBLE_CAP];
    i32 n = snprintf(buf, sizeof(buf), "#version 330 core\n#define MAX_JOINTS %d\n",
                     SHADER_MAX_JOINTS);
    ASSERT(n > 0);

    for (u32 b = 0; b < SHADER_FEATURE_BITS; b++) {
        if (!(features & (1u << b)))
            continue;
        i32 w = snprintf(buf + n, sizeof(buf) - (u64)n, "#define %s 1\n", FEATURE_DEFINES[b]);
        ASSERT(w > 0);
        n += w;
        ASSERT_MSG((u64)n < sizeof(buf), "shader preamble overflowed %d bytes", PREAMBLE_CAP);
    }

    return add_chunk(c, (const char *)memory::push_bytes(a, buf, (u64)n), (u64)n);
}

static u32 compile_stage(u32 type, const Chunks *c, const char *label) {
    u32 id = glCreateShader(type);
    glShaderSource(id, (i32)c->count, c->text, c->len);
    glCompileShader(id);

    i32 ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (ok)
        return id;

    char info[INFO_LOG_CAP];
    glGetShaderInfoLog(id, sizeof(info), NULL, info);
    LOG_ERR("shader: %s failed to compile\n%s", label, info);
    glDeleteShader(id);
    return 0;
}

// Reads back every active uniform, so the draw side needs no struct field per
// uniform. Block members report location -1 and are skipped: they are addressed
// through the UBO, not by location.
static b32 reflect(memory::Arena *a, Program *p, const char *label) {
    i32 active = 0;
    glGetProgramiv(p->gl, GL_ACTIVE_UNIFORMS, &active);
    p->uniforms = strmap::create(a, (u64)(active > 0 ? active : 1));

    // A name the buffer cannot hold would be truncated, and a truncated name
    // resolves to no location -- a uniform that silently does nothing forever.
    i32 longest = 0;
    glGetProgramiv(p->gl, GL_ACTIVE_UNIFORM_MAX_LENGTH, &longest);
    if (longest > UNIFORM_NAME_CAP) {
        LOG_ERR("shader: %s has a uniform name of %d bytes; the cap is %d", label, longest,
                UNIFORM_NAME_CAP);
        return false;
    }

    for (i32 i = 0; i < active; i++) {
        char buf[UNIFORM_NAME_CAP];
        i32 len = 0;
        i32 size = 0;
        u32 type = 0;
        glGetActiveUniform(p->gl, (u32)i, (i32)sizeof(buf), &len, &size, &type, buf);
        if (len <= 0)
            continue;

        // arrays come back as "u_foo[0]"; key on the bare name
        for (i32 c = 0; c < len; c++) {
            if (buf[c] == '[') {
                buf[c] = 0;
                len = c;
                break;
            }
        }

        i32 loc = glGetUniformLocation(p->gl, buf);
        if (loc < 0)
            continue;

        // the map keys on the pointer, so the name has to outlive the program
        strmap::insert(&p->uniforms, memory::push_str(a, buf), (u32)loc);
    }

    for (u32 b = 0; b < BUILTIN_COUNT; b++) {
        u32 *loc = strmap::find(&p->uniforms, BUILTIN_NAMES[b]);
        p->builtin[b] = loc ? (i32)*loc : -1;
    }
    return true;
}

static b32 link_program(memory::Arena *a, const Chunks *vc, const Chunks *fc, const char *label,
                        Program *out) {
    u32 v = compile_stage(GL_VERTEX_SHADER, vc, label);
    u32 f = compile_stage(GL_FRAGMENT_SHADER, fc, label);
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        return false;
    }

    u32 prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);

    // the program reference-counts the stages, so drop ours either way
    glDeleteShader(v);
    glDeleteShader(f);

    i32 ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[INFO_LOG_CAP];
        glGetProgramInfoLog(prog, sizeof(info), NULL, info);
        LOG_ERR("shader: %s failed to link\n%s", label, info);
        glDeleteProgram(prog);
        return false;
    }

    out->gl = prog;
    if (!reflect(a, out, label)) {
        glDeleteProgram(prog);
        out->gl = 0;
        return false;
    }

    u32 frame = glGetUniformBlockIndex(prog, "Frame");
    if (frame != GL_INVALID_INDEX)
        glUniformBlockBinding(prog, frame, SHADER_FRAME_BINDING);

    u32 joints = glGetUniformBlockIndex(prog, "Joints");
    if (joints != GL_INVALID_INDEX)
        glUniformBlockBinding(prog, joints, SHADER_JOINT_BINDING);

    return true;
}

static b32 build_variant(memory::Arena *a, memory::Arena *scratch, const char *root,
                         const char *vert_file, const char *frag_file, u32 features,
                         const char *label, Program *out) {
    memory::ArenaTemp t = memory::temp_begin(scratch);

    // Source ids are per stage, so both stages number their includes from 0 and
    // a driver's "<id>(line)" is unambiguous within the stage it came from.
    Chunks vc{};
    Chunks fc{};

    b32 ok = add_preamble(&vc, scratch, features) && add_file(&vc, scratch, root, vert_file, 0) &&
             add_preamble(&fc, scratch, features) && add_file(&fc, scratch, root, frag_file, 0) &&
             link_program(a, &vc, &fc, label, out);

    memory::temp_end(t);
    return ok;
}

// "mesh", "mesh[SKINNED]", "mesh[SKINNED|ALPHA_MASK]". Without it all four
// variants report under one name and a failure in one is impossible to place.
static void variant_label(char *dst, u64 cap, const char *name_, u32 features) {
    i32 n = snprintf(dst, cap, "%s", name_);
    ASSERT(n > 0);

    const char *sep = "[";
    for (u32 b = 0; b < SHADER_FEATURE_BITS; b++) {
        if (!(features & (1u << b)))
            continue;
        n += snprintf(dst + n, cap - (u64)n, "%s%s", sep, FEATURE_DEFINES[b]);
        sep = "|";
    }
    if (features)
        snprintf(dst + n, cap - (u64)n, "]");
}

static result::Error load_one(memory::Arena *a, memory::Arena *scratch, const char *root,
                              config::Node *decl) {
    const char *name_ = config::value(decl, 0, NULL);
    if (!name_) {
        LOG_ERR("shader: a 'shader' block has no name");
        return result::ERR_PARSE;
    }
    // refused rather than truncated: a clipped name would later fail the
    // default_shader lookup with a misleading "no shader named"
    if (strlen(name_) >= SHADER_NAME_CAP) {
        LOG_ERR("shader: name '%s' is longer than the %d bytes allowed", name_, SHADER_NAME_CAP - 1);
        return result::ERR_PARSE;
    }
    if (g_reg.count >= SHADER_MAX_COUNT) {
        LOG_ERR("shader: more than %d shaders declared, '%s' dropped", SHADER_MAX_COUNT, name_);
        return result::ERR_EXHAUSTED;
    }

    const char *vert_file = config::get(decl->first_child, "vert", NULL);
    const char *frag_file = config::get(decl->first_child, "frag", NULL);
    if (!vert_file || !frag_file) {
        LOG_ERR("shader: '%s' needs both a vert and a frag", name_);
        return result::ERR_PARSE;
    }

    u32 supported = FEATURE_NONE;
    config::Node *feats = config::find(decl->first_child, "features");
    for (u32 i = 0; feats && i < feats->value_count; i++) {
        u32 bit = feature_from_name(feats->values[i]);
        if (!bit) {
            LOG_ERR("shader: '%s' claims unknown feature '%s'", name_, feats->values[i]);
            return result::ERR_PARSE;
        }
        supported |= bit;
    }

    // zeroed first: a previous init's or a failed build's program ids must not
    // survive in the variants this manifest does not claim
    Shader *sh = &g_reg.items[g_reg.count];
    memset(sh, 0, sizeof(*sh));
    snprintf(sh->name, SHADER_NAME_CAP, "%s", name_);
    sh->supported = supported;

    u32 wanted = 0;
    for (u32 v = 0; v < SHADER_VARIANT_COUNT; v++) {
        if (v & ~supported)
            continue; // this shader does not claim that combination
        wanted++;

        char label[VARIANT_LABEL_CAP];
        variant_label(label, sizeof(label), name_, v);
        if (!build_variant(a, scratch, root, vert_file, frag_file, v, label, &sh->variants[v]))
            return result::ERR_INVALID;
    }

    g_reg.count++;
    LOG_INFO("shader: '%s' = %s + %s, %u variant(s)", name_, vert_file, frag_file, wanted);
    return result::OK;
}

result::Error init(memory::Arena *a, memory::Arena *scratch, const char *root) {
    ASSERT(a && scratch && root);
    ASSERT_MSG(g_reg.count == 0, "shader::init called twice");

    g_reg.arena = a;

    memory::ArenaTemp t = memory::temp_begin(scratch);
    const char *manifest = FPATH(scratch, root, SHADER_DIR, SHADER_MANIFEST);

    config::DocResult doc = config::load(scratch, manifest);
    if (doc.err != result::OK) {
        LOG_ERR("shader: cannot read %s", manifest);
        memory::temp_end(t);
        return doc.err;
    }
    // the manifest is infrastructure: a damaged one is a broken deployment
    if (doc.val->problems) {
        LOG_ERR("shader: %s has %u structural problem(s)", manifest, doc.val->problems);
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    for (config::Node *n = config::find(doc.val->first, "shader"); n;
         n = config::find_next(n, "shader")) {
        result::Error e = load_one(a, scratch, root, n);
        if (e != result::OK) {
            memory::temp_end(t);
            return e;
        }
    }

    if (g_reg.count == 0) {
        LOG_ERR("shader: %s declares no shaders", manifest);
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    const char *dflt = config::get(doc.val->first, "default_shader", NULL);
    if (!dflt) {
        LOG_ERR("shader: %s has no default_shader", manifest);
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    HandleResult h = find(dflt);
    memory::temp_end(t);
    if (h.err != result::OK)
        return h.err;

    g_reg.dflt = h.val;

    // name(), not dflt: dflt points into the manifest text, which the temp_end
    // above just rolled back out from under it.
    LOG_INFO("shader: %u shader(s) loaded, default '%s'", g_reg.count, name(h.val));
    return result::OK;
}

void shutdown() {
    for (u32 i = 0; i < g_reg.count; i++) {
        for (u32 v = 0; v < SHADER_VARIANT_COUNT; v++)
            if (g_reg.items[i].variants[v].gl)
                glDeleteProgram(g_reg.items[i].variants[v].gl);
        memset(&g_reg.items[i], 0, sizeof(g_reg.items[i]));
    }
    g_reg.count = 0;
    g_reg.dflt = Handle{0};
}

HandleResult find(const char *name_) {
    ASSERT(name_);
    for (u32 i = 0; i < g_reg.count; i++)
        if (strcmp(g_reg.items[i].name, name_) == 0)
            return HandleResult{.val = Handle{i}};

    LOG_ERR("shader: no shader named '%s' is declared", name_);
    return HandleResult{.err = result::ERR_NOT_FOUND};
}

Handle default_handle() {
    ASSERT_MSG(g_reg.count > 0, "shader::default_handle before init");
    return g_reg.dflt;
}

// init built every combination each shader claims, and masks it here, so
// failing to resolve one means a handle from somewhere else entirely.
Program *program(Handle h, u32 features) {
    ASSERT_MSG(h.v < g_reg.count, "shader: handle %u out of range", h.v);

    Shader *sh = &g_reg.items[h.v];
    Program *p = &sh->variants[features & sh->supported];
    ASSERT_MSG(p->gl, "shader: '%s' has no variant %u", sh->name, features & sh->supported);
    return p;
}

u32 supported_features(Handle h) {
    return h.v < g_reg.count ? g_reg.items[h.v].supported : FEATURE_NONE;
}

u32 program_id(Handle h, u32 features) {
    ASSERT_MSG(h.v < g_reg.count, "shader: handle %u out of range", h.v);
    return h.v * SHADER_VARIANT_COUNT + (features & g_reg.items[h.v].supported);
}

i32 uniform_loc(Program *p, const char *name_) {
    ASSERT(p && name_);
    u32 *loc = strmap::find(&p->uniforms, name_);
    return loc ? (i32)*loc : -1;
}

u32 count() { return g_reg.count; }

const char *name(Handle h) { return h.v < g_reg.count ? g_reg.items[h.v].name : "<invalid>"; }

u32 feature_from_name(const char *name_) {
    i32 bit = str::name_index(FEATURE_DEFINES, SHADER_FEATURE_BITS, name_);
    return bit < 0 ? FEATURE_NONE : 1u << bit;
}

const char *feature_name(u32 bit_index) {
    return bit_index < SHADER_FEATURE_BITS ? FEATURE_DEFINES[bit_index] : NULL;
}

} // namespace shader
