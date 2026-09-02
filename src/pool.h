#pragma once

#include "common.h"
#include "memory.h"

// Fixed-capacity slot allocator with generation counters over caller-owned
// payload bytes, shared by assets and entities. No lifetime policy: `scope` is
// a byte the caller gives meaning to. Index 0 is reserved, so alloc returns 0
// for "full" and handle 0 is never live. (The asset manager puts a fallback in
// slot 0; nothing here requires that.)

#define POOL_EPOCH_SHIFT 24
#define POOL_INDEX_MASK ((1u << POOL_EPOCH_SHIFT) - 1)

namespace pool {

// The encoding is public so handles can be serialized and printed.
inline u32 handle_index(u32 v) { return v & POOL_INDEX_MASK; }
inline u8 handle_epoch(u32 v) { return (u8)(v >> POOL_EPOCH_SHIFT); }

struct Slot {
    u8 epoch;
    b8 used;
    u8 scope;

    // a stale handle is dereferenced every frame by whoever holds it; warn once per generation
    b8 warned;
};

struct Pool {
    u8 *items;
    Slot *slots;
    u32 cap;
    u64 stride;
    u32 live; // slots in use, not counting the reserved index 0

    // FIFO, not LIFO: a freed slot goes to the back, so with an 8-bit epoch its
    // stale handles do not resolve again after 256 create/delete cycles
    u32 *free_ring;
    u32 free_head;  // next index to hand out
    u32 free_count; // indices waiting in the ring

    // an index past cap has no slot to carry a warned flag, so it gets one here
    b8 warned_range;
};

Pool create(memory::Arena *a, u32 cap, u64 stride);

// Zeroes the payload and returns the index, or 0 when the pool is full.
u32 alloc(Pool *p, u8 scope);

void release(Pool *p, u32 index);
void release_scope(Pool *p, u8 scope);

// Unchecked: for slot 0 and for an index alloc just returned.
void *at(Pool *p, u32 index);

// Packs the slot's current generation into a handle value.
u32 make_handle(const Pool *p, u32 index);

b32 alive(const Pool *p, u32 handle);

// The payload for a live handle, else NULL.
void *resolve(Pool *p, u32 handle);

// resolve(), but substituting slot 0 and warning once per slot instead of
// returning NULL. Handle 0 substitutes silently, since it means "unset".
void *resolve_or_zero(Pool *p, u32 handle, const char *what);

// Live slots; 0 when empty or at the end. Indices, so a caller can hold one across an alloc.
u32 first(const Pool *p);
u32 next(const Pool *p, u32 index);

} // namespace pool
