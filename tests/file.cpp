#include <string.h>

#include "file.h"
#include "logger.h"
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
    logger::set_level(logger::FATAL); // file::read logs the miss, expected here
    file::BlobResult r = file::read(a, "definitely/not/a/real/path.glb");
    logger::set_level(logger::TRACE);

    CHECK(r.err == result::ERR_NOT_FOUND);
    CHECK(r.val.data == NULL);
    CHECK(r.val.size == 0);
}

// a failed read must roll the arena back rather than strand the buffer it pushed
static void failed_read_leaves_arena_untouched(memory::Arena *a) {
    u64 before = a->used;

    logger::set_level(logger::FATAL);
    file::BlobResult r = file::read(a, "definitely/not/a/real/path.glb");
    logger::set_level(logger::TRACE);

    CHECK(r.err != result::OK);
    CHECK(a->used == before);
}

test::Suite file_suite() {
    static const test::Case cases[] = {
        {"reads_existing_file", reads_existing_file},
        {"appends_nul_past_size", appends_nul_past_size},
        {"missing_file_reports_not_found", missing_file_reports_not_found},
        {"failed_read_leaves_arena_untouched", failed_read_leaves_arena_untouched},
    };
    return test::Suite{.name = "file", .cases = cases, .count = ARRAY_COUNT(cases)};
}
