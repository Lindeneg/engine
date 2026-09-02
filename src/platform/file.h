#pragma once

#include "core/common.h"
#include "core/memory.h"
#include "core/result.h"

namespace file {

struct Blob {
    u8 *data;
    u64 size;
};

RESULT_TYPE(Blob, BlobResult);

BlobResult read(memory::Arena *a, const char *path);

// Replaces the file wholesale. Logs on failure, like read.
result::Error write(const char *path, const void *data, u64 size);

// For optional files, where read()'s error log would be noise.
b32 exists(const char *path);
}; // namespace file
