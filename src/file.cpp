#include "file.h"

#include "logger.h"

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

namespace file {

BlobResult read(memory::Arena *a, const char *path) {
    FILE *f = os_fopen(path, "rb");
    if (!f) {
        LOG_ERR("file_read: cannot open %s", path);
        return BlobResult{.err = result::ERR_NOT_FOUND};
    }

    if (os_fseek(f, 0, SEEK_END) != 0) {
        LOG_ERR("file_read: cannot seek %s", path);
        fclose(f);
        return BlobResult{.err = result::ERR_IO};
    }

    i64 len = os_ftell(f);
    if (len < 0) {
        LOG_ERR("file_read: cannot size %s", path);
        fclose(f);
        return BlobResult{.err = result::ERR_IO};
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
        return BlobResult{.err = result::ERR_IO};
    }

    buf[len] = 0;
    return BlobResult{.val = Blob{.data = buf, .size = (u64)len}};
}
} // namespace file
