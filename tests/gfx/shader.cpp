#include "gfx/shader.h"

#include <string.h>

#include "suites.h"

static config::Doc *manifest(memory::Arena *a, const char *text) {
    return MUST(config::parse(a, memory::push_str(a, text), "shaders.cfg"));
}

static result::Error parse(memory::Arena *a, const char *text, shader::Decl *out, u32 cap,
                           u32 *count, const char **roles) {
    return shader::parse_manifest(manifest(a, text), out, cap, count, roles);
}

#define SHADER_X "shader x {\n    vert a\n    frag b\n}\n"

// every role naming `name`, except `skip` (ROLE_COUNT skips none) naming `instead`
static const char *role_lines(memory::Arena *a, u32 skip, const char *instead) {
    char *text = memory::push_str(a, "");
    for (u32 r = 0; r < shader::ROLE_COUNT; r++) {
        const char *name = r == skip ? instead : "x";
        if (!name)
            continue;
        text = memory::push_fmt(a, "%s%s %s\n", text, shader::role_key((shader::Role)r), name);
    }
    return text;
}

static const char *good(memory::Arena *a) {
    return memory::push_fmt(a,
                            "%s"
                            "shader mesh {\n"
                            "    vert mesh.vert\n"
                            "    frag mesh.frag\n"
                            "    features SKINNED ALPHA_MASK\n"
                            "}\n"
                            "shader x {\n"
                            "    vert x.vert\n"
                            "    frag x.frag\n"
                            "}\n",
                            role_lines(a, shader::ROLE_DEFAULT, "mesh"));
}

static void good_manifest_parses(memory::Arena *a) {
    shader::Decl decls[4];
    u32 count = 0;
    const char *roles[shader::ROLE_COUNT] = {};

    CHECK(parse(a, good(a), decls, 4, &count, roles) == result::OK);
    CHECK(count == 2);
    CHECK(strcmp(roles[shader::ROLE_DEFAULT], "mesh") == 0);
    for (u32 r = 1; r < shader::ROLE_COUNT; r++)
        CHECK(strcmp(roles[r], "x") == 0);
    CHECK(strcmp(decls[0].name, "mesh") == 0);
    CHECK(strcmp(decls[0].vert, "mesh.vert") == 0);
    CHECK(strcmp(decls[0].frag, "mesh.frag") == 0);
    CHECK(decls[0].supported == (shader::FEATURE_SKINNED | shader::FEATURE_ALPHA_MASK));
    CHECK(strcmp(decls[1].name, "x") == 0);
    CHECK(decls[1].supported == shader::FEATURE_NONE);
}

static result::Error parse_bad(memory::Arena *a, const char *text, u32 cap) {
    shader::Decl decls[4];
    u32 count = 0;
    const char *roles[shader::ROLE_COUNT] = {};

    test::mute();
    result::Error e = parse(a, text, decls, cap, &count, roles);
    CHECK(test::unmute() == 1);
    return e;
}

static const char *with_roles(memory::Arena *a, const char *body) {
    return memory::push_fmt(a, "%s%s", role_lines(a, shader::ROLE_COUNT, NULL), body);
}

static void broken_manifests_are_refused(memory::Arena *a) {
    CHECK(parse_bad(a, with_roles(a, "shader x {\n    vert a\n    frag b\n    features GLOW\n}\n"),
                    4) == result::ERR_PARSE);
    CHECK(parse_bad(a, with_roles(a, "shader x {\n    vert a\n}\n"), 4) == result::ERR_PARSE);
    CHECK(parse_bad(a, with_roles(a, "shader {\n    vert a\n    frag b\n}\n"), 4) ==
          result::ERR_PARSE);
    CHECK(parse_bad(a, with_roles(a, ""), 4) == result::ERR_PARSE);
    CHECK(parse_bad(a, good(a), 1) == result::ERR_EXHAUSTED);

    // every role must be named, and name a declared shader
    for (u32 r = 0; r < shader::ROLE_COUNT; r++) {
        const char *missing = memory::push_fmt(a, "%s" SHADER_X, role_lines(a, r, NULL));
        CHECK(parse_bad(a, missing, 4) == result::ERR_PARSE);

        const char *undeclared = memory::push_fmt(a, "%s" SHADER_X, role_lines(a, r, "nope"));
        CHECK(parse_bad(a, undeclared, 4) == result::ERR_NOT_FOUND);
    }

    // a name at the cap is refused up front, not truncated
    char long_name[SHADER_NAME_CAP + 1];
    memset(long_name, 'x', SHADER_NAME_CAP);
    long_name[SHADER_NAME_CAP] = 0;
    const char *text = memory::push_fmt(a, "%sshader %s {\n    vert a\n    frag b\n}\n",
                                        role_lines(a, shader::ROLE_COUNT, NULL), long_name);
    CHECK(parse_bad(a, text, 4) == result::ERR_PARSE);
}

static void feature_names_round_trip(memory::Arena *) {
    for (u32 b = 0; b < SHADER_FEATURE_BITS; b++)
        CHECK(shader::feature_from_name(shader::feature_name(b)) == (1u << b));
    CHECK(shader::feature_from_name("GLOW") == shader::FEATURE_NONE);
    CHECK(shader::feature_name(SHADER_FEATURE_BITS) == NULL);
}

static void role_keys_are_the_manifest_keys(memory::Arena *) {
    CHECK(strcmp(shader::role_key(shader::ROLE_DEFAULT), "default_shader") == 0);
    CHECK(strcmp(shader::role_key(shader::ROLE_DEBUG), "debug_shader") == 0);
    CHECK(strcmp(shader::role_key(shader::ROLE_CANVAS), "canvas_shader") == 0);
}

test::Suite shader_suite() {
    static const test::Case cases[] = {
        CASE(good_manifest_parses),
        CASE(broken_manifests_are_refused),
        CASE(feature_names_round_trip),
        CASE(role_keys_are_the_manifest_keys),
    };
    return test::Suite{.name = "shader", .cases = cases, .count = ARRAY_COUNT(cases)};
}
