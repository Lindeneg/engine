#include "path.h"

#include <stdarg.h>
#include <string.h>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif PLATFORM_LINUX
#include <unistd.h>
#endif

#define PATH_SEP "/"

namespace path {

char *join(memory::Arena *a, ...) {
    u64 sep_len = sizeof(PATH_SEP) - 1;

    va_list args;
    va_start(args, a);
    u64 total = 0;
    u64 count = 0;
    for (const char *seg = va_arg(args, const char *); seg; seg = va_arg(args, const char *)) {
        total += strlen(seg);
        count++;
    }
    va_end(args);

    if (count > 1)
        total += (count - 1) * sep_len;

    char *buf = push_array(a, char, total + 1);

    va_start(args, a);
    u64 off = 0;
    u64 i = 0;
    for (const char *seg = va_arg(args, const char *); seg; seg = va_arg(args, const char *)) {
        if (i++ > 0) {
            memcpy(buf + off, PATH_SEP, sep_len);
            off += sep_len;
        }
        u64 len = strlen(seg);
        memcpy(buf + off, seg, len);
        off += len;
    }
    va_end(args);

    buf[off] = 0;
    return buf;
}

char *swap_ext(memory::Arena *a, const char *path, const char *ext) {
    ASSERT(a && path && ext);

    u64 len = strlen(path);
    u64 stem = len;

    // scan back only to the last separator: a dot in a directory name is not
    // this file's extension
    for (u64 i = len; i > 0; i--) {
        char c = path[i - 1];
        if (c == '/' || c == '\\')
            break;
        if (c == '.') {
            stem = i - 1;
            break;
        }
    }

    u64 ext_len = strlen(ext);
    char *out = push_array_nz(a, char, stem + ext_len + 1);
    memcpy(out, path, stem);
    memcpy(out + stem, ext, ext_len + 1);
    return out;
}

b32 exe_dir(char *dst, u64 cap) {
    ASSERT(dst && cap);

#if PLATFORM_WINDOWS
    DWORD n = GetModuleFileNameA(NULL, dst, (DWORD)cap);
    if (n == 0 || n >= cap)
        return false;
#elif PLATFORM_LINUX
    i64 n = readlink("/proc/self/exe", dst, cap - 1);
    if (n <= 0 || (u64)n >= cap - 1)
        return false;
    dst[n] = 0;
#endif

    for (i64 i = (i64)strlen(dst) - 1; i >= 0; i--) {
        if (dst[i] == '/' || dst[i] == '\\') {
            dst[i] = 0;
            return true;
        }
    }
    return false;
}

} // namespace path
