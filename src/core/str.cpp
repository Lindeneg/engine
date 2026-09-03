#include "core/str.h"

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

// lead byte thresholds and the continuation pattern
#define UTF8_TWO 0xC0u
#define UTF8_THREE 0xE0u
#define UTF8_FOUR 0xF0u
#define UTF8_CONTINUATION 0x80u
#define UTF8_CONTINUATION_MASK 0xC0u
#define UTF8_MAX_CODEPOINT 0x10FFFFu

u32 utf8_next(const char **s) {
    ASSERT(s && *s);

    u8 c = (u8)**s;
    if (!c)
        return 0;
    (*s)++;

    u32 extra = 0;
    u32 cp = c;
    if (c >= UTF8_FOUR) {
        extra = 3;
        cp = c & 0x07u;
    } else if (c >= UTF8_THREE) {
        extra = 2;
        cp = c & 0x0Fu;
    } else if (c >= UTF8_TWO) {
        extra = 1;
        cp = c & 0x1Fu;
    } else if (c >= UTF8_CONTINUATION) {
        return STR_REPLACEMENT_CODEPOINT; // a stray continuation byte
    }

    for (u32 i = 0; i < extra; i++) {
        u8 cc = (u8)**s;
        if ((cc & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION)
            return STR_REPLACEMENT_CODEPOINT; // truncated; the offending byte is read next
        (*s)++;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    return cp;
}

u32 utf8_prev(const char *s, u32 offset) {
    ASSERT(s);
    if (offset == 0)
        return 0;
    offset--;
    while (offset > 0 && ((u8)s[offset] & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION)
        offset--;
    return offset;
}

u32 utf8_encode(u32 cp, char *out) {
    ASSERT(out);
    if (cp > UTF8_MAX_CODEPOINT)
        cp = STR_REPLACEMENT_CODEPOINT;

    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(UTF8_TWO | (cp >> 6));
        out[1] = (char)(UTF8_CONTINUATION | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(UTF8_THREE | (cp >> 12));
        out[1] = (char)(UTF8_CONTINUATION | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(UTF8_CONTINUATION | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(UTF8_FOUR | (cp >> 18));
    out[1] = (char)(UTF8_CONTINUATION | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(UTF8_CONTINUATION | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(UTF8_CONTINUATION | (cp & 0x3Fu));
    return 4;
}

} // namespace str
