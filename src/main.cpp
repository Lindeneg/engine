#include "common.h"
#include "logger.h"
#include "memory.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(1024));
    memory::Arena perm = memory::create(MiB(256));
    memory::Arena scratch = memory::create(MiB(64));

    LOG_INFO("engine: boot");

#if CL_MEM_STATS
    memory::log_stats(&perm, "perm");
    memory::log_stats(&scratch, "scratch");
#endif

    logger::shutdown();
    return EXIT_SUCCESS;
}
