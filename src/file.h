#pragma once

#include "common.h"
#include "memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);
};

namespace file {

struct Blob {
    u8 *data;
    u64 size;
};

inline b32 ok(Blob b) { return b.data != NULL; }

Blob read(memory::Arena *a, const char *path);
}; // namespace file
