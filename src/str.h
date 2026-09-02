#pragma once

#include "common.h"

namespace str {

// Index of s in names, or -1. For the enum <-> name tables config files use.
i32 name_index(const char *const *names, u32 count, const char *s);

// Every non-alphanumeric byte becomes '_', so "mixamorig:Hips" (FBX2glTF) and
// "mixamorig_Hips" (THREE) compare equal. Run on every node or joint name
// before comparing or hashing it. Truncates to cap - 1 and NUL-terminates;
// NULL reads as "".
void canonical_name(char *dst, u64 cap, const char *src);

} // namespace str
