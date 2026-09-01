#include "gfx.h"

#include <string.h>

namespace gfx {

static const char *CULL_NAMES[CULL_COUNT] = {"back", "none", "front"};
static const char *BLEND_NAMES[BLEND_COUNT] = {"none", "alpha", "add"};

const char *cull_name(CullMode m) { return m < CULL_COUNT ? CULL_NAMES[m] : "?"; }
const char *blend_name(BlendMode m) { return m < BLEND_COUNT ? BLEND_NAMES[m] : "?"; }

b32 cull_from_name(const char *s, CullMode *out) {
    ASSERT(s && out);
    for (u32 i = 0; i < CULL_COUNT; i++) {
        if (strcmp(CULL_NAMES[i], s) != 0)
            continue;
        *out = (CullMode)i;
        return true;
    }
    return false;
}

b32 blend_from_name(const char *s, BlendMode *out) {
    ASSERT(s && out);
    for (u32 i = 0; i < BLEND_COUNT; i++) {
        if (strcmp(BLEND_NAMES[i], s) != 0)
            continue;
        *out = (BlendMode)i;
        return true;
    }
    return false;
}

} // namespace gfx
