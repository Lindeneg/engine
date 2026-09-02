#pragma once

#include "core/common.h"

// Graphics vocabulary a material carries and a draw applies; GL-free so asset::
// can hold it. render.cpp translates to GL once.

namespace gfx {

enum CullMode { CULL_BACK, CULL_NONE, CULL_FRONT, CULL_COUNT };
enum BlendMode { BLEND_NONE, BLEND_ALPHA, BLEND_ADD, BLEND_COUNT };

struct RenderState {
    CullMode cull;
    BlendMode blend;
    b8 depth_test;
    b8 depth_write;
};

// not the zeroed struct: that disables depth test and write
inline RenderState state_default() {
    return RenderState{CULL_BACK, BLEND_NONE, true, true};
}

// 6 bits for the draw queue's sort key
inline u32 state_key(RenderState s) {
    return ((u32)s.cull << 4) | ((u32)s.blend << 2) | ((u32)(s.depth_test ? 1 : 0) << 1) |
           (u32)(s.depth_write ? 1 : 0);
}

// name tables for config files and the editor
const char *cull_name(CullMode m);
const char *blend_name(BlendMode m);
b32 cull_from_name(const char *s, CullMode *out);
b32 blend_from_name(const char *s, BlendMode *out);

} // namespace gfx
