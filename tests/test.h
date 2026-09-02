#pragma once

#include "common.h"
#include "memory.h"

namespace test {

typedef void (*Fn)(memory::Arena *scratch);

struct Case {
    const char *name;
    Fn fn;
};

struct Suite {
    const char *name;
    const Case *cases;
    u64 count;
};

void check(b32 cond, const char *file, i32 line, const char *expr);

// Silences the logger around a path that is expected to warn or error.
// unmute() returns how many WARN+ERR messages the path produced, so a case
// can assert that it actually did. Nests badly on purpose: do not.
void mute();
u32 unmute();

// runs every case inside its own temp scope on scratch; returns failed case count
i32 run(const Suite *suites, u64 count, memory::Arena *scratch);

} // namespace test

#define CHECK(expr) test::check((expr) != 0, __FILE__, __LINE__, #expr)

// A case-table entry whose name cannot drift from its function.
#define CASE(fn) {#fn, fn}
