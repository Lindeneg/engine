#pragma once

#include "common.h"

#define ARENA_DEFAULT_ALIGN 16
#define ARENA_COMMIT_CHUNK KiB(64)

#define push_struct(a, T) (T *)arena_push((a), sizeof(T), alignof(T))
#define push_array(a, T, n) (T *)arena_push((a), sizeof(T) * (n), alignof(T))

namespace memory {
struct Arena {
    u8 *base;
    u64 cap;
    u64 used;
#if CL_MEM_STATS
    u64 committed;
    u64 peak; // high-water mark, survives reset
#endif
};

struct ArenaTemp {
    Arena *arena;
    u64 used;
};

void init(u64 reserve);
Arena create(u64 cap);
void *push(Arena *a, size_t size, size_t align = ARENA_DEFAULT_ALIGN);

void reset(Arena *a);
ArenaTemp temp_begin(Arena *a);
void temp_end(ArenaTemp t);

#if CL_MEM_STATS
struct Stats {
    u64 used, peak, committed, cap;
};

struct SysStats {
    u64 reserved, carved;
};

Stats stats(const Arena *a);
SysStats sys_stats();
void log_stats(const Arena *a, const char *name); // logs used/peak/committed/cap
void log_sys_stats();
#endif
} // namespace memory
