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

// runs every case inside its own temp scope on scratch; returns failed case count
i32 run(const Suite *suites, u64 count, memory::Arena *scratch);

} // namespace test

#define CHECK(expr) test::check((expr) != 0, __FILE__, __LINE__, #expr)
