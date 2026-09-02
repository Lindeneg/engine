#pragma once

#include <stdarg.h>

#include "common.h"

#define ARENA_DEFAULT_ALIGN 16
#define ARENA_COMMIT_CHUNK KiB(64)

#define push_struct(a, T) (T *)memory::push((a), sizeof(T), alignof(T))
#define push_array(a, T, n) (T *)memory::push((a), sizeof(T) * (n), alignof(T))

// Non-zeroing variants. For bulk buffers that are fully overwritten right after
// the push (file reads, staging). Contents are undefined on return.
#define push_struct_nz(a, T) (T *)memory::push_nz((a), sizeof(T), alignof(T))
#define push_array_nz(a, T, n) (T *)memory::push_nz((a), sizeof(T) * (n), alignof(T))

namespace memory {
struct Arena {
    u8 *base;
    u64 cap;
    u64 used;
    u64 committed;
    u64 peak; // high-water mark, survives reset
};

struct ArenaTemp {
    Arena *arena;
    u64 used;
};

void init(u64 reserve);
Arena create(u64 cap);
void *push(Arena *a, size_t size, size_t align = ARENA_DEFAULT_ALIGN);
void *push_nz(Arena *a, size_t size, size_t align = ARENA_DEFAULT_ALIGN);

// Copies into the arena. For strings that have to outlive their source: map
// keys, names stored on a pool payload.
char *push_str(Arena *a, const char *s);
void *push_bytes(Arena *a, const void *src, u64 n);
char *push_fmt(Arena *a, const char *fmt, ...);
char *push_vfmt(Arena *a, const char *fmt, va_list ap);

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
