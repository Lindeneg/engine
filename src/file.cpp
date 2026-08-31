#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "file.h"

#include <stdarg.h>
#include <string.h>

#include "logger.h"

#if PLATFORM_WINDOWS
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

static FILE *os_fopen(const char *path, const char *mode) {
#if PLATFORM_WINDOWS
    FILE *f = NULL;
    fopen_s(&f, path, mode);
    return f;
#elif PLATFORM_LINUX
    return fopen(path, mode);
#endif
}

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
} // namespace path

namespace file {

Descriptor read(memory::Arena *a, const char *path) {
    Descriptor result = {0};

    FILE *f = os_fopen(path, "rb");
    if (!f) {
        LOG_ERR("file_read: cannot open %s", path);
        return result;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        LOG_ERR("file_read: cannot size %s", path);
        fclose(f);
        return result;
    }

    u8 *buf = push_array(a, u8, (u64)len + 1);
    u64 got = (u64)fread(buf, 1, (u64)len, f);
    fclose(f);

    if (got != (u64)len) {
        LOG_ERR("file_read: short read on %s (%llu of %ld)", path, (unsigned long long)got, len);
        return result;
    }

    buf[len] = 0;
    result.data = buf;
    result.size = (u64)len;
    return result;
}
} // namespace file
