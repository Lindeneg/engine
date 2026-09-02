#include "drawkey.h"

#include <string.h>

namespace drawkey {

// Positive floats order the same way as their bit patterns read as integers, so
// this is an exact, range-free ordering -- no far plane to normalise against.
static u32 depth_bits(f32 d) {
    u32 bits;
    memcpy(&bits, &d, sizeof(bits));
    return d > 0.0f ? bits : 0;
}

u64 opaque(u32 program, u32 state, u32 mesh, u32 material, f32 dist) {
    ASSERT_MSG(mesh <= DRAWKEY_INDEX_MAX && material <= DRAWKEY_INDEX_MAX,
               "drawkey: pool index past the %d bits the key holds", DRAWKEY_INDEX_BITS);

    u64 depth = (u64)MIN((u32)MAX(dist, 0.0f), DRAWKEY_DEPTH_MAX);
    return ((u64)(program & 0xFFFF) << 48) | ((u64)(state & 0xFF) << 40) |
           ((u64)(mesh & DRAWKEY_INDEX_MAX) << 24) | ((u64)(material & DRAWKEY_INDEX_MAX) << 8) |
           depth;
}

u64 transparent(u32 program, u32 mesh, f32 dist) {
    ASSERT_MSG(mesh <= DRAWKEY_INDEX_MAX, "drawkey: pool index past the %d bits the key holds",
               DRAWKEY_INDEX_BITS);

    // inverted so ascending order comes out far-to-near
    u64 depth = ~(u64)depth_bits(dist) & 0xFFFFFFFFull;
    return (depth << 32) | ((u64)(program & 0xFFFF) << 16) | (u64)(mesh & DRAWKEY_INDEX_MAX);
}

void sort(Entry *a, Entry *tmp, u32 n) {
    if (n < 2)
        return;

    Entry *src = a;
    Entry *dst = tmp;

    for (u32 shift = 0; shift < 64; shift += 8) {
        u32 counts[256] = {};
        for (u32 i = 0; i < n; i++)
            counts[(src[i].key >> shift) & 0xFF]++;

        // every key shares this byte, so the pass would only copy
        if (counts[(src[0].key >> shift) & 0xFF] == n)
            continue;

        u32 sum = 0;
        for (u32 b = 0; b < 256; b++) {
            u32 c = counts[b];
            counts[b] = sum;
            sum += c;
        }

        for (u32 i = 0; i < n; i++)
            dst[counts[(src[i].key >> shift) & 0xFF]++] = src[i];

        Entry *swap = src;
        src = dst;
        dst = swap;
    }

    // an odd number of passes leaves the result in tmp
    if (src != a)
        memcpy(a, src, sizeof(Entry) * n);
}

} // namespace drawkey
