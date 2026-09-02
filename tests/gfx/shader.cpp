#include <string.h>

#include "gfx/shader.h"
#include "suites.h"

static config::Doc *manifest(memory::Arena *a, const char *text) {
    return MUST(config::parse(a, memory::push_str(a, text), "shaders.cfg"));
}

static result::Error parse(memory::Arena *a, const char *text, shader::Decl *out, u32 cap,
                           u32 *count, const char **dflt) {
    return shader::parse_manifest(manifest(a, text), out, cap, count, dflt);
}

static const char *GOOD = "default_shader mesh\n"
                          "shader mesh {\n"
                          "    vert mesh.vert\n"
                          "    frag mesh.frag\n"
                          "    features SKINNED ALPHA_MASK\n"
                          "}\n"
                          "shader flat {\n"
                          "    vert flat.vert\n"
                          "    frag flat.frag\n"
                          "}\n";

static void good_manifest_parses(memory::Arena *a) {
    shader::Decl decls[4];
    u32 count = 0;
    const char *dflt = NULL;

    CHECK(parse(a, GOOD, decls, 4, &count, &dflt) == result::OK);
    CHECK(count == 2);
    CHECK(strcmp(dflt, "mesh") == 0);
    CHECK(strcmp(decls[0].name, "mesh") == 0);
    CHECK(strcmp(decls[0].vert, "mesh.vert") == 0);
    CHECK(strcmp(decls[0].frag, "mesh.frag") == 0);
    CHECK(decls[0].supported == (shader::FEATURE_SKINNED | shader::FEATURE_ALPHA_MASK));
    CHECK(strcmp(decls[1].name, "flat") == 0);
    CHECK(decls[1].supported == shader::FEATURE_NONE);
}

static result::Error parse_bad(memory::Arena *a, const char *text, u32 cap) {
    shader::Decl decls[4];
    u32 count = 0;
    const char *dflt = NULL;

    test::mute();
    result::Error e = parse(a, text, decls, cap, &count, &dflt);
    CHECK(test::unmute() == 1);
    return e;
}

static void broken_manifests_are_refused(memory::Arena *a) {
    CHECK(parse_bad(a, "default_shader x\nshader x {\n    vert a\n    frag b\n    features GLOW\n}\n", 4) ==
          result::ERR_PARSE);
    CHECK(parse_bad(a, "default_shader x\nshader x {\n    vert a\n}\n", 4) == result::ERR_PARSE);
    CHECK(parse_bad(a, "default_shader x\nshader {\n    vert a\n    frag b\n}\n", 4) ==
          result::ERR_PARSE);
    CHECK(parse_bad(a, "shader x {\n    vert a\n    frag b\n}\n", 4) == result::ERR_PARSE);
    CHECK(parse_bad(a, "default_shader nope\nshader x {\n    vert a\n    frag b\n}\n", 4) ==
          result::ERR_NOT_FOUND);
    CHECK(parse_bad(a, "default_shader x\n", 4) == result::ERR_PARSE);
    CHECK(parse_bad(a, GOOD, 1) == result::ERR_EXHAUSTED);

    // a name at the cap is refused up front, not truncated
    char long_name[SHADER_NAME_CAP + 1];
    memset(long_name, 'x', SHADER_NAME_CAP);
    long_name[SHADER_NAME_CAP] = 0;
    const char *text = memory::push_fmt(
        a, "default_shader %s\nshader %s {\n    vert a\n    frag b\n}\n", long_name, long_name);
    CHECK(parse_bad(a, text, 4) == result::ERR_PARSE);
}

static void feature_names_round_trip(memory::Arena *) {
    for (u32 b = 0; b < SHADER_FEATURE_BITS; b++)
        CHECK(shader::feature_from_name(shader::feature_name(b)) == (1u << b));
    CHECK(shader::feature_from_name("GLOW") == shader::FEATURE_NONE);
    CHECK(shader::feature_name(SHADER_FEATURE_BITS) == NULL);
}

test::Suite shader_suite() {
    static const test::Case cases[] = {
        CASE(good_manifest_parses),
        CASE(broken_manifests_are_refused),
        CASE(feature_names_round_trip),
    };
    return test::Suite{.name = "shader", .cases = cases, .count = ARRAY_COUNT(cases)};
}
