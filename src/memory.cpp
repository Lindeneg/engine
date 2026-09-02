#include "./memory.h"

#include <stdarg.h>
#include <string.h>

#include "logger.h"
#include "platform.h"

struct ArenaRoot {
    u8 *base;
    u64 reserved;
    u64 carved;
    u64 page_size;
};

static ArenaRoot g_arena_root;

static f64 to_mib(u64 bytes) { return (f64)bytes / (1024.0 * 1024.0); }

static void commit_for(memory::Arena *a, u64 needed) {
    if (needed <= a->committed)
        return;

    u64 want = ALIGN_UP_2(needed, ARENA_COMMIT_CHUNK);
    if (want > a->cap)
        want = a->cap; // cap is page-aligned, stays aligned

    b32 ok = platform::commit(a->base + a->committed, want - a->committed);
    ASSERT_MSG(ok, "commit of %llu bytes failed", (unsigned long long)(want - a->committed));

    a->committed = want;
}

static void *push_impl(memory::Arena *a, size_t size, size_t align, b32 zero) {
    ASSERT(a);
    ASSERT(align && (align & (align - 1)) == 0); // non power-of-two is a code bug

    u64 pos = ALIGN_UP_2(a->used, align);
    u64 new_used = pos + size;
    ASSERT_MSG(new_used <= a->cap, "arena exhausted: want %llu, cap %llu",
               (unsigned long long)new_used, (unsigned long long)a->cap);

    commit_for(a, new_used);
    a->used = new_used;
    if (a->used > a->peak)
        a->peak = a->used;
    u8 *ptr = a->base + pos;
    if (zero)
        memset(ptr, 0, size);
    return ptr;
}

namespace memory {
void init(u64 reserve) {
    ASSERT_MSG(!g_arena_root.base, "memory system already initialized");
    g_arena_root.page_size = platform::page_size();

    reserve = ALIGN_UP_2(reserve, platform::alloc_granularity());
    void *base = platform::reserve(reserve);
    ASSERT_MSG(base, "reserve of %llu bytes failed", (unsigned long long)reserve);

    g_arena_root.base = (u8 *)base;
    g_arena_root.reserved = reserve;
    g_arena_root.carved = 0;
    LOG_INFO("mem: reserved %.0f MiB virtual", to_mib(reserve));
}

Arena create(u64 cap) {
    ASSERT_MSG(g_arena_root.base, "memory::create before memory::init");
    cap = ALIGN_UP_2(cap, g_arena_root.page_size);
    ASSERT_MSG(g_arena_root.carved + cap <= g_arena_root.reserved,
               "arena budget exhausted: want %llu, %llu left", (unsigned long long)cap,
               (unsigned long long)(g_arena_root.reserved - g_arena_root.carved));

    Arena arena{
        .base = g_arena_root.base + g_arena_root.carved,
        .cap = cap,
    };

    g_arena_root.carved += cap;

    LOG_DEBUG("mem: arena created @ %p (%.2f MiB)", (void *)arena.base, to_mib(cap));
    return arena;
}

void *push(Arena *a, size_t size, size_t align) { return push_impl(a, size, align, true); }

void *push_nz(Arena *a, size_t size, size_t align) { return push_impl(a, size, align, false); }

char *push_str(Arena *a, const char *s) {
    ASSERT(s);
    return (char *)push_bytes(a, s, strlen(s) + 1);
}

void *push_bytes(Arena *a, const void *src, u64 n) {
    ASSERT(src || n == 0);
    void *out = push_impl(a, n, ARENA_DEFAULT_ALIGN, false);
    memcpy(out, src, n);
    return out;
}

char *push_fmt(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = push_vfmt(a, fmt, ap);
    va_end(ap);
    return out;
}

char *push_vfmt(Arena *a, const char *fmt, va_list ap) {
    ASSERT(fmt);

    va_list measure;
    va_copy(measure, ap);
    i32 n = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    ASSERT(n >= 0);

    char *out = (char *)push_impl(a, (u64)n + 1, ARENA_DEFAULT_ALIGN, false);
    vsnprintf(out, (u64)n + 1, fmt, ap);
    return out;
}

void reset(Arena *a) {
    ASSERT(a);
    a->used = 0;
}

ArenaTemp temp_begin(Arena *a) {
    ASSERT(a);
    return ArenaTemp{.arena = a, .used = a->used};
}

void temp_end(ArenaTemp t) {
    ASSERT(t.arena);
    // a mark above the current position means scopes ended out of order, or
    // the arena was reset underneath a live temp
    ASSERT_MSG(t.used <= t.arena->used, "temp_end mark %llu above arena position %llu",
               (unsigned long long)t.used, (unsigned long long)t.arena->used);
    t.arena->used = t.used;
}

#if CL_MEM_STATS
Stats stats(const Arena *a) {
    return Stats{.used = a->used, .peak = a->peak, .committed = a->committed, .cap = a->cap};
}

SysStats sys_stats(void) {
    return SysStats{.reserved = g_arena_root.reserved, .carved = g_arena_root.carved};
}

void log_stats(const Arena *a, const char *name) {
    f64 pct = a->cap ? 100.0 * (f64)a->peak / (f64)a->cap : 0.0;
    LOG_INFO("mem %-10s @ %p used %.2f / peak %.2f / committed %.2f / cap %.2f MiB (peak %.1f%%)",
             name, (void *)a->base, to_mib(a->used), to_mib(a->peak), to_mib(a->committed),
             to_mib(a->cap), pct);
}

void log_sys_stats() {
    LOG_INFO("mem %-10s carved %.2f / reserved %.2f MiB", "system", to_mib(g_arena_root.carved),
             to_mib(g_arena_root.reserved));
}
#endif
}; // namespace memory
