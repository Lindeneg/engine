#include "common.h"
#include "logger.h"
#include "memory.h"
#include "suites.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(64));
    memory::Arena scratch = memory::create(MiB(16));

    const test::Suite suites[] = {
        asset_suite(),
        file_suite(),
        gltf_suite(),
        path_suite(),
        result_suite(),
        strmap_suite(),
    };

    i32 failed = test::run(suites, ARRAY_COUNT(suites), &scratch);

#if CL_MEM_STATS
    memory::log_stats(&scratch, "scratch");
#endif

    logger::shutdown();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
