#include "path.h"

#include <stdarg.h>
#include <string.h>

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

} // namespace path
