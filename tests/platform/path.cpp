#include "platform/path.h"

#include <string.h>

#include "suites.h"

static void joins_segments(memory::Arena *a) {
    CHECK(strcmp(FPATH(a, "assets", "characters", "soldier.glb"),
                 "assets/characters/soldier.glb") == 0);
    CHECK(strcmp(FPATH(a, "a", "b"), "a/b") == 0);
}

static void single_segment_gets_no_separator(memory::Arena *a) {
    CHECK(strcmp(FPATH(a, "solo"), "solo") == 0);
    CHECK(strcmp(FPATH(a, "assets/already/joined"), "assets/already/joined") == 0);
}

// segments are pasted verbatim; an empty or separator-bearing one shows up as-is
static void does_not_normalize(memory::Arena *a) {
    CHECK(strcmp(FPATH(a, "", "b"), "/b") == 0);
    CHECK(strcmp(FPATH(a, "a", ""), "a/") == 0);
    CHECK(strcmp(FPATH(a, "a/", "b"), "a//b") == 0);
}

static void result_is_nul_terminated(memory::Arena *a) {
    char *p = FPATH(a, "one", "two");
    CHECK(strlen(p) == 7);
    CHECK(p[7] == 0);
}

// how sidecar files are addressed
static void swaps_the_extension(memory::Arena *a) {
    CHECK(strcmp(path::swap_ext(a, "characters/soldier.glb", ".mat.cfg"),
                 "characters/soldier.mat.cfg") == 0);

    // no extension at all: append rather than mangle
    CHECK(strcmp(path::swap_ext(a, "weapons/pistol", ".mat.cfg"), "weapons/pistol.mat.cfg") == 0);

    // only the last dot of the final segment counts
    CHECK(strcmp(path::swap_ext(a, "a.b/c.d.glb", ".cfg"), "a.b/c.d.cfg") == 0);

    // a dot in a directory name is not this file's extension
    CHECK(strcmp(path::swap_ext(a, "v1.2/model", ".cfg"), "v1.2/model.cfg") == 0);

    CHECK(strcmp(path::swap_ext(a, "soldier.glb", ""), "soldier") == 0);
}

static void relative_to_strips_the_root(memory::Arena *) {
    char out[64];

    CHECK(path::relative_to("D:/games/assets", "D:/games/assets/weapons/pistol.glb", out,
                            sizeof(out)));
    CHECK(strcmp(out, "weapons/pistol.glb") == 0);

    // either slash, a trailing one on the root, and the OS's own spelling
    CHECK(path::relative_to("D:/games/assets/", "D:\\games\\assets\\weapons\\pistol.glb", out,
                            sizeof(out)));
    CHECK(strcmp(out, "weapons/pistol.glb") == 0);

    // outside the root, a sibling folder sharing the prefix, and the root itself
    CHECK(!path::relative_to("D:/games/assets", "D:/games/other/pistol.glb", out, sizeof(out)));
    CHECK(!path::relative_to("D:/games/assets", "D:/games/assets2/pistol.glb", out, sizeof(out)));
    CHECK(!path::relative_to("D:/games/assets", "D:/games/assets", out, sizeof(out)));

    // and it must fit
    char tiny[8];
    CHECK(!path::relative_to("D:/games/assets", "D:/games/assets/weapons/pistol.glb", tiny,
                             sizeof(tiny)));
}

static void basename_is_the_final_segment(memory::Arena *) {
    CHECK(strcmp(path::basename("D:/games/stuff"), "stuff") == 0);
    CHECK(strcmp(path::basename("D:\\games\\stuff"), "stuff") == 0);
    CHECK(strcmp(path::basename("stuff"), "stuff") == 0);
    CHECK(strcmp(path::basename("D:/games/"), "") == 0);
    CHECK(strcmp(path::basename(""), "") == 0);
}

test::Suite path_suite() {
    static const test::Case cases[] = {
        CASE(joins_segments),
        CASE(single_segment_gets_no_separator),
        CASE(does_not_normalize),
        CASE(result_is_nul_terminated),
        CASE(swaps_the_extension),
        CASE(relative_to_strips_the_root),
        CASE(basename_is_the_final_segment),
    };
    return test::Suite{.name = "path", .cases = cases, .count = ARRAY_COUNT(cases)};
}
