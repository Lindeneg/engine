#include "file.h"

#include <stdarg.h>
#include <string.h>

#include "logger.h"

#define PATH_SEP "/"

static FILE *os_fopen(const char *path, const char *mode) {
#if PLATFORM_WINDOWS
    FILE *f = NULL;
    fopen_s(&f, path, mode);
    return f;
#elif PLATFORM_LINUX
    return fopen(path, mode);
#endif
}

static i32 os_fseek(FILE *f, i64 off, i32 origin) {
#if PLATFORM_WINDOWS
    return _fseeki64(f, off, origin);
#elif PLATFORM_LINUX
    return fseeko(f, (off_t)off, origin);
#endif
}

static i64 os_ftell(FILE *f) {
#if PLATFORM_WINDOWS
    return _ftelli64(f);
#elif PLATFORM_LINUX
    return (i64)ftello(f);
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

Blob read(memory::Arena *a, const char *path) {
    Blob result = {0};

    FILE *f = os_fopen(path, "rb");
    if (!f) {
        LOG_ERR("file_read: cannot open %s", path);
        return result;
    }

    if (os_fseek(f, 0, SEEK_END) != 0) {
        LOG_ERR("file_read: cannot seek %s", path);
        fclose(f);
        return result;
    }

    i64 len = os_ftell(f);
    if (len < 0) {
        LOG_ERR("file_read: cannot size %s", path);
        fclose(f);
        return result;
    }
    os_fseek(f, 0, SEEK_SET);

    memory::ArenaTemp t = memory::temp_begin(a);

    u8 *buf = push_array_nz(a, u8, (u64)len + 1);
    u64 got = (u64)fread(buf, 1, (u64)len, f);
    fclose(f);

    if (got != (u64)len) {
        LOG_ERR("file_read: short read on %s (%llu of %lld)", path, (unsigned long long)got,
                (long long)len);
        memory::temp_end(t);
        return result;
    }

    buf[len] = 0;
    result.data = buf;
    result.size = (u64)len;
    return result;
}
} // namespace file
