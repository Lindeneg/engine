#pragma once

#include "common.h"

// Draw ordering for the render queue, GL-free so it can be tested. Opaque keys
// group by the most expensive state change first (program, state, mesh,
// material), depth in the low bits breaking ties front-to-back for early-Z.
// Transparent keys put distance first, far-to-near: ordering there is correctness.

// mesh and material indices get this many bits; pool indices have 24
#define DRAWKEY_INDEX_BITS 16
#define DRAWKEY_INDEX_MAX ((1u << DRAWKEY_INDEX_BITS) - 1)

// opaque depth is bucketed in whole units up to this
#define DRAWKEY_DEPTH_MAX 255u

namespace drawkey {

struct Entry {
    u64 key;
    u32 item;
};

u64 opaque(u32 program, u32 state, u32 mesh, u32 material, f32 dist);
u64 transparent(u32 program, u32 mesh, f32 dist);

// LSD radix on the full key: linear, stable. tmp must hold n entries.
void sort(Entry *a, Entry *tmp, u32 n);

} // namespace drawkey
