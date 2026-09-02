#pragma once

#include <stdio.h>
#include <time.h>

#include "common.h"

// The one module that talks to the OS; nothing else sees <windows.h> or the macros it leaks.
namespace platform {

// zero-initialized is a valid unlocked mutex on every supported platform
struct Mutex {
    u64 storage[8];
};

void lock(Mutex *m);
void unlock(Mutex *m);

u64 page_size();
u64 alloc_granularity();
void *reserve(u64 bytes);         // address space only; NULL on failure
b32 commit(void *ptr, u64 bytes); // backs reserved pages with memory

FILE *file_open(const char *path, const char *mode);
i32 file_seek(FILE *f, i64 offset, i32 origin); // 64-bit offsets on both platforms
i64 file_tell(FILE *f);

void local_time(struct tm *out, const time_t *t);
b32 enable_console_color();          // true when stdout can take ANSI colour
void debug_print(const char *text); // the debugger's output pane, where one exists

} // namespace platform
