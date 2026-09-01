#pragma once

#include "common.h"

// Graphics vocabulary shared by the asset layer and the renderer. No GL here,
// deliberately: asset:: must stay free of it, and asset.h cannot include
// render.h without a cycle. These are values a material carries and a draw
// applies -- the translation to GL calls happens once, in render.cpp.

namespace gfx {

enum CullMode { CULL_BACK, CULL_NONE, CULL_FRONT, CULL_COUNT };
enum BlendMode { BLEND_NONE, BLEND_ALPHA, BLEND_ADD, BLEND_COUNT };

struct RenderState {
    CullMode cull;
    BlendMode blend;
    b8 depth_test;
    b8 depth_write;
};

// The default a material starts from. Not the zeroed struct: zeroed leaves
// depth_test and depth_write false, which is silently wrong everywhere.
inline RenderState state_default() {
    return RenderState{CULL_BACK, BLEND_NONE, true, true};
}

// Packed into 6 bits for the draw queue's sort key. Grouping by state is what
// stops a flush from toggling GL state between adjacent draws.
inline u32 state_key(RenderState s) {
    return ((u32)s.cull << 4) | ((u32)s.blend << 2) | ((u32)(s.depth_test ? 1 : 0) << 1) |
           (u32)(s.depth_write ? 1 : 0);
}

// Name tables. A material config reads "cull back", and an editor offers the
// list from here rather than keeping a second copy of it.
const char *cull_name(CullMode m);
const char *blend_name(BlendMode m);
b32 cull_from_name(const char *s, CullMode *out);
b32 blend_from_name(const char *s, BlendMode *out);

} // namespace gfx
