#include "./memory.h"

#include <string.h>

#include "logger.h"

struct ArenaRoot {
    u8 *base;
    u64 reserved;
    u64 carved;
    u64 page_size;
};

static ArenaRoot g_arena_root;

static f64 to_mib(u64 bytes) { return (f64)bytes / (1024.0 * 1024.0); }

#if PLATFORM_WINDOWS
#include <windows.h>
#elif PLATFORM_LINUX
#include <sys/mman.h>
#include <unistd.h>
#endif

static u64 os_page_size() {
#if PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#elif PLATFORM_LINUX
    return (u64)sysconf(_SC_PAGESIZE);
#endif
}

static u64 os_alloc_granularity() {
#if PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity;
#elif PLATFORM_LINUX
    return (u64)sysconf(_SC_PAGESIZE);
#endif
}

static void *os_reserve(u64 size) {
#if PLATFORM_WINDOWS
    return VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
#elif PLATFORM_LINUX
    void *p = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

static b32 os_commit(void *ptr, u64 size) {
#if PLATFORM_WINDOWS
    return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#elif PLATFORM_LINUX
    return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

static void commit_for(memory::Arena *a, u64 needed) {
    if (needed <= a->committed)
        return;

    u64 want = ALIGN_UP_2(needed, ARENA_COMMIT_CHUNK);
    if (want > a->cap)
        want = a->cap; // cap is page-aligned, stays aligned

    b32 ok = os_commit(a->base + a->committed, want - a->committed);
    ASSERT_MSG(ok, "commit of %llu bytes failed", (unsigned long long)(want - a->committed));

    a->committed = want;
}

namespace memory {
void init(u64 reserve) {
    ASSERT_MSG(!g_arena_root.base, "memory system already initialized");
    g_arena_root.page_size = os_page_size();

    reserve = ALIGN_UP_2(reserve, os_alloc_granularity());
    void *base = os_reserve(reserve);
    ASSERT_MSG(base, "reserve of %llu bytes failed", (unsigned long long)reserve);

    g_arena_root.base = (u8 *)base;
    g_arena_root.reserved = reserve;
    g_arena_root.carved = 0;
    LOG_INFO("mem: reserved %.0f MiB virtual", to_mib(reserve));
}

Arena create(u64 cap) {
    cap = ALIGN_UP_2(cap, g_arena_root.page_size);
    ASSERT_MSG(g_arena_root.carved + cap <= g_arena_root.reserved,
               "arena budget exhausted: want %llu, %llu left", (unsigned long long)cap,
               (unsigned long long)(g_arena_root.reserved - g_arena_root.carved));

    Arena arena{
        .base = g_arena_root.base + g_arena_root.carved,
        .cap = cap,
    };

    g_arena_root.carved += cap;

    LOG_DEBUG("mem: arena created @ %p (%.0f MiB)", (void *)arena.base, to_mib(cap));
    return arena;
}

void *push(Arena *a, size_t size, size_t align) {
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
    memset(ptr, 0, size);
    return ptr;
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
