#include "logger.h"

#include <stdarg.h>
#include <time.h>

#include "platform.h"
#include "str.h"

#define TIMESTAMP_CAP 16
#define MESSAGE_CAP 1024
#define LINE_CAP (MESSAGE_CAP + 128) // message plus level, file and line

static platform::Mutex g_lock;
static FILE *g_file = NULL;
static logger::Level g_min = logger::TRACE;
static b32 g_color = false; // true when stdout is an ANSI-capable console
static u32 g_counts[logger::FATAL + 1];

// index == Level; the lower-case form is what engine.cfg uses
static const char *LEVEL_NAMES[logger::FATAL + 1] = {"trace", "debug", "info",
                                                     "warn",  "error", "fatal"};
static const char *LEVEL_TAGS[logger::FATAL + 1] = {"TRACE", "DEBUG", "INFO",
                                                    "WARN",  "ERROR", "FATAL"};

static const char *level_name(logger::Level level) {
    return level <= logger::FATAL ? LEVEL_TAGS[level] : "?????";
}

static const char *level_color(logger::Level level) {
    switch (level) {
    case logger::TRACE:
        return "\x1b[90m"; // bright black
    case logger::DEBUG:
        return "\x1b[36m"; // cyan
    case logger::INFO:
        return "\x1b[32m"; // green
    case logger::WARN:
        return "\x1b[33m"; // yellow
    case logger::ERR:
        return "\x1b[31m"; // red
    case logger::FATAL:
        return "\x1b[35m"; // magenta
    }
    return "";
}

namespace logger {

void init(const char *file_path) {
    platform::lock(&g_lock);
    if (file_path && !g_file)
        g_file = platform::file_open(file_path, "w");
    g_color = platform::enable_console_color();
    b32 no_file = file_path && !g_file;
    platform::unlock(&g_lock);

    // g_lock is not recursive, so this has to happen after the unlock
    if (no_file)
        LOG_WARN("logger: cannot open %s, console only", file_path);
}

void shutdown(void) {
    platform::lock(&g_lock);
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    platform::unlock(&g_lock);
}

void set_level(Level min) { g_min = min; }
Level level() { return g_min; }

b32 level_from_name(const char *name, Level *out) {
    ASSERT(out);
    i32 i = str::name_index(LEVEL_NAMES, ARRAY_COUNT(LEVEL_NAMES), name);
    if (i < 0)
        return false;
    *out = (Level)i;
    return true;
}
u32 count(Level level) { return level <= FATAL ? g_counts[level] : 0; }

void write(Level level, const char *file, i32 line, const char *fmt, ...) {
    if (level <= FATAL)
        g_counts[level]++;
    if (level < g_min)
        return;

    const char *name = file;
    for (const char *p = file; *p; ++p) {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }

    char ts[TIMESTAMP_CAP];
    time_t now = time(NULL);
    struct tm lt;
    platform::local_time(&lt, &now);
    strftime(ts, sizeof(ts), "%H:%M:%S", &lt);

    char msg[MESSAGE_CAP];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const char *lvl = level_name(level);
    const char *color = g_color ? level_color(level) : "";
    const char *reset = g_color ? "\x1b[0m" : "";
    FILE *console = (level >= ERR) ? stderr : stdout;

    platform::lock(&g_lock);

    fprintf(console, "%s %s%-5s%s %s:%d: %s\n", ts, color, lvl, reset, name, line, msg);

    if (g_file) {
        fprintf(g_file, "%s %-5s %s:%d: %s\n", ts, lvl, name, line, msg);
        fflush(g_file);
    }

    char dbg[LINE_CAP];
    snprintf(dbg, sizeof(dbg), "[%s] %s:%d: %s\n", lvl, name, line, msg);
    platform::debug_print(dbg);

    platform::unlock(&g_lock);

    if (level == FATAL) {
        fflush(NULL);
        abort();
    }
}

void lassert(b32 fatal, const char *file, i32 line, const char *fmt, ...) {
    char msg[MESSAGE_CAP];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    write(fatal ? FATAL : WARN, file, line, "%s", msg);
}
} // namespace logger
