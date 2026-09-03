#pragma once

#include "core/common.h"
#include "core/memory.h"
#include "core/result.h"

#define FILE_NAME_CAP 256
#define FILE_PATH_CAP 1024
#define FILE_LIST_CAP 256    // entries one directory operation handles
#define FILE_COPY_CHUNK 65536 // bytes per read while copying, where the OS does not copy for us

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

// Deletes the file; logs on failure. False when it was not there to delete.
b32 remove(const char *path);

// Directories, for projects: made with their parents, copied whole, listed,
// and removed with everything in them. Listing is sorted by name so a menu
// built from it is stable.
b32 make_directory(const char *path);
result::Error copy(const char *src, const char *dst);
result::Error copy_directory(const char *src, const char *dst);
b32 remove_directory(const char *path);

struct Entry {
    char name[FILE_NAME_CAP];
    b32 is_directory;
};

// Entries of dir, without . and ..; how many there were, which may exceed cap.
u32 list(const char *dir, Entry *out, u32 cap);
}; // namespace file
