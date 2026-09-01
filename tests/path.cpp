#include <string.h>

#include "path.h"
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

// documents the contract rather than endorsing it: segments are pasted verbatim,
// so an empty or separator-bearing segment shows up in the output as-is
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

// how sidecar files are addressed, so getting it wrong silently means an
// override file that is never found
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

test::Suite path_suite() {
    static const test::Case cases[] = {
        {"joins_segments", joins_segments},
        {"single_segment_gets_no_separator", single_segment_gets_no_separator},
        {"does_not_normalize", does_not_normalize},
        {"result_is_nul_terminated", result_is_nul_terminated},
        {"swaps_the_extension", swaps_the_extension},
    };
    return test::Suite{.name = "path", .cases = cases, .count = ARRAY_COUNT(cases)};
}
