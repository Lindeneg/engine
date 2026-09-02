#include <string.h>

#include "platform/file.h"
#include "suites.h"

// these read a file from the repo root, so the runner has to be invoked from there
#define KNOWN_FILE "CMakeLists.txt"

static void reads_existing_file(memory::Arena *a) {
    file::BlobResult r = file::read(a, KNOWN_FILE);
    CHECK(r.err == result::OK);
    CHECK(r.val.data != NULL);
    CHECK(r.val.size > 0);
}

static void appends_nul_past_size(memory::Arena *a) {
    file::Blob b{MUST(file::read(a, KNOWN_FILE))};
    CHECK(b.data[b.size] == 0);
    CHECK(strlen((const char *)b.data) == b.size); // no interior NUL in a text file
}

static void missing_file_reports_not_found(memory::Arena *a) {
    test::mute();
    file::BlobResult r = file::read(a, "definitely/not/a/real/path.glb");
    CHECK(test::unmute() == 1); // read logs the miss

    CHECK(r.err == result::ERR_NOT_FOUND);
    CHECK(r.val.data == NULL);
    CHECK(r.val.size == 0);
}

// a failed read must roll the arena back rather than strand the buffer it pushed
static void failed_read_leaves_arena_untouched(memory::Arena *a) {
    u64 before = a->used;

    test::mute();
    file::BlobResult r = file::read(a, "definitely/not/a/real/path.glb");
    test::unmute();

    CHECK(r.err != result::OK);
    CHECK(a->used == before);
}

test::Suite file_suite() {
    static const test::Case cases[] = {
        CASE(reads_existing_file),
        CASE(appends_nul_past_size),
        CASE(missing_file_reports_not_found),
        CASE(failed_read_leaves_arena_untouched),
    };
    return test::Suite{.name = "file", .cases = cases, .count = ARRAY_COUNT(cases)};
}
