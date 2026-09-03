#include "editor/undo.h"

#include <string.h>

#include "core/logger.h"

namespace undo {

enum Kind { KIND_FIELD, KIND_ADD, KIND_REMOVE, KIND_IMPORT_OPTIONS, KIND_CLIP_OPTIONS };

struct Entry {
    Kind kind;
    entity::Handle entity;
    i32 component;
    u32 offset;
    u32 size;
    b32 open; // still merging with edits of the same target
    char path[ENTITY_PATH_CAP]; // the asset, for the options kinds
    u8 before[UNDO_VALUE_CAP];
    u8 after[UNDO_VALUE_CAP];
};

static_assert(sizeof(asset::ImportOptions) <= UNDO_VALUE_CAP, "import options do not fit");
static_assert(sizeof(asset::ClipOptions) <= UNDO_VALUE_CAP, "clip options do not fit");

struct Journal {
    Entry *entries;
    u32 cap;
    u32 count;  // held
    u32 cursor; // applied; the rest are redo
};

#define X(ID, Type, key)                                                                           \
    static_assert(sizeof(entity::Type) <= UNDO_VALUE_CAP, #Type " does not fit an undo entry");
COMPONENT_LIST(X)
#undef X
static_assert(ENTITY_NAME_CAP <= UNDO_VALUE_CAP, "an entity name does not fit an undo entry");

Journal *create(memory::Arena *a, u32 depth) {
    ASSERT(a);
    ASSERT_MSG(depth > 0, "undo: zero depth");

    Journal *j = push_struct(a, Journal);
    j->entries = push_array(a, Entry, depth);
    j->cap = depth;
    return j;
}

// drops the redo tail and, at capacity, the oldest entry
static Entry *push(Journal *j) {
    j->count = j->cursor;
    if (j->count == j->cap) {
        memmove(j->entries, j->entries + 1, (j->cap - 1) * sizeof(Entry));
        j->count--;
    }

    Entry *e = &j->entries[j->count++];
    j->cursor = j->count;
    memset(e, 0, sizeof(*e));
    return e;
}

static Entry *top(Journal *j) { return j->cursor ? &j->entries[j->cursor - 1] : NULL; }

void record_field(Journal *j, entity::Handle h, i32 component, u32 offset, u32 size,
                  const void *before, const void *after, b32 continuing) {
    ASSERT(j && before && after);
    ASSERT_MSG(size <= UNDO_VALUE_CAP, "undo: a %u byte field does not fit an entry", size);

    Entry *t = top(j);
    if (continuing && t && t->open && t->kind == KIND_FIELD && t->entity.v == h.v &&
        t->component == component && t->offset == offset && t->size == size) {
        memcpy(t->after, after, size);
        return;
    }

    Entry *e = push(j);
    e->kind = KIND_FIELD;
    e->entity = h;
    e->component = component;
    e->offset = offset;
    e->size = size;
    e->open = continuing;
    memcpy(e->before, before, size);
    memcpy(e->after, after, size);
}

void record_add(Journal *j, entity::Handle h, entity::Component c, const void *bytes) {
    ASSERT(j && bytes);
    Entry *e = push(j);
    e->kind = KIND_ADD;
    e->entity = h;
    e->component = (i32)c;
    e->size = (u32)entity::component_desc(c)->size;
    memcpy(e->after, bytes, e->size);
}

void record_remove(Journal *j, entity::Handle h, entity::Component c, const void *bytes) {
    ASSERT(j && bytes);
    Entry *e = push(j);
    e->kind = KIND_REMOVE;
    e->entity = h;
    e->component = (i32)c;
    e->size = (u32)entity::component_desc(c)->size;
    memcpy(e->before, bytes, e->size);
}

void record_import_options(Journal *j, const char *rel_path, asset::ImportOptions before,
                           asset::ImportOptions after, b32 continuing) {
    ASSERT(j && rel_path);

    Entry *t = top(j);
    if (continuing && t && t->open && t->kind == KIND_IMPORT_OPTIONS &&
        strcmp(t->path, rel_path) == 0) {
        memcpy(t->after, &after, sizeof(after));
        return;
    }

    Entry *e = push(j);
    e->kind = KIND_IMPORT_OPTIONS;
    e->size = sizeof(after);
    e->open = continuing;
    snprintf(e->path, sizeof(e->path), "%s", rel_path);
    memcpy(e->before, &before, sizeof(before));
    memcpy(e->after, &after, sizeof(after));
}

void record_clip_options(Journal *j, const char *rel_path, asset::ClipOptions before,
                         asset::ClipOptions after) {
    ASSERT(j && rel_path);

    Entry *e = push(j);
    e->kind = KIND_CLIP_OPTIONS;
    e->size = sizeof(after);
    snprintf(e->path, sizeof(e->path), "%s", rel_path);
    memcpy(e->before, &before, sizeof(before));
    memcpy(e->after, &after, sizeof(after));
}

void close(Journal *j) {
    ASSERT(j);
    Entry *t = top(j);
    if (t)
        t->open = false;
}

void clear(Journal *j) {
    ASSERT(j);
    j->count = 0;
    j->cursor = 0;
}

b32 can_undo(const Journal *j) {
    ASSERT(j);
    return j->cursor > 0;
}

b32 can_redo(const Journal *j) {
    ASSERT(j);
    return j->cursor < j->count;
}

// puts a component back with the bytes it had, and lets it resolve what they name
static b32 restore_component(entity::World *w, asset::Manager *m, entity::Handle h,
                             entity::Component c, const u8 *bytes, u32 size) {
    void *comp = entity::add_component(w, h, c);
    if (!comp)
        return false;
    memcpy(comp, bytes, size);
    entity::component_desc(c)->after_read(w, m, h, comp);
    return true;
}

static b32 apply(entity::World *w, asset::Manager *m, Entry *e, b32 forward) {
    const u8 *bytes = forward ? e->after : e->before;

    // the asset kinds name a file, not an entity
    if (e->kind == KIND_IMPORT_OPTIONS) {
        asset::ImportOptions opts;
        memcpy(&opts, bytes, sizeof(opts));
        asset::apply_import_options(m, e->path, opts);
        return asset::write_import_options(m, e->path, opts) == result::OK;
    }
    if (e->kind == KIND_CLIP_OPTIONS) {
        asset::ClipOptions opts;
        memcpy(&opts, bytes, sizeof(opts));
        asset::apply_clip_options(m, e->path, opts);
        return asset::write_clip_options(m, e->path, opts) == result::OK;
    }

    if (!entity::alive(w, e->entity)) {
        LOG_WARN("undo: entity %u is gone; skipping the entry", e->entity.v);
        return false;
    }

    switch (e->kind) {
    case KIND_FIELD: {
        if (e->component == UNDO_ENTITY) {
            memcpy((u8 *)entity::get(w, e->entity) + e->offset, bytes, e->size);
            return true;
        }

        entity::Component c = (entity::Component)e->component;
        void *comp = entity::component(w, e->entity, c);
        if (!comp) {
            LOG_WARN("undo: entity %u no longer has the component; skipping the entry",
                     e->entity.v);
            return false;
        }
        memcpy((u8 *)comp + e->offset, bytes, e->size);
        entity::component_desc(c)->after_read(w, m, e->entity, comp);
        return true;
    }

    case KIND_ADD:
        if (forward)
            return restore_component(w, m, e->entity, (entity::Component)e->component, e->after,
                                     e->size);
        entity::remove_component(w, e->entity, (entity::Component)e->component);
        return true;

    case KIND_REMOVE:
        if (forward) {
            entity::remove_component(w, e->entity, (entity::Component)e->component);
            return true;
        }
        return restore_component(w, m, e->entity, (entity::Component)e->component, e->before,
                                 e->size);

    case KIND_IMPORT_OPTIONS:
    case KIND_CLIP_OPTIONS:
        break; // handled above
    }
    return false;
}

b32 undo(Journal *j, entity::World *w, asset::Manager *m) {
    ASSERT(j && w && m);
    if (j->cursor == 0)
        return false;

    Entry *e = &j->entries[--j->cursor];
    e->open = false;
    apply(w, m, e, false);
    return true;
}

b32 redo(Journal *j, entity::World *w, asset::Manager *m) {
    ASSERT(j && w && m);
    if (j->cursor == j->count)
        return false;

    Entry *e = &j->entries[j->cursor++];
    apply(w, m, e, true);
    return true;
}

u32 count(const Journal *j) {
    ASSERT(j);
    return j->count;
}

u32 cursor(const Journal *j) {
    ASSERT(j);
    return j->cursor;
}

} // namespace undo
