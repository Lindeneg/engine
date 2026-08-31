#include <cstdlib>

#include "common.h"
#include "logger.h"
#include "memory.h"

int main() {

    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(1024));

    auto arena1{memory::create(MiB(512))};
    auto arena2{memory::create(MiB(512))};

    auto ptr{memory::push(&arena1, MiB(256))};
    auto ptr2{memory::push(&arena1, MiB(256))};

    auto ptr3{memory::push(&arena2, MiB(256))};

#if CL_MEM_STATS
    memory::log_sys_stats();
    memory::log_stats(&arena1, "arena1");
    memory::log_stats(&arena2, "arena2");
#endif

    memory::reset(&arena1);
    memory::reset(&arena2);

#if CL_MEM_STATS
    memory::log_sys_stats();
    memory::log_stats(&arena1, "arena1");
    memory::log_stats(&arena2, "arena2");
#endif

    logger::shutdown();
    return EXIT_SUCCESS;
}
