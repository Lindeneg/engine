#include "common.h"
#include "gltf.h"
#include "logger.h"
#include "memory.h"
#include "path.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(1024));
    memory::Arena scratch = memory::create(MiB(256));

    const char *assets[] = {
        "characters/soldier.glb",
        "weapons/pistol.glb",
        "animations/pistol_idle.glb",
    };

    for (u64 i = 0; i < ARRAY_COUNT(assets); i++) {
        const char *full = FPATH(&scratch, "assets", assets[i]);
        if (gltf::inspect(&scratch, full) != result::OK)
            LOG_ERR("inspect failed for %s", full);
    }

#if CL_MEM_STATS
    memory::log_stats(&scratch, "scratch");
#endif

    logger::shutdown();
    return EXIT_SUCCESS;
}
