#pragma once

#include "core/common.h"
#include "core/memory.h"

namespace strmap {

struct Slot {
    const char *key;
    u64 hash;
    u32 value;
};

struct Map {
    Slot *slots;
    u64 cap;
    u64 mask;
    u64 count;
};

Map create(memory::Arena *a, u64 max_entries);

b32 insert(Map *m, const char *key, u32 value);
u32 *find(Map *m, const char *key);
b32 remove(Map *m, const char *key);
void clear(Map *m);

} // namespace strmap
