#pragma once

#include "common.h"
#include "memory.h"
#include "result.h"

namespace file {

struct Blob {
    u8 *data;
    u64 size;
};

RESULT_TYPE(Blob, BlobResult);

BlobResult read(memory::Arena *a, const char *path);

// For optional files, where absence is normal and read()'s error log would be
// noise -- a sidecar config that most assets will not have, say.
b32 exists(const char *path);
}; // namespace file
