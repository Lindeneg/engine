#include "project/project.h"

#include <string.h>

#include "core/logger.h"
#include "platform/file.h"
#include "platform/path.h"

#define RECENT_KEY "project"
#define DEFAULT_LEVEL "main"

namespace project {

static const char *cfg_path(memory::Arena *a, const char *dir) {
    return FPATH(a, dir, PROJECT_CFG);
}

b32 is_project(const char *dir) {
    ASSERT(dir);
    char path[PROJECT_PATH_CAP];
    snprintf(path, sizeof(path), "%s/%s", dir, PROJECT_CFG);
    return file::exists(path);
}

// sets a key's single value, adding the key when the file lacks it
static void set_key(memory::Arena *a, config::Doc *doc, const char *key, const char *value) {
    const char *vals[] = {value};
    config::Node *n = config::find(doc->first, key);
    if (n)
        config::set_values(a, n, vals, 1);
    else
        config::append(a, doc, NULL, key, vals, 1);
}

result::Error open(memory::Arena *a, const char *dir, Project *out) {
    ASSERT(a && dir && out);

    if (snprintf(out->dir, sizeof(out->dir), "%s", dir) >= (i32)sizeof(out->dir)) {
        LOG_ERR("project: the path %s is longer than %d bytes", dir, PROJECT_PATH_CAP);
        return result::ERR_INVALID;
    }

    const char *path = cfg_path(a, dir);
    config::DocResult doc = config::load(a, path);
    if (doc.err != result::OK) {
        LOG_ERR("project: %s has no readable %s", dir, PROJECT_CFG);
        return doc.err;
    }
    if (doc.val->problems) {
        LOG_ERR("project: %s has %u structural problem(s)", path, doc.val->problems);
        return result::ERR_PARSE;
    }

    out->doc = doc.val;
    snprintf(out->name, sizeof(out->name), "%s", config::get(doc.val->first, "name", "untitled"));
    snprintf(out->title, sizeof(out->title), "%s",
             config::get(doc.val->first, "title", out->name));
    snprintf(out->level, sizeof(out->level), "%s",
             config::get(doc.val->first, "level", DEFAULT_LEVEL));
    snprintf(out->script, sizeof(out->script), "%s",
             config::get(doc.val->first, "script", PROJECT_DEFAULT_SCRIPT));

    LOG_INFO("project: opened '%s' at %s, level '%s'", out->name, dir, out->level);
    return result::OK;
}

result::Error save(memory::Arena *scratch, Project *p) {
    ASSERT(scratch && p && p->doc);

    memory::ArenaTemp t = memory::temp_begin(scratch);
    set_key(scratch, p->doc, "name", p->name);
    set_key(scratch, p->doc, "title", p->title);
    set_key(scratch, p->doc, "level", p->level);
    set_key(scratch, p->doc, "script", p->script);

    // the values above were pushed into scratch; the doc keeps pointing at them
    // only until this scope ends, so the file is written before that
    result::Error e = config::save(scratch, p->doc, cfg_path(scratch, p->dir));
    memory::temp_end(t);

    if (e == result::OK)
        LOG_INFO("project: saved %s", p->dir);
    return e;
}

result::Error create(memory::Arena *a, memory::Arena *scratch, const char *engine_root,
                     const char *parent, const char *name, Project *out) {
    ASSERT(a && scratch && engine_root && parent && name && out);

    if (!name[0] || strlen(name) >= PROJECT_NAME_CAP) {
        LOG_ERR("project: '%s' is not a usable name", name);
        return result::ERR_INVALID;
    }
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\' || *c == ':') {
            LOG_ERR("project: '%s' has a path separator in its name", name);
            return result::ERR_INVALID;
        }
    }

    memory::ArenaTemp t = memory::temp_begin(scratch);
    const char *dir = FPATH(scratch, parent, name);
    if (file::exists(cfg_path(scratch, dir))) {
        LOG_ERR("project: %s already holds a project", dir);
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    result::Error e = file::copy_directory(FPATH(scratch, engine_root, PROJECT_TEMPLATE_DIR), dir);
    if (e != result::OK) {
        memory::temp_end(t);
        return e;
    }

    // the template's own folders that ship empty do not survive a copy, so they are made
    static const char *DIRS[] = {PROJECT_LEVEL_DIR, PROJECT_ENTITY_DIR, PROJECT_MODEL_DIR,
                                 PROJECT_ANIMATION_DIR, PROJECT_SCRIPT_DIR};
    for (u32 i = 0; i < ARRAY_COUNT(DIRS); i++)
        if (!file::make_directory(FPATH(scratch, dir, DIRS[i]))) {
            memory::temp_end(t);
            return result::ERR_IO;
        }

    char dir_copy[PROJECT_PATH_CAP];
    snprintf(dir_copy, sizeof(dir_copy), "%s", dir);
    memory::temp_end(t);

    e = open(a, dir_copy, out);
    if (e != result::OK)
        return e;

    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->title, sizeof(out->title), "%s", name);
    e = save(scratch, out);
    if (e == result::OK)
        LOG_INFO("project: created '%s' at %s", name, dir_copy);
    return e;
}

u32 recent(memory::Arena *a, const char *engine_root, const char **out, u32 cap) {
    ASSERT(a && engine_root && (out || cap == 0));

    const char *path = FPATH(a, engine_root, PROJECT_RECENT_FILE);
    if (!file::exists(path))
        return 0;

    config::DocResult doc = config::load(a, path);
    if (doc.err != result::OK)
        return 0;

    u32 n = 0;
    for (config::Node *p = config::find(doc.val->first, RECENT_KEY); p && n < cap;
         p = config::find_next(p, RECENT_KEY)) {
        const char *dir = config::value(p, 0, NULL);
        if (dir)
            out[n++] = dir;
    }
    return n;
}

static void save_recent(memory::Arena *scratch, const char *engine_root, const char *const *dirs,
                        u32 n) {
    const char *path = FPATH(scratch, engine_root, PROJECT_RECENT_FILE);
    config::Doc *doc = config::create(scratch, path);
    for (u32 i = 0; i < n; i++) {
        const char *vals[] = {dirs[i]};
        config::append(scratch, doc, NULL, RECENT_KEY, vals, 1);
    }
    config::save(scratch, doc, path);
}

void remember(memory::Arena *scratch, const char *engine_root, const char *dir, u32 cap) {
    ASSERT(scratch && engine_root && dir);
    if (cap == 0)
        return;

    memory::ArenaTemp t = memory::temp_begin(scratch);

    const char **old = push_array(scratch, const char *, cap);
    u32 n = recent(scratch, engine_root, old, cap);

    const char **list = push_array(scratch, const char *, cap);
    u32 kept = 0;
    list[kept++] = dir;
    for (u32 i = 0; i < n && kept < cap; i++)
        if (strcmp(old[i], dir) != 0)
            list[kept++] = old[i];

    save_recent(scratch, engine_root, list, kept);
    memory::temp_end(t);
}

void forget(memory::Arena *scratch, const char *engine_root, const char *dir, u32 cap) {
    ASSERT(scratch && engine_root && dir);
    if (cap == 0)
        return;

    memory::ArenaTemp t = memory::temp_begin(scratch);

    const char **list = push_array(scratch, const char *, cap);
    u32 n = recent(scratch, engine_root, list, cap);

    u32 kept = 0;
    for (u32 i = 0; i < n; i++)
        if (strcmp(list[i], dir) != 0)
            list[kept++] = list[i];

    if (kept != n)
        save_recent(scratch, engine_root, list, kept);
    memory::temp_end(t);
}

} // namespace project
