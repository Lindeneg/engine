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

test::Suite path_suite() {
    static const test::Case cases[] = {
        {"joins_segments", joins_segments},
        {"single_segment_gets_no_separator", single_segment_gets_no_separator},
        {"does_not_normalize", does_not_normalize},
        {"result_is_nul_terminated", result_is_nul_terminated},
    };
    return test::Suite{.name = "path", .cases = cases, .count = ARRAY_COUNT(cases)};
}
