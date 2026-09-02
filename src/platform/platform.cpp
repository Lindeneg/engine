#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "platform/platform.h"

#if PLATFORM_WINDOWS
#include <windows.h>
#elif PLATFORM_LINUX
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace platform {

#if PLATFORM_WINDOWS

static_assert(sizeof(SRWLOCK) <= sizeof(Mutex::storage), "Mutex storage too small");

void lock(Mutex *m) { AcquireSRWLockExclusive((SRWLOCK *)m->storage); }
void unlock(Mutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)m->storage); }

u64 page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

u64 alloc_granularity() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity;
}

void *reserve(u64 bytes) { return VirtualAlloc(0, bytes, MEM_RESERVE, PAGE_READWRITE); }

b32 commit(void *ptr, u64 bytes) {
    return VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

FILE *file_open(const char *path, const char *mode) {
    FILE *f = NULL;
    fopen_s(&f, path, mode);
    return f;
}

i32 file_seek(FILE *f, i64 offset, i32 origin) { return _fseeki64(f, offset, origin); }
i64 file_tell(FILE *f) { return _ftelli64(f); }

void local_time(struct tm *out, const time_t *t) { localtime_s(out, t); }

b32 enable_console_color() {
    DWORD mode = 0;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(err, &mode))
        SetConsoleMode(err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(out, &mode))
        return false; // redirected to a file or pipe
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return true;
}

void debug_print(const char *text) { OutputDebugStringA(text); }

#elif PLATFORM_LINUX

// glibc's PTHREAD_MUTEX_INITIALIZER is all zeros, which is what makes a zeroed
// Mutex valid without an init call
static_assert(sizeof(pthread_mutex_t) <= sizeof(Mutex::storage), "Mutex storage too small");

void lock(Mutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->storage); }
void unlock(Mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->storage); }

u64 page_size() { return (u64)sysconf(_SC_PAGESIZE); }
u64 alloc_granularity() { return (u64)sysconf(_SC_PAGESIZE); }

void *reserve(u64 bytes) {
    void *p = mmap(0, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

b32 commit(void *ptr, u64 bytes) { return mprotect(ptr, bytes, PROT_READ | PROT_WRITE) == 0; }

FILE *file_open(const char *path, const char *mode) { return fopen(path, mode); }
i32 file_seek(FILE *f, i64 offset, i32 origin) { return fseeko(f, (off_t)offset, origin); }
i64 file_tell(FILE *f) { return (i64)ftello(f); }

void local_time(struct tm *out, const time_t *t) { localtime_r(t, out); }
b32 enable_console_color() { return isatty(STDOUT_FILENO); }
void debug_print(const char *) {}

#endif

} // namespace platform
