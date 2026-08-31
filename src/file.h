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
}; // namespace file
