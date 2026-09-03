#include "editor/dashboard.h"

#include "gfx/canvas.h"
#include "platform/dialog.h"
#include "project/project.h"
#include "ui/ui.h"

#define PANEL_TITLE "Projects"
#define STATUS_NOT_A_PROJECT "That folder has no project.cfg"
#define STATUS_NEEDS_NAME "A new project needs a name"
#define STATUS_NEEDS_LOCATION "Choose where the new project's folder goes"
#define STATUS_CREATE_FAILED "The project could not be created; the log says why"
#define LOCATION_UNSET "Choose location..."

// label and field share a row; a lone label under the recent list gets the whole
static const f32 FIELD_ROW[2] = {0.3f, 0.7f};

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

    if (cfg.recent_cap) {
        d->recent = push_array(a, const char *, cfg.recent_cap);
        d->recent_count = project::recent(a, engine_root, d->recent, cfg.recent_cap);
    }
    return d;
}

static Result take(const char *dir, char *out_dir, u32 cap) {
    snprintf(out_dir, cap, "%s", dir);
    return RESULT_OPEN;
}

Result frame(Dashboard *d, memory::Arena *a, memory::Arena *scratch, i32 fb_width, i32 fb_height,
             char *out_dir, u32 cap) {
    ASSERT(d && a && scratch && out_dir && cap);

    canvas::Rect panel{((f32)fb_width - d->cfg.width) * 0.5f,
                       ((f32)fb_height - d->cfg.height) * 0.5f, d->cfg.width, d->cfg.height};
    ui::begin_panel(PANEL_TITLE, panel);
    Result result = RESULT_NONE;

    ui::label("Recent");
    if (d->recent_count == 0)
        ui::label("(none yet)");
    for (u32 i = 0; i < d->recent_count && result == RESULT_NONE; i++) {
        ui::push_id(i);
        if (ui::button(d->recent[i])) {
            if (project::is_project(d->recent[i]))
                result = take(d->recent[i], out_dir, cap);
            else
                snprintf(d->status, sizeof(d->status), "%s: %s", STATUS_NOT_A_PROJECT,
                         d->recent[i]);
        }
        ui::pop_id();
    }

    ui::separator();
    if (ui::button("Open folder...")) {
        char dir[PROJECT_PATH_CAP];
        if (dialog::choose_folder("Open a project", dir, sizeof(dir))) {
            if (project::is_project(dir))
                result = take(dir, out_dir, cap);
            else
                snprintf(d->status, sizeof(d->status), "%s", STATUS_NOT_A_PROJECT);
        }
    }

    ui::separator();
    ui::label("New project");
    ui::columns(2, FIELD_ROW);
    ui::label("name");
    ui::text_field("name", d->name, sizeof(d->name));
    ui::label("location");
    if (ui::button(d->location[0] ? d->location : LOCATION_UNSET))
        dialog::choose_folder("Where the project's folder goes", d->location,
                              sizeof(d->location));
    ui::columns(1);

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

    ui::separator();
    if (d->status[0])
        ui::label(d->status);
    if (ui::button("Quit"))
        result = RESULT_QUIT;

    ui::end_panel();
    return result;
}

} // namespace dashboard
