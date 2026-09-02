#include "core/result.h"

#include "core/logger.h"

namespace result {

const char *str(Error e) {
    switch (e) {
    case OK:
        return "ok";
    case ERR_NOT_FOUND:
        return "not found";
    case ERR_IO:
        return "io";
    case ERR_PARSE:
        return "parse";
    case ERR_UNSUPPORTED:
        return "unsupported";
    case ERR_INVALID:
        return "invalid";
    case ERR_EXHAUSTED:
        return "exhausted";
    }
    return "?";
}

void fail(Error e, const char *file, i32 line) {
    logger::lassert(true, file, line, "MUST failed: %s", str(e));
}

void warn(Error e, const char *file, i32 line) {
    logger::write(logger::WARN, file, line, "fallback: %s", str(e));
}

} // namespace result
