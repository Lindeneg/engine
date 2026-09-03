#include "editor/undo.h"

#include <string.h>

#include "core/config.h"
#include "core/logger.h"

// the key a subtree is written under in a blob
#define BLOB_KEY "entity"
#define BLOB_DOC_NAME "<undo>"

namespace undo {

enum Kind {
    KIND_FIELD,
    KIND_ADD,
    KIND_REMOVE,
    KIND_CREATE, // forward: exists; backward: gone
    KIND_DELETE, // forward: gone; backward: exists
    KIND_REPARENT,
    KIND_IMPORT_OPTIONS,
    KIND_CLIP_OPTIONS,
};

struct Entry {
    Kind kind;
    u32 id;
    i32 component;
    u32 offset;
    u32 size;
    b32 open; // still merging with edits of the same target

    // the existence kinds: where the subtree hangs; the reparent kind: both ends
    u32 parent;
    u32 old_parent;
    char joint[ENTITY_NAME_CAP];

    char path[ENTITY_PATH_CAP]; // the asset, for the options kinds
    u8 before[UNDO_VALUE_CAP];
    u8 after[UNDO_VALUE_CAP];
};

struct Journal {
    memory::Arena *scratch;
    Entry *entries;
    char *blobs; // depth * blob_bytes; entry i owns blob i
    u32 blob_bytes;
    u32 cap;
    u32 count;  // held
    u32 cursor; // applied; the rest are redo
};

#define X(ID, Type, key)                                                                           \
    static_assert(sizeof(entity::Type) <= UNDO_VALUE_CAP, #Type " does not fit an undo entry");
COMPONENT_LIST(X)
#undef X
static_assert(ENTITY_NAME_CAP <= UNDO_VALUE_CAP, "an entity name does not fit an undo entry");
static_assert(sizeof(entity::Transform) <= UNDO_VALUE_CAP, "a transform does not fit an undo entry");
static_assert(sizeof(asset::ImportOptions) <= UNDO_VALUE_CAP, "import options do not fit");
static_assert(sizeof(asset::ClipOptions) <= UNDO_VALUE_CAP, "clip options do not fit");

Journal *create(memory::Arena *a, memory::Arena *scratch, u32 depth, u32 blob_bytes) {
    ASSERT(a && scratch);
    ASSERT_MSG(depth > 0 && blob_bytes > 0, "undo: zero depth or blob");

    Journal *j = push_struct(a, Journal);
    j->scratch = scratch;
    j->entries = push_array(a, Entry, depth);
    j->blobs = push_array(a, char, (u64)depth * blob_bytes);
    j->blob_bytes = blob_bytes;
    j->cap = depth;
    return j;
}

static char *blob(Journal *j, u32 index) { return j->blobs + (u64)index * j->blob_bytes; }

// drops the redo tail and, at capacity, the oldest entry with its blob
static Entry *push(Journal *j) {
    j->count = j->cursor;
    if (j->count == j->cap) {
        memmove(j->entries, j->entries + 1, (j->cap - 1) * sizeof(Entry));
        memmove(j->blobs, blob(j, 1), (u64)(j->cap - 1) * j->blob_bytes);
        j->count--;
    }

    Entry *e = &j->entries[j->count++];
    j->cursor = j->count;
    memset(e, 0, sizeof(*e));
    return e;
}

static Entry *top(Journal *j) { return j->cursor ? &j->entries[j->cursor - 1] : NULL; }

void record_field(Journal *j, u32 id, i32 component, u32 offset, u32 size, const void *before,
                  const void *after, b32 continuing) {
    ASSERT(j && before && after);
    ASSERT_MSG(size <= UNDO_VALUE_CAP, "undo: a %u byte field does not fit an entry", size);

    Entry *t = top(j);
    if (continuing && t && t->open && t->kind == KIND_FIELD && t->id == id &&
        t->component == component && t->offset == offset && t->size == size) {
        memcpy(t->after, after, size);
        return;
    }

    Entry *e = push(j);
    e->kind = KIND_FIELD;
    e->id = id;
    e->component = component;
    e->offset = offset;
    e->size = size;
    e->open = continuing;
    memcpy(e->before, before, size);
    memcpy(e->after, after, size);
}

void record_add(Journal *j, u32 id, entity::Component c, const void *bytes) {
    ASSERT(j && bytes);
    Entry *e = push(j);
    e->kind = KIND_ADD;
    e->id = id;
    e->component = (i32)c;
    e->size = (u32)entity::component_desc(c)->size;
    memcpy(e->after, bytes, e->size);
}

void record_remove(Journal *j, u32 id, entity::Component c, const void *bytes) {
    ASSERT(j && bytes);
    Entry *e = push(j);
    e->kind = KIND_REMOVE;
    e->id = id;
    e->component = (i32)c;
    e->size = (u32)entity::component_desc(c)->size;
    memcpy(e->before, bytes, e->size);
}

// the subtree as a save would write it, plus the joint it hangs from, which the
// writer leaves to the referencing block; false when it does not fit
static b32 write_subtree(Journal *j, entity::World *w, entity::Handle h, char *out) {
    entity::Entity *e = entity::get(w, h);
    memory::ArenaTemp t = memory::temp_begin(j->scratch);

    config::Doc *doc = config::create(j->scratch, BLOB_DOC_NAME);
    entity::write_entity(j->scratch, doc, NULL, w, h, BLOB_KEY, false);
    if (e->parent.v && e->attach_joint[0]) {
        const char *joint[] = {e->attach_joint};
        config::append(j->scratch, doc, doc->first, "attach_joint", joint, 1);
    }

    char *text = config::write(j->scratch, doc);
    u64 len = strlen(text);
    b32 fits = len + 1 <= j->blob_bytes;
    if (fits)
        memcpy(out, text, len + 1);
    else
        LOG_WARN("undo: \"%s\" and its children are %llu bytes, more than the %u an entry holds; "
                 "this cannot be undone",
                 e->name, (unsigned long long)len, j->blob_bytes);

    memory::temp_end(t);
    return fits;
}

static b32 record_existence(Journal *j, entity::World *w, entity::Handle h, Kind kind) {
    ASSERT(j && w);
    entity::Entity *e = entity::get(w, h);
    if (!e)
        return false;

    // written before the entry is pushed, so a subtree that does not fit leaves no trace
    char *scratch_blob = blob(j, j->cursor < j->cap ? j->cursor : j->cap - 1);
    if (j->cursor == j->cap)
        scratch_blob = blob(j, 0); // about to be freed by push
    if (!write_subtree(j, w, h, scratch_blob))
        return false;

    // push may move the blobs down by one; write again into the slot the entry owns
    Entry *entry = push(j);
    u32 index = (u32)(entry - j->entries);
    if (blob(j, index) != scratch_blob)
        write_subtree(j, w, h, blob(j, index));

    entry->kind = kind;
    entry->id = e->id;
    entry->parent = entity::id_of(w, e->parent);
    return true;
}

b32 record_create(Journal *j, entity::World *w, entity::Handle h) {
    return record_existence(j, w, h, KIND_CREATE);
}

b32 record_delete(Journal *j, entity::World *w, entity::Handle h) {
    return record_existence(j, w, h, KIND_DELETE);
}

void record_reparent(Journal *j, u32 id, u32 old_parent, u32 new_parent, const char *old_joint,
                     const entity::Transform *before, const entity::Transform *after) {
    ASSERT(j && before && after);
    Entry *e = push(j);
    e->kind = KIND_REPARENT;
    e->id = id;
    e->old_parent = old_parent;
    e->parent = new_parent;
    snprintf(e->joint, sizeof(e->joint), "%s", old_joint ? old_joint : "");
    e->size = sizeof(entity::Transform);
    memcpy(e->before, before, sizeof(*before));
    memcpy(e->after, after, sizeof(*after));
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

// the subtree in the blob, back under its parent with its ids
static b32 respawn(Journal *j, entity::World *w, asset::Manager *m, Entry *e, u32 index) {
    if (entity::find_by_id(w, e->id).v)
        return true; // already there: an undo of a delete that was itself undone

    entity::Handle parent = entity::find_by_id(w, e->parent);
    if (e->parent && !parent.v) {
        LOG_WARN("undo: the parent of entity %u is gone; it comes back at the top level", e->id);
    }

    memory::ArenaTemp t = memory::temp_begin(j->scratch);
    char *text = memory::push_str(j->scratch, blob(j, index));
    config::DocResult doc = config::parse(j->scratch, text, BLOB_DOC_NAME);
    b32 ok = doc.err == result::OK && doc.val->first &&
             entity::spawn_from_config(w, m, doc.val->first, parent).v != 0;
    memory::temp_end(t);

    if (!ok)
        LOG_WARN("undo: entity %u could not be brought back", e->id);
    return ok;
}

static void reparent(entity::World *w, asset::Manager *m, entity::Handle h, u32 parent_id,
                     const char *joint, const u8 *transform) {
    entity::Handle parent = entity::find_by_id(w, parent_id);
    if (parent_id && !parent.v) {
        LOG_WARN("undo: parent %u is gone; entity stays where it is", parent_id);
        return;
    }
    if (!entity::set_parent(w, m, h, parent, joint))
        return;
    memcpy(&entity::get(w, h)->local, transform, sizeof(entity::Transform));
}

static b32 apply(Journal *j, entity::World *w, asset::Manager *m, Entry *e, u32 index,
                 b32 forward) {
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

    // existence is the one thing that can be applied to an entity that is not there
    b32 should_exist = (e->kind == KIND_CREATE) == (forward != 0);
    if (e->kind == KIND_CREATE || e->kind == KIND_DELETE) {
        if (should_exist)
            return respawn(j, w, m, e, index);
        entity::Handle h = entity::find_by_id(w, e->id);
        if (h.v)
            entity::destroy(w, h);
        return true;
    }

    entity::Handle h = entity::find_by_id(w, e->id);
    if (!h.v) {
        LOG_WARN("undo: entity %u is gone; skipping the entry", e->id);
        return false;
    }

    switch (e->kind) {
    case KIND_FIELD: {
        if (e->component == UNDO_ENTITY) {
            memcpy((u8 *)entity::get(w, h) + e->offset, bytes, e->size);
            return true;
        }

        entity::Component c = (entity::Component)e->component;
        void *comp = entity::component(w, h, c);
        if (!comp) {
            LOG_WARN("undo: entity %u no longer has the component; skipping the entry", e->id);
            return false;
        }
        memcpy((u8 *)comp + e->offset, bytes, e->size);
        entity::component_desc(c)->after_read(w, m, h, comp);
        return true;
    }

    case KIND_ADD:
        if (forward)
            return restore_component(w, m, h, (entity::Component)e->component, e->after, e->size);
        entity::remove_component(w, h, (entity::Component)e->component);
        return true;

    case KIND_REMOVE:
        if (forward) {
            entity::remove_component(w, h, (entity::Component)e->component);
            return true;
        }
        return restore_component(w, m, h, (entity::Component)e->component, e->before, e->size);

    case KIND_REPARENT:
        if (forward)
            reparent(w, m, h, e->parent, NULL, e->after);
        else
            reparent(w, m, h, e->old_parent, e->joint[0] ? e->joint : NULL, e->before);
        return true;

    case KIND_CREATE:
    case KIND_DELETE:
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

    u32 index = --j->cursor;
    Entry *e = &j->entries[index];
    e->open = false;
    apply(j, w, m, e, index, false);
    return true;
}

b32 redo(Journal *j, entity::World *w, asset::Manager *m) {
    ASSERT(j && w && m);
    if (j->cursor == j->count)
        return false;

    u32 index = j->cursor++;
    apply(j, w, m, &j->entries[index], index, true);
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
