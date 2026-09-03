#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"

// A project is a folder the engine is pointed at: project.cfg at its root,
// levels/, entities/ with templates/, models/, animations/, and its own
// input.cfg. Everything a game is lives there; the engine's own resources live
// in the engine folder and are the same for every project. The engine knows
// the folder it was given and nothing else about content.
//
// A new project is a copy of the engine's project_template with its name
// written in. The dashboard lists recent projects from a file in the engine
// folder, most recent first.

#define PROJECT_CFG "project.cfg"
#define PROJECT_TEMPLATE_DIR "project_template"
#define PROJECT_RECENT_FILE "recent.cfg"
#define PROJECT_INPUT_CFG "input.cfg"

// the folders a project keeps; where the editor's imports land, by kind
#define PROJECT_LEVEL_DIR "levels"
#define PROJECT_ENTITY_DIR "entities"
#define PROJECT_MODEL_DIR "models"
#define PROJECT_ANIMATION_DIR "animations"

#define PROJECT_NAME_CAP 64
#define PROJECT_PATH_CAP 512

namespace project {

struct Project {
    char dir[PROJECT_PATH_CAP];
    char name[PROJECT_NAME_CAP];
    char title[PROJECT_NAME_CAP]; // the window's
    char level[PROJECT_NAME_CAP]; // opened first, under levels/

    // the whole file, kept, for the blocks the engine reads by name: world, player
    config::Doc *doc;
};

// True when dir holds a project.cfg.
b32 is_project(const char *dir);

// Reads project.cfg into a; the doc lives there. A folder without one, or one
// that will not parse, is an error: opening a project is infrastructure.
result::Error open(memory::Arena *a, const char *dir, Project *out);

// Writes name, title and level back into the doc and saves it. Blocks the
// engine only reads are kept as they were.
result::Error save(memory::Arena *scratch, Project *p);

// parent/name from the engine's template, named, then opened into a.
result::Error create(memory::Arena *a, memory::Arena *scratch, const char *engine_root,
                     const char *parent, const char *name, Project *out);

// The recent list: read into a; a project moved to its front and the list
// trimmed to cap; or a folder dropped, once it is no project any more.
u32 recent(memory::Arena *a, const char *engine_root, const char **out, u32 cap);
void remember(memory::Arena *scratch, const char *engine_root, const char *dir, u32 cap);
void forget(memory::Arena *scratch, const char *engine_root, const char *dir, u32 cap);

} // namespace project
