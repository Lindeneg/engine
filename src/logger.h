#pragma once

#include "common.h"

namespace logger {
enum Level { TRACE, DEBUG, INFO, WARN, ERR, FATAL };

// file_path may be NULL for console-only output
void init(const char *file_path);
void shutdown();

void set_level(Level min);
Level level();

// Messages seen at this level since startup, counted before level filtering.
// Tests read it to assert that a path warned.
u32 count(Level level);

void write(Level level, const char *file, i32 line, const char *fmt, ...);

void lassert(b32 fatal, const char *file, i32 line, const char *fmt, ...);

} // namespace logger

#define LOG_TRACE(...) logger::write(logger::TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) logger::write(logger::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) logger::write(logger::INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) logger::write(logger::WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERR(...) logger::write(logger::ERR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) logger::write(logger::FATAL, __FILE__, __LINE__, __VA_ARGS__)

// Once per call site for the life of the process. For anything on a per-frame
// or per-object path, where the first occurrence is the only useful one.
#define LOG_WARN_ONCE(...)                                                                         \
    do {                                                                                           \
        static b8 warned_once_;                                                                    \
        if (!warned_once_) {                                                                       \
            warned_once_ = true;                                                                   \
            LOG_WARN(__VA_ARGS__);                                                                 \
        }                                                                                          \
    } while (0)
