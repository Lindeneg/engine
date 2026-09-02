#include "platform/file.h"

#include "core/logger.h"
#include "platform/platform.h"

namespace file {

BlobResult read(memory::Arena *a, const char *path) {
    FILE *f = platform::file_open(path, "rb");
    if (!f) {
        LOG_ERR("file_read: cannot open %s", path);
        return BlobResult{.err = result::ERR_NOT_FOUND};
    }

    if (platform::file_seek(f, 0, SEEK_END) != 0) {
        LOG_ERR("file_read: cannot seek %s", path);
        fclose(f);
        return BlobResult{.err = result::ERR_IO};
    }

    i64 len = platform::file_tell(f);
    if (len < 0) {
        LOG_ERR("file_read: cannot size %s", path);
        fclose(f);
        return BlobResult{.err = result::ERR_IO};
    }
    platform::file_seek(f, 0, SEEK_SET);

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

result::Error write(const char *path, const void *data, u64 size) {
    ASSERT(path && (data || size == 0));

    FILE *f = platform::file_open(path, "wb");
    if (!f) {
        LOG_ERR("file_write: cannot open %s", path);
        return result::ERR_IO;
    }

    u64 put = fwrite(data, 1, size, f);
    b32 closed = fclose(f) == 0;
    if (put != size || !closed) {
        LOG_ERR("file_write: short write on %s (%llu of %llu)", path, (unsigned long long)put,
                (unsigned long long)size);
        return result::ERR_IO;
    }

    return result::OK;
}

b32 exists(const char *path) {
    ASSERT(path);

    FILE *f = platform::file_open(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}
} // namespace file
