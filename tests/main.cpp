#include "common.h"
#include "logger.h"
#include "memory.h"
#include "suites.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    // reserving is free; the entity suite loads real models into scopes of
    // its own, and every asset case carves a small manager that never returns
    memory::init(MiB(256));
    memory::Arena scratch = memory::create(MiB(16));

    const test::Suite suites[] = {
        asset_suite(),
        config_suite(),
        entity_suite(),
        file_suite(),
        linalg_suite(),
        path_suite(),
        pool_suite(),
        result_suite(),
        str_suite(),
        strmap_suite(),
    };

    i32 failed = test::run(suites, ARRAY_COUNT(suites), &scratch);

#if CL_MEM_STATS
    memory::log_stats(&scratch, "scratch");
#endif

    logger::shutdown();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
