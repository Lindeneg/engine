#pragma once

#include "common.h"
#include "memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);
};

namespace file {

struct Descriptor {
    u8 *data;
    u64 size;
};

Descriptor read(memory::Arena *a, const char *path);
}; // namespace file
