#include "core/common.h"
#include "core/logger.h"
#include "core/memory.h"
#include "suites.h"

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    // reserving is free, and the asset and entity suites carve managers that never return
    memory::init(MiB(256));
    memory::Arena scratch = memory::create(MiB(16));

    const test::Suite suites[] = {
        anim_suite(),      asset_suite(),  canvas_suite(), config_suite(), debugdraw_suite(),
        drawkey_suite(),   editor_suite(), entity_suite(), file_suite(),   font_suite(),
        frametime_suite(), gfx_suite(),    gizmo_suite(),  input_suite(),  intersect_suite(),
        linalg_suite(),
        memory_suite(),    motion_suite(), path_suite(),   pool_suite(),   project_suite(),
        result_suite(),    shader_suite(),
        str_suite(),       strmap_suite(), textedit_suite(), ui_suite(),      undo_suite(),
    };

    i32 failed = test::run(suites, ARRAY_COUNT(suites), &scratch);

#if CL_MEM_STATS
    memory::log_stats(&scratch, "scratch");
#endif

    logger::shutdown();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
