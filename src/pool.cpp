#include "pool.h"

#include <string.h>

#include "logger.h"

namespace pool {

Pool create(memory::Arena *a, u32 cap, u64 stride) {
    ASSERT(a && cap > 0 && stride > 0);

    Pool p = {
        .items = (u8 *)memory::push(a, (u64)cap * stride),
        .slots = push_array(a, Slot, cap),
        .cap = cap,
        .stride = stride,
    };

    p.slots[0].used = true; // reserved, never handed out
    return p;
}

void *at(Pool *p, u32 index) {
    ASSERT(p && index < p->cap);
    return p->items + (u64)index * p->stride;
}

u32 make_handle(const Pool *p, u32 index) {
    ASSERT(p && index < p->cap);
    return ((u32)p->slots[index].epoch << POOL_EPOCH_SHIFT) | index;
}

u32 alloc(Pool *p, u8 scope) {
    ASSERT(p);

    for (u32 i = 1; i < p->cap; i++) {
        if (p->slots[i].used)
            continue;

        p->slots[i].used = true;
        p->slots[i].scope = scope;
        p->slots[i].warned = false;
        p->live++;
        memset(at(p, i), 0, p->stride);
        return i;
    }

    return 0;
}

void release(Pool *p, u32 index) {
    ASSERT(p);

    if (index == 0 || index >= p->cap || !p->slots[index].used)
        return;

    p->slots[index].used = false;
    p->slots[index].epoch++; // wraps at 256; a stale handle collides only after that many reuses
    p->live--;
}

void release_scope(Pool *p, u8 scope) {
    ASSERT(p);

    for (u32 i = 1; i < p->cap; i++)
        if (p->slots[i].used && p->slots[i].scope == scope)
            release(p, i);
}

b32 alive(const Pool *p, u32 handle) {
    ASSERT(p);

    u32 idx = pool_handle_index(handle);
    return idx != 0 && idx < p->cap && p->slots[idx].used &&
           p->slots[idx].epoch == pool_handle_epoch(handle);
}

void *resolve(Pool *p, u32 handle) {
    return alive(p, handle) ? at(p, pool_handle_index(handle)) : NULL;
}

void *resolve_or_zero(Pool *p, u32 handle, const char *what) {
    ASSERT(p);

    if (alive(p, handle))
        return at(p, pool_handle_index(handle));

    u32 idx = pool_handle_index(handle);
    if (handle != 0 && idx < p->cap && !p->slots[idx].warned) {
        p->slots[idx].warned = true;
        LOG_WARN("pool: stale %s handle %u, substituting slot 0", what, handle);
    }

    return at(p, 0);
}

u32 first(const Pool *p) { return next(p, 0); }

u32 next(const Pool *p, u32 index) {
    ASSERT(p);

    for (u32 i = index + 1; i < p->cap; i++)
        if (p->slots[i].used)
            return i;
    return 0;
}

} // namespace pool
