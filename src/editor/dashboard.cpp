#include "editor/dashboard.h"

#include "gfx/canvas.h"
#include "platform/dialog.h"
#include "platform/path.h"
#include "project/project.h"
#include "ui/ui.h"

#define PANEL_TITLE "Projects"
#define STATUS_NOT_A_PROJECT "That folder has no project.cfg"
#define STATUS_FORGOTTEN "That folder has no project.cfg any more, so it left the list"
#define STATUS_NEEDS_NAME "A new project needs a name"
#define STATUS_NEEDS_LOCATION "Choose where the new project's folder goes"
#define STATUS_CREATE_FAILED "The project could not be created; the log says why"
#define LOCATION_UNSET "Choose location..."
#define NONE_YET "(none yet)"

// no recent entry is being dropped this frame
#define KEEP_ALL (-1)

// the left part of a row and the rest: a label beside its field, a project's
// name beside its path, a lone button
static const f32 SPLIT_ROW[2] = {0.3f, 0.7f};

// the same left edge with the rest halved: a button under the fields, or at the far right
static const f32 THIRDS_ROW[3] = {0.3f, 0.35f, 0.35f};

namespace dashboard {

struct Dashboard {
    Config cfg;
    char engine_root[PROJECT_PATH_CAP];

    const char **recent;
    u32 recent_count;

    char name[PROJECT_NAME_CAP];
    char location[PROJECT_PATH_CAP];
    char status[DASHBOARD_STATUS_CAP];
};

Dashboard *init(memory::Arena *a, const char *engine_root, Config cfg) {
    ASSERT(a && engine_root);

    Dashboard *d = push_struct(a, Dashboard);
    d->cfg = cfg;
    snprintf(d->engine_root, sizeof(d->engine_root), "%s", engine_root);

    if (cfg.recent_cap)
        d->recent = push_array(a, const char *, cfg.recent_cap);
    refresh(d, a);
    return d;
}

void refresh(Dashboard *d, memory::Arena *a) {
    ASSERT(d && a);
    d->recent_count = 0;
    if (d->cfg.recent_cap)
        d->recent_count = project::recent(a, d->engine_root, d->recent, d->cfg.recent_cap);
}

static Result take(const char *dir, char *out_dir, u32 cap) {
    snprintf(out_dir, cap, "%s", dir);
    return RESULT_OPEN;
}

// the folder is no project any more: off the file, and off the screen
static void forget_recent(Dashboard *d, memory::Arena *scratch, u32 i) {
    const char *dir = d->recent[i];
    snprintf(d->status, sizeof(d->status), "%s: %s", STATUS_FORGOTTEN, dir);
    project::forget(scratch, d->engine_root, dir, d->cfg.recent_cap);

    for (u32 j = i; j + 1 < d->recent_count; j++)
        d->recent[j] = d->recent[j + 1];
    d->recent_count--;
}

static Result recent_section(Dashboard *d, memory::Arena *scratch, char *out_dir, u32 cap) {
    Result result = RESULT_NONE;

    ui::label("Recent");
    if (d->recent_count == 0)
        ui::label_dim(NONE_YET);

    // dropped after the rows, so the ones below it still draw this frame
    i32 forget = KEEP_ALL;
    for (u32 i = 0; i < d->recent_count; i++) {
        const char *dir = d->recent[i];
        const char *name = path::basename(dir);

        ui::push_id(i);
        ui::columns(2, SPLIT_ROW);
        if (ui::button(name[0] ? name : dir)) {
            if (project::is_project(dir))
                result = take(dir, out_dir, cap);
            else
                forget = (i32)i;
        }
        ui::label_dim(dir);
        ui::pop_id();
    }
    ui::columns(1);

    if (forget != KEEP_ALL)
        forget_recent(d, scratch, (u32)forget);

    ui::spacer();
    ui::columns(2, SPLIT_ROW);
    if (ui::button("Open folder...")) {
        char dir[PROJECT_PATH_CAP];
        if (dialog::choose_folder("Open a project", dir, sizeof(dir))) {
            if (project::is_project(dir))
                result = take(dir, out_dir, cap);
            else
                snprintf(d->status, sizeof(d->status), "%s", STATUS_NOT_A_PROJECT);
        }
    }
    ui::columns(1);
    return result;
}

static Result create_section(Dashboard *d, memory::Arena *a, memory::Arena *scratch, char *out_dir,
                             u32 cap) {
    Result result = RESULT_NONE;

    ui::label("New project");
    ui::columns(2, SPLIT_ROW);
    ui::label("name");
    ui::text_field("name", d->name, sizeof(d->name));
    ui::label("location");
    if (ui::button(d->location[0] ? d->location : LOCATION_UNSET))
        dialog::choose_folder("Where the project's folder goes", d->location,
                              sizeof(d->location));

    ui::columns(3, THIRDS_ROW);
    ui::spacer();
    if (ui::button("Create")) {
        if (!d->name[0]) {
            snprintf(d->status, sizeof(d->status), "%s", STATUS_NEEDS_NAME);
        } else if (!d->location[0]) {
            snprintf(d->status, sizeof(d->status), "%s", STATUS_NEEDS_LOCATION);
        } else {
            project::Project made;
            if (project::create(a, scratch, d->engine_root, d->location, d->name, &made) ==
                result::OK)
                result = take(made.dir, out_dir, cap);
            else
                snprintf(d->status, sizeof(d->status), "%s", STATUS_CREATE_FAILED);
        }
    }
    ui::columns(1);
    return result;
}

Result frame(Dashboard *d, memory::Arena *a, memory::Arena *scratch, i32 fb_width, i32 fb_height,
             char *out_dir, u32 cap) {
    ASSERT(d && a && scratch && out_dir && cap);

    canvas::Rect panel{((f32)fb_width - d->cfg.width) * 0.5f,
                       ((f32)fb_height - d->cfg.height) * 0.5f, d->cfg.width, d->cfg.height};
    ui::begin_panel(PANEL_TITLE, panel, d->cfg.padding);

    // one click a frame, so at most one section answers
    Result result = recent_section(d, scratch, out_dir, cap);

    ui::spacer();
    ui::separator();
    Result created = create_section(d, a, scratch, out_dir, cap);
    if (created != RESULT_NONE)
        result = created;

    ui::spacer();
    ui::separator();
    if (d->status[0])
        ui::label(d->status);
    else
        ui::spacer(); // the row stays, so Quit does not jump when a status appears

    ui::columns(3, THIRDS_ROW);
    ui::spacer();
    ui::spacer();
    if (ui::button("Quit"))
        result = RESULT_QUIT;
    ui::columns(1);

    ui::end_panel();
    return result;
}

} // namespace dashboard
