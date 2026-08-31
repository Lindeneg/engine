#pragma once

#include "common.h"
#include "memory.h"

#define FPATH(a, ...) path::join((a), __VA_ARGS__, (const char *)NULL)

namespace path {
char *join(memory::Arena *a, ...);
}; // namespace path
