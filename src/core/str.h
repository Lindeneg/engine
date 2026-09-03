#pragma once

#include "core/common.h"

namespace str {

// Index of s in names, or -1. For the enum <-> name tables config files use.
i32 name_index(const char *const *names, u32 count, const char *s);

// Non-alphanumerics are dropped, so "mixamorig:Hips", "mixamorig_Hips" and
// "mixamorigHips" all compare equal: three exporters, one joint. Truncates to
// cap - 1; NULL reads as "".
void canonical_name(char *dst, u64 cap, const char *src);

// UTF-8. next decodes one codepoint and advances s, 0 at the end; a malformed
// sequence yields U+FFFD and skips one byte. prev is the byte offset of the
// codepoint before offset, 0 at the start. encode writes at most
// STR_UTF8_MAX_BYTES and returns how many; past U+10FFFF encodes U+FFFD.
u32 utf8_next(const char **s);
u32 utf8_prev(const char *s, u32 offset);
u32 utf8_encode(u32 codepoint, char *out);

} // namespace str

#define STR_UTF8_MAX_BYTES 4
#define STR_REPLACEMENT_CODEPOINT 0xFFFDu
