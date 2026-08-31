#pragma once

#include "common.h"

namespace result {

enum Error {
    OK = 0,
    ERR_NOT_FOUND,
    ERR_IO,
    ERR_PARSE,
    ERR_UNSUPPORTED,
    ERR_INVALID,
    ERR_EXHAUSTED,
};

const char *str(Error e);

void fail(Error e, const char *file, i32 line); // aborts
void warn(Error e, const char *file, i32 line);

} // namespace result

// Generates the result struct plus the two unwrap overloads MUST and OR_ELSE
// resolve to. Declare it in the same namespace as T so ADL finds them.
#define RESULT_TYPE(T, Name)                                                                       \
    struct Name {                                                                                  \
        T val;                                                                                     \
        result::Error err;                                                                         \
    };                                                                                             \
    inline T result_unwrap(Name r, const char *file, i32 line) {                                   \
        if (r.err != result::OK)                                                                   \
            result::fail(r.err, file, line);                                                       \
        return r.val;                                                                              \
    }                                                                                              \
    inline T result_or(Name r, T fallback, const char *file, i32 line) {                           \
        if (r.err != result::OK) {                                                                 \
            result::warn(r.err, file, line);                                                       \
            return fallback;                                                                       \
        }                                                                                          \
        return r.val;                                                                              \
    }

// Overload for functions that return a bare Error rather than a result struct,
// so MUST() reads the same either way.
inline void result_unwrap(result::Error e, const char *file, i32 line) {
    if (e != result::OK)
        result::fail(e, file, line);
}

// expression; aborts if the result carries an error
#define MUST(expr) result_unwrap((expr), __FILE__, __LINE__)

// expression; yields fallback and warns if the result carries an error
#define OR_ELSE(expr, fallback) result_or((expr), (fallback), __FILE__, __LINE__)

// statement; binds the unwrapped value to name, else returns the error to the
// caller. Declares two names, so it needs braces inside an if/loop body.
#define TRY(name, expr)                                                                            \
    auto name##_r_ = (expr);                                                                       \
    if (name##_r_.err != result::OK)                                                               \
        return {.err = name##_r_.err};                                                             \
    auto name = name##_r_.val
