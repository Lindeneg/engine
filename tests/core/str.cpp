#include "core/str.h"

#include <string.h>

#include "suites.h"

static void separators_collapse_to_underscore(memory::Arena *) {
    char buf[64];

    str::canonical_name(buf, sizeof(buf), "mixamorig:Hips");
    CHECK(strcmp(buf, "mixamorig_Hips") == 0);

    str::canonical_name(buf, sizeof(buf), "a.b c-d:e");
    CHECK(strcmp(buf, "a_b_c_d_e") == 0);
}

// the whole point: the two exporters in our asset pipeline must agree
static void both_exporter_spellings_agree(memory::Arena *) {
    char fbx[64];
    char three[64];

    str::canonical_name(fbx, sizeof(fbx), "mixamorig:RightHandMiddle3");
    str::canonical_name(three, sizeof(three), "mixamorig_RightHandMiddle3");
    CHECK(strcmp(fbx, three) == 0);
}

static void is_idempotent(memory::Arena *) {
    char once[64];
    char twice[64];

    str::canonical_name(once, sizeof(once), "mixamorig:LeftHandThumb1");
    str::canonical_name(twice, sizeof(twice), once);
    CHECK(strcmp(once, twice) == 0);
}

static void case_is_preserved(memory::Arena *) {
    char buf[64];
    str::canonical_name(buf, sizeof(buf), "mixamorig:LeftUpLeg");
    CHECK(strcmp(buf, "mixamorig_LeftUpLeg") == 0);
}

static void truncates_without_overflow(memory::Arena *) {
    char buf[8];
    str::canonical_name(buf, sizeof(buf), "mixamorig:Hips");
    CHECK(strlen(buf) == 7);
    CHECK(strcmp(buf, "mixamor") == 0);
}

static void handles_empty_and_null(memory::Arena *) {
    char buf[16];

    str::canonical_name(buf, sizeof(buf), "");
    CHECK(buf[0] == 0);

    str::canonical_name(buf, sizeof(buf), NULL);
    CHECK(buf[0] == 0);
}

static void name_index_is_exact(memory::Arena *) {
    static const char *names[] = {"back", "none", "front"};

    CHECK(str::name_index(names, 3, "back") == 0);
    CHECK(str::name_index(names, 3, "front") == 2);
    CHECK(str::name_index(names, 3, "fron") == -1);
    CHECK(str::name_index(names, 3, "Front") == -1);
    CHECK(str::name_index(names, 0, "back") == -1);
}

static void utf8_decodes_and_recovers(memory::Arena *) {
    const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    CHECK(str::utf8_next(&s) == 'a');
    CHECK(str::utf8_next(&s) == 0xE9);
    CHECK(str::utf8_next(&s) == 0x20AC);
    CHECK(str::utf8_next(&s) == 0x1F600);
    CHECK(str::utf8_next(&s) == 0);

    // a stray continuation byte, then a truncated sequence, then a plain byte
    const char *bad = "\x80\xE2\x82"
                      "b";
    CHECK(str::utf8_next(&bad) == STR_REPLACEMENT_CODEPOINT);
    CHECK(str::utf8_next(&bad) == STR_REPLACEMENT_CODEPOINT);
    CHECK(str::utf8_next(&bad) == 'b');
    CHECK(str::utf8_next(&bad) == 0);
}

static void utf8_steps_back_and_encodes(memory::Arena *) {
    const char *s = "a\xC3\xA9" "b";
    CHECK(str::utf8_prev(s, 4) == 3);
    CHECK(str::utf8_prev(s, 3) == 1);
    CHECK(str::utf8_prev(s, 1) == 0);
    CHECK(str::utf8_prev(s, 0) == 0);

    const u32 points[] = {'a', 0xE9, 0x20AC, 0x1F600};
    for (u32 i = 0; i < ARRAY_COUNT(points); i++) {
        char out[STR_UTF8_MAX_BYTES + 1] = {};
        u32 n = str::utf8_encode(points[i], out);
        CHECK(n == i + 1);
        const char *p = out;
        CHECK(str::utf8_next(&p) == points[i]);
    }

    char out[STR_UTF8_MAX_BYTES];
    CHECK(str::utf8_encode(0x110000, out) == 3); // past the last codepoint: U+FFFD
}

test::Suite str_suite() {
    static const test::Case cases[] = {
        CASE(separators_collapse_to_underscore),
        CASE(both_exporter_spellings_agree),
        CASE(is_idempotent),
        CASE(case_is_preserved),
        CASE(truncates_without_overflow),
        CASE(handles_empty_and_null),
        CASE(name_index_is_exact),
        CASE(utf8_decodes_and_recovers),
        CASE(utf8_steps_back_and_encodes),
    };
    return test::Suite{.name = "str", .cases = cases, .count = ARRAY_COUNT(cases)};
}
