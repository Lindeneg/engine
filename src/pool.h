#pragma once

#include "common.h"
#include "memory.h"

// Fixed-capacity slot allocator with generation counters, over caller-owned
// payload bytes. Extracted from the asset manager because entities and their
// components need exactly the same thing: a stable u32 handle that stops
// answering once its slot has been recycled.
//
// It carries no lifetime policy of its own. `scope` is a byte the caller
// assigns meaning to, and the payload is the caller's to fill.
//
// Index 0 is reserved and never handed out, which buys two things at once: 0
// works as the "pool is full" answer from alloc, and handle 0 is never a live
// entity. The asset manager additionally writes a drawable fallback into slot
// 0 and substitutes it on a stale lookup; nothing here requires that.

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

    // A stale handle is usually dereferenced every frame by whatever still
    // holds it, so reporting it every time buries the first occurrence under
    // thousands of copies. One line per slot, per generation.
    b8 warned;
};

struct Pool {
    u8 *items;
    Slot *slots;
    u32 cap;
    u64 stride;
    u32 live; // slots in use, not counting the reserved index 0

    // Free indices as a FIFO ring, so alloc is O(1) and a freed slot goes to
    // the back of the line. LIFO would hand the same slot straight back on
    // every create/delete cycle, and with an 8-bit epoch that slot's stale
    // handles would start resolving again after 256 cycles.
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

// Iteration over live slots. first() returns 0 when the pool is empty, and
// next() returns 0 at the end. Indices, not pointers, so a caller can hold one
// across a call that allocates.
u32 first(const Pool *p);
u32 next(const Pool *p, u32 index);

} // namespace pool
