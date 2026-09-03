#include "gfx/shader.h"

#include <glad/glad.h>
#include <stdarg.h>
#include <string.h>

#include "core/config.h"
#include "core/logger.h"
#include "core/str.h"
#include "platform/file.h"
#include "platform/path.h"

#define SHADER_DIR "shaders"
#define SHADER_MANIFEST "shaders.cfg"
#define UNIFORM_NAME_CAP 96
#define PREAMBLE_CAP 512
#define MAX_CHUNKS 64
#define MAX_INCLUDE_DEPTH 8
#define FMT_CAP 128                              // one generated #line or #define
#define INFO_LOG_CAP 2048                        // driver compile/link messages
#define VARIANT_LABEL_CAP (SHADER_NAME_CAP + 64) // "name[FEATURE|FEATURE]"

namespace shader {

// bit index -> #define; order matches Feature
static const char *FEATURE_DEFINES[SHADER_FEATURE_BITS] = {
    "SKINNED",
    "ALPHA_MASK",
};

// manifest key per role; order matches Role
static const char *ROLE_KEYS[ROLE_COUNT] = {
    "default_shader",
    "debug_shader",
    "canvas_shader",
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
    Handle roles[ROLE_COUNT];
};

static Registry g_reg;

// a stage as pieces (preamble, file spans, #line directives); glShaderSource
// takes an array, so nothing is concatenated
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

// the name from a line that is only #include "name", copied into a; else NULL
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

// appends one file, splicing in its #includes. Each file gets a source id and
// #line, so driver errors point at the file the author wrote.
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

// #version must come first, so this is chunk 0; MAX_JOINTS comes from the C++ constant
static b32 add_preamble(Chunks *c, memory::Arena *a, u32 features) {
    char buf[PREAMBLE_CAP];
    i32 n =
        snprintf(buf, sizeof(buf), "#version 330 core\n#define MAX_JOINTS %d\n", SHADER_MAX_JOINTS);
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

// every active uniform by name; block members report -1 and are skipped
static b32 reflect(memory::Arena *a, Program *p, const char *label) {
    i32 active = 0;
    glGetProgramiv(p->gl, GL_ACTIVE_UNIFORMS, &active);
    p->uniforms = strmap::create(a, (u64)(active > 0 ? active : 1));

    // a truncated name resolves to no location and silently does nothing
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

    // source ids are per stage
    Chunks vc{};
    Chunks fc{};

    b32 ok = add_preamble(&vc, scratch, features) && add_file(&vc, scratch, root, vert_file, 0) &&
             add_preamble(&fc, scratch, features) && add_file(&fc, scratch, root, frag_file, 0) &&
             link_program(a, &vc, &fc, label, out);

    memory::temp_end(t);
    return ok;
}

// "mesh", "mesh[SKINNED|ALPHA_MASK]": so a failure names its variant
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

result::Error parse_manifest(config::Doc *doc, Decl *out, u32 cap, u32 *out_count,
                             const char **out_roles) {
    ASSERT(doc && out && out_count && out_roles);

    u32 n = 0;
    for (config::Node *decl = config::find(doc->first, "shader"); decl;
         decl = config::find_next(decl, "shader")) {
        const char *name_ = config::value(decl, 0, NULL);
        if (!name_) {
            LOG_ERR("shader: %s:%u: a 'shader' block has no name", doc->name, decl->line);
            return result::ERR_PARSE;
        }
        // refused, not truncated: a clipped name would fail the default_shader lookup misleadingly
        if (strlen(name_) >= SHADER_NAME_CAP) {
            LOG_ERR("shader: name '%s' is longer than the %d bytes allowed", name_,
                    SHADER_NAME_CAP - 1);
            return result::ERR_PARSE;
        }
        if (n >= cap) {
            LOG_ERR("shader: more than %u shaders declared, '%s' dropped", cap, name_);
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

        out[n++] =
            Decl{.name = name_, .vert = vert_file, .frag = frag_file, .supported = supported};
    }

    if (n == 0) {
        LOG_ERR("shader: %s declares no shaders", doc->name);
        return result::ERR_PARSE;
    }

    for (u32 r = 0; r < ROLE_COUNT; r++) {
        const char *named = config::get(doc->first, ROLE_KEYS[r], NULL);
        if (!named) {
            LOG_ERR("shader: %s has no %s", doc->name, ROLE_KEYS[r]);
            return result::ERR_PARSE;
        }

        b32 declared = false;
        for (u32 i = 0; i < n && !declared; i++)
            declared = strcmp(out[i].name, named) == 0;
        if (!declared) {
            LOG_ERR("shader: %s '%s' is not declared in %s", ROLE_KEYS[r], named, doc->name);
            return result::ERR_NOT_FOUND;
        }

        out_roles[r] = named;
    }

    *out_count = n;
    return result::OK;
}

static result::Error build_one(memory::Arena *a, memory::Arena *scratch, const char *root,
                               const Decl *d) {
    // zeroed: no program id from a previous init may survive in an unclaimed variant
    Shader *sh = &g_reg.items[g_reg.count];
    memset(sh, 0, sizeof(*sh));
    snprintf(sh->name, SHADER_NAME_CAP, "%s", d->name);
    sh->supported = d->supported;

    u32 wanted = 0;
    for (u32 v = 0; v < SHADER_VARIANT_COUNT; v++) {
        if (v & ~d->supported)
            continue; // this shader does not claim that combination
        wanted++;

        char label[VARIANT_LABEL_CAP];
        variant_label(label, sizeof(label), d->name, v);
        if (!build_variant(a, scratch, root, d->vert, d->frag, v, label, &sh->variants[v]))
            return result::ERR_INVALID;
    }

    g_reg.count++;
    LOG_INFO("shader: '%s' = %s + %s, %u variant(s)", d->name, d->vert, d->frag, wanted);
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

    Decl decls[SHADER_MAX_COUNT];
    u32 count = 0;
    const char *roles[ROLE_COUNT] = {};
    result::Error e = parse_manifest(doc.val, decls, SHADER_MAX_COUNT, &count, roles);
    for (u32 i = 0; i < count && e == result::OK; i++)
        e = build_one(a, scratch, root, &decls[i]);

    if (e != result::OK) {
        memory::temp_end(t);
        return e;
    }

    // parse_manifest checked every role names a declared shader, so this cannot fail
    for (u32 r = 0; r < ROLE_COUNT; r++)
        g_reg.roles[r] = MUST(find(roles[r]));
    memory::temp_end(t);

    LOG_INFO("shader: %u shader(s) loaded", g_reg.count);
    for (u32 r = 0; r < ROLE_COUNT; r++)
        LOG_INFO("shader:   %s = '%s'", ROLE_KEYS[r], name(g_reg.roles[r]));
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
    memset(g_reg.roles, 0, sizeof(g_reg.roles));
}

HandleResult find(const char *name_) {
    ASSERT(name_);
    for (u32 i = 0; i < g_reg.count; i++)
        if (strcmp(g_reg.items[i].name, name_) == 0)
            return HandleResult{.val = Handle{i}};

    LOG_ERR("shader: no shader named '%s' is declared", name_);
    return HandleResult{.err = result::ERR_NOT_FOUND};
}

Handle role_handle(Role r) {
    ASSERT_MSG(g_reg.count > 0, "shader::role_handle before init");
    ASSERT(r < ROLE_COUNT);
    return g_reg.roles[r];
}

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

const char *role_key(Role r) {
    ASSERT(r < ROLE_COUNT);
    return ROLE_KEYS[r];
}

} // namespace shader
