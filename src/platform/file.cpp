#include "platform/file.h"

#include <string.h>

#include "core/logger.h"
#include "platform/platform.h"

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

b32 remove(const char *path) {
    ASSERT(path);

    if (!exists(path))
        return false;
    if (::remove(path) != 0) {
        LOG_ERR("file_remove: cannot delete %s", path);
        return false;
    }
    return true;
}

// one level; the parents are the caller's, see make_directory
static b32 make_one_directory(const char *path) {
#if PLATFORM_WINDOWS
    if (CreateDirectoryA(path, NULL))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path, 0755) == 0)
        return true;
    return errno == EEXIST;
#endif
}

b32 make_directory(const char *path) {
    ASSERT(path);

    // each prefix ending at a separator, then the whole
    char buf[FILE_PATH_CAP];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (i32)sizeof(buf)) {
        LOG_ERR("file_mkdir: %s is longer than %d bytes", path, FILE_PATH_CAP);
        return false;
    }
    for (char *c = buf + 1; *c; c++) {
        if (*c != '/' && *c != '\\')
            continue;
        // a drive root "D:/" is not ours to make
        if (c[-1] == ':')
            continue;
        *c = 0;
        b32 ok = make_one_directory(buf);
        *c = '/';
        if (!ok) {
            LOG_ERR("file_mkdir: cannot create %s", buf);
            return false;
        }
    }
    if (!make_one_directory(buf)) {
        LOG_ERR("file_mkdir: cannot create %s", buf);
        return false;
    }
    return true;
}

result::Error copy(const char *src, const char *dst) {
    ASSERT(src && dst);
#if PLATFORM_WINDOWS
    if (!CopyFileA(src, dst, FALSE)) {
        LOG_ERR("file_copy: cannot copy %s to %s", src, dst);
        return result::ERR_IO;
    }
    return result::OK;
#else
    FILE *in = platform::file_open(src, "rb");
    if (!in) {
        LOG_ERR("file_copy: cannot open %s", src);
        return result::ERR_IO;
    }
    FILE *out = platform::file_open(dst, "wb");
    if (!out) {
        fclose(in);
        LOG_ERR("file_copy: cannot open %s", dst);
        return result::ERR_IO;
    }

    char buf[FILE_COPY_CHUNK];
    u64 n;
    b32 ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = ok && fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    if (!ok) {
        LOG_ERR("file_copy: short copy of %s to %s", src, dst);
        return result::ERR_IO;
    }
    return result::OK;
#endif
}

u32 list(const char *dir, Entry *out, u32 cap) {
    ASSERT(dir && (out || cap == 0));

    u32 n = 0;
#if PLATFORM_WINDOWS
    char pattern[FILE_PATH_CAP];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);

    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
            continue;
        if (n < cap) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", found.cFileName);
            out[n].is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        n++;
    } while (FindNextFileA(h, &found));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n < cap) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
            char full[FILE_PATH_CAP];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;
            out[n].is_directory = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        n++;
    }
    closedir(d);
#endif

    // sorted by name: a menu built from this must not shuffle between runs
    u32 have = MIN(n, cap);
    for (u32 i = 1; i < have; i++) {
        Entry e = out[i];
        u32 j = i;
        while (j > 0 && strcmp(out[j - 1].name, e.name) > 0) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = e;
    }
    return n;
}

result::Error copy_directory(const char *src, const char *dst) {
    ASSERT(src && dst);

    if (!make_directory(dst))
        return result::ERR_IO;

    Entry entries[FILE_LIST_CAP];
    u32 n = list(src, entries, FILE_LIST_CAP);
    if (n > FILE_LIST_CAP) {
        LOG_ERR("file_copy: %s holds more than %d entries; not all were copied", src,
                FILE_LIST_CAP);
        n = FILE_LIST_CAP;
    }

    for (u32 i = 0; i < n; i++) {
        char from[FILE_PATH_CAP];
        char to[FILE_PATH_CAP];
        snprintf(from, sizeof(from), "%s/%s", src, entries[i].name);
        snprintf(to, sizeof(to), "%s/%s", dst, entries[i].name);
        result::Error e =
            entries[i].is_directory ? copy_directory(from, to) : copy(from, to);
        if (e != result::OK)
            return e;
    }
    return result::OK;
}

b32 remove_directory(const char *path) {
    ASSERT(path);

    Entry entries[FILE_LIST_CAP];
    u32 n = MIN(list(path, entries, FILE_LIST_CAP), (u32)FILE_LIST_CAP);
    for (u32 i = 0; i < n; i++) {
        char full[FILE_PATH_CAP];
        snprintf(full, sizeof(full), "%s/%s", path, entries[i].name);
        if (entries[i].is_directory ? !remove_directory(full) : !remove(full))
            return false;
    }

#if PLATFORM_WINDOWS
    if (!RemoveDirectoryA(path)) {
#else
    if (rmdir(path) != 0) {
#endif
        LOG_ERR("file_rmdir: cannot remove %s", path);
        return false;
    }
    return true;
}
} // namespace file
