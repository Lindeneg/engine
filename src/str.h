#pragma once

#include "common.h"

namespace str {

// Index of s in names, or -1. For the enum <-> name tables config files use.
i32 name_index(const char *const *names, u32 count, const char *s);

// Non-alphanumerics become '_', so "mixamorig:Hips" and "mixamorig_Hips"
// compare equal. Truncates to cap - 1; NULL reads as "".
void canonical_name(char *dst, u64 cap, const char *src);

} // namespace str
