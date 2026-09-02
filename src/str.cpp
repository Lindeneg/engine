#include "str.h"

#include <string.h>

namespace str {

i32 name_index(const char *const *names, u32 count, const char *s) {
    ASSERT(names && s);
    for (u32 i = 0; i < count; i++)
        if (strcmp(names[i], s) == 0)
            return (i32)i;
    return -1;
}

void canonical_name(char *dst, u64 cap, const char *src) {
    ASSERT(dst && cap);

    u64 i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            char c = src[i];
            b32 alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            dst[i] = alnum ? c : '_';
        }
    }
    dst[i] = 0;
}

} // namespace str
