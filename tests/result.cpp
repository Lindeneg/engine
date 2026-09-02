#include "result.h"
#include "suites.h"

RESULT_TYPE(u32, U32Result);

static U32Result halve(u32 v) {
    if (v & 1)
        return U32Result{.err = result::ERR_INVALID};
    return U32Result{.val = v / 2};
}

static U32Result quarter(u32 v) {
    TRY(h, halve(v));
    return halve(h);
}

static void must_unwraps(memory::Arena *) {
    u32 v{MUST(halve(8))};
    CHECK(v == 4);
}

static void or_else_falls_back(memory::Arena *) {
    u32 good{OR_ELSE(halve(8), 99u)};
    CHECK(good == 4);

    test::mute();
    u32 bad{OR_ELSE(halve(7), 99u)};
    CHECK(test::unmute() == 1); // OR_ELSE warns on the failing path, by design
    CHECK(bad == 99);
}

static void try_propagates(memory::Arena *) {
    U32Result ok = quarter(20);
    CHECK(ok.err == result::OK);
    CHECK(ok.val == 5);

    U32Result first = quarter(5); // fails inside the TRY
    CHECK(first.err == result::ERR_INVALID);
    CHECK(first.val == 0);

    U32Result second = quarter(6); // TRY passes, the tail call fails
    CHECK(second.err == result::ERR_INVALID);
}

static void str_covers_every_code(memory::Arena *) {
    const result::Error codes[] = {
        result::OK,          result::ERR_NOT_FOUND, result::ERR_IO,        result::ERR_PARSE,
        result::ERR_UNSUPPORTED, result::ERR_INVALID, result::ERR_EXHAUSTED,
    };
    for (u64 i = 0; i < ARRAY_COUNT(codes); i++) {
        const char *s = result::str(codes[i]);
        CHECK(s != NULL);
        CHECK(s[0] != '?'); // the fallthrough sentinel means a code lost its case
    }
}

test::Suite result_suite() {
    static const test::Case cases[] = {
        CASE(must_unwraps),
        CASE(or_else_falls_back),
        CASE(try_propagates),
        CASE(str_covers_every_code),
    };
    return test::Suite{.name = "result", .cases = cases, .count = ARRAY_COUNT(cases)};
}
