#include "gfx/gfx.h"

#include "core/str.h"

namespace gfx {

static const char *CULL_NAMES[CULL_COUNT] = {"back", "none", "front"};
static const char *BLEND_NAMES[BLEND_COUNT] = {"none", "alpha", "add"};

const char *cull_name(CullMode m) { return m < CULL_COUNT ? CULL_NAMES[m] : "?"; }
const char *blend_name(BlendMode m) { return m < BLEND_COUNT ? BLEND_NAMES[m] : "?"; }

b32 cull_from_name(const char *s, CullMode *out) {
    ASSERT(out);
    i32 i = str::name_index(CULL_NAMES, CULL_COUNT, s);
    if (i < 0)
        return false;
    *out = (CullMode)i;
    return true;
}

b32 blend_from_name(const char *s, BlendMode *out) {
    ASSERT(out);
    i32 i = str::name_index(BLEND_NAMES, BLEND_COUNT, s);
    if (i < 0)
        return false;
    *out = (BlendMode)i;
    return true;
}

} // namespace gfx
