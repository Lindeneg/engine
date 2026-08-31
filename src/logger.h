#pragma once

#include "common.h"

namespace logger {
enum Level { TRACE, DEBUG, INFO, WARN, ERR, FATAL };

// file_path may be NULL for console-only output
void init(const char *file_path);
void shutdown();

void set_level(Level min);

void write(Level level, const char *file, i32 line, const char *fmt, ...);

void lassert(b32 fatal, const char *file, i32 line, const char *fmt, ...);

} // namespace logger

#define LOG_TRACE(...) logger::write(logger::TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) logger::write(logger::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) logger::write(logger::INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) logger::write(logger::WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERR(...) logger::write(logger::ERR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) logger::write(logger::FATAL, __FILE__, __LINE__, __VA_ARGS__)
