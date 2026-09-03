#pragma once

#include "core/common.h"

// A GPU texture: created from bytes once, bound to a unit, destroyed. Where the
// bytes come from is not its business; a font bakes them, a material will read
// them from an image.

namespace texture {

enum Format {
    FORMAT_RGBA8,
    FORMAT_R8,
    FORMAT_COVERAGE, // one channel, read as (1, 1, 1, r): a glyph atlas, a mask
    FORMAT_COUNT,
};

enum Filter { FILTER_NEAREST, FILTER_LINEAR, FILTER_COUNT };

struct Texture {
    u32 gl; // 0 when not created
    u32 width;
    u32 height;
    Format format;
};

// Uploads once, clamped at the edges, no mipmaps. Needs a context.
Texture create(u32 width, u32 height, Format format, Filter filter, const void *pixels);
void destroy(Texture *t);

void bind(const Texture *t, u32 unit);

} // namespace texture
