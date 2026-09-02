#include "test.h"

#include "core/logger.h"

#define REPORT_LIMIT 10

static const char *g_suite = "?";
static const char *g_case = "?";
static i32 g_checks = 0;
static i32 g_fails = 0;
static i32 g_total_checks = 0;
static i32 g_total_fails = 0;

static logger::Level g_unmuted_level;
static u32 g_muted_at; // WARN + ERR count when mute() was called

namespace test {

void mute() {
    g_unmuted_level = logger::level();
    g_muted_at = logger::count(logger::WARN) + logger::count(logger::ERR);
    logger::set_level(logger::FATAL);
}

u32 unmute() {
    logger::set_level(g_unmuted_level);
    return logger::count(logger::WARN) + logger::count(logger::ERR) - g_muted_at;
}

void check(b32 cond, const char *file, i32 line, const char *expr) {
    g_checks++;
    if (cond)
        return;

    g_fails++;

    // a case may mute the logger to exercise a warning path; a failed check
    // inside that window must still be heard, so lift the level to report it
    logger::Level muted = logger::level();
    if (muted > logger::ERR)
        logger::set_level(logger::ERR);

    if (g_fails <= REPORT_LIMIT)
        logger::write(logger::ERR, file, line, "%s/%s: %s", g_suite, g_case, expr);
    if (g_fails == REPORT_LIMIT + 1)
        logger::write(logger::ERR, file, line, "%s/%s: further failures suppressed", g_suite,
                      g_case);

    logger::set_level(muted);
}

i32 run(const Suite *suites, u64 count, memory::Arena *scratch) {
    i32 cases = 0;
    i32 failed = 0;

    for (u64 s = 0; s < count; s++) {
        g_suite = suites[s].name;

        for (u64 c = 0; c < suites[s].count; c++) {
            g_case = suites[s].cases[c].name;
            g_checks = 0;
            g_fails = 0;
            cases++;

            // a case that mutes the logger and returns early must not mute the
            // rest of the run
            logger::Level level = logger::level();
            memory::ArenaTemp t = memory::temp_begin(scratch);
            suites[s].cases[c].fn(scratch);
            memory::temp_end(t);
            logger::set_level(level);

            g_total_checks += g_checks;
            g_total_fails += g_fails;

            if (g_fails) {
                failed++;
                LOG_ERR("FAIL  %s/%s (%d of %d checks)", g_suite, g_case, g_fails, g_checks);
            } else {
                LOG_INFO("ok    %s/%s (%d checks)", g_suite, g_case, g_checks);
            }
        }
    }

    if (failed)
        LOG_ERR("tests: %d of %d cases failed, %d of %d checks failed", failed, cases, g_total_fails,
                g_total_checks);
    else
        LOG_INFO("tests: %d cases passed, %d checks", cases, g_total_checks);

    return failed;
}

} // namespace test
