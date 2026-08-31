#include <string.h>

#include "gltf.h"
#include "suites.h"

static void separators_collapse_to_underscore(memory::Arena *) {
    char buf[64];

    gltf::canonical_name(buf, sizeof(buf), "mixamorig:Hips");
    CHECK(strcmp(buf, "mixamorig_Hips") == 0);

    gltf::canonical_name(buf, sizeof(buf), "a.b c-d:e");
    CHECK(strcmp(buf, "a_b_c_d_e") == 0);
}

// the whole point: the two exporters in our asset pipeline must agree
static void both_exporter_spellings_agree(memory::Arena *) {
    char fbx[64];
    char three[64];

    gltf::canonical_name(fbx, sizeof(fbx), "mixamorig:RightHandMiddle3");
    gltf::canonical_name(three, sizeof(three), "mixamorig_RightHandMiddle3");
    CHECK(strcmp(fbx, three) == 0);
}

static void is_idempotent(memory::Arena *) {
    char once[64];
    char twice[64];

    gltf::canonical_name(once, sizeof(once), "mixamorig:LeftHandThumb1");
    gltf::canonical_name(twice, sizeof(twice), once);
    CHECK(strcmp(once, twice) == 0);
}

static void case_is_preserved(memory::Arena *) {
    char buf[64];
    gltf::canonical_name(buf, sizeof(buf), "mixamorig:LeftUpLeg");
    CHECK(strcmp(buf, "mixamorig_LeftUpLeg") == 0);
}

static void truncates_without_overflow(memory::Arena *) {
    char buf[8];
    gltf::canonical_name(buf, sizeof(buf), "mixamorig:Hips");
    CHECK(strlen(buf) == 7);
    CHECK(strcmp(buf, "mixamor") == 0);
}

static void handles_empty_and_null(memory::Arena *) {
    char buf[16];

    gltf::canonical_name(buf, sizeof(buf), "");
    CHECK(buf[0] == 0);

    gltf::canonical_name(buf, sizeof(buf), NULL);
    CHECK(buf[0] == 0);
}

test::Suite gltf_suite() {
    static const test::Case cases[] = {
        {"separators_collapse_to_underscore", separators_collapse_to_underscore},
        {"both_exporter_spellings_agree", both_exporter_spellings_agree},
        {"is_idempotent", is_idempotent},
        {"case_is_preserved", case_is_preserved},
        {"truncates_without_overflow", truncates_without_overflow},
        {"handles_empty_and_null", handles_empty_and_null},
    };
    return test::Suite{.name = "gltf", .cases = cases, .count = ARRAY_COUNT(cases)};
}
