#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/memory.h"
#include "scene/entity.h"

// The undo journal: a list of what changed, each entry holding the bytes
// before and after, replayed through the entity and descriptor tables. It
// knows nothing about widgets: the inspector records what it changed, and the
// journal puts it back.
//
// A change is recorded after it was made in place. With `continuing`, an edit
// of the same target on top of the journal is merged into, so a slider drag or
// a run of keystrokes is one entry; close() ends the merging.
//
// Only values and components are journaled. Creating, deleting or reparenting
// an entity clears the journal, since a recycled handle would leave every
// older entry pointing at the wrong thing. That is a deliberate limit of this
// slice, not an oversight; stable entity identity is its own design.

// the largest thing an entry carries: a whole component, or an entity's name
#define UNDO_VALUE_CAP 256

// the component index that means the entity's own fields
#define UNDO_ENTITY (-1)

namespace undo {

struct Journal;

Journal *create(memory::Arena *a, u32 depth);

// A field already changed in place. component is an entity::Component, or
// UNDO_ENTITY with an offset into the Entity itself.
void record_field(Journal *j, entity::Handle h, i32 component, u32 offset, u32 size,
                  const void *before, const void *after, b32 continuing);

// A component just added, with its bytes as they stand; one about to be
// removed, with the bytes it had.
void record_add(Journal *j, entity::Handle h, entity::Component c, const void *bytes);
void record_remove(Journal *j, entity::Handle h, entity::Component c, const void *bytes);

// An asset's import settings, already written and applied. Replaying one
// writes the sidecar again and applies it to whatever is loaded.
void record_import_options(Journal *j, const char *rel_path, asset::ImportOptions before,
                           asset::ImportOptions after, b32 continuing);
void record_clip_options(Journal *j, const char *rel_path, asset::ClipOptions before,
                         asset::ClipOptions after);

void close(Journal *j);
void clear(Journal *j);

b32 can_undo(const Journal *j);
b32 can_redo(const Journal *j);

// False when there was nothing to do. An entry whose entity has died is
// skipped with a warning; the journal still moves past it.
b32 undo(Journal *j, entity::World *w, asset::Manager *m);
b32 redo(Journal *j, entity::World *w, asset::Manager *m);

u32 count(const Journal *j);  // entries held
u32 cursor(const Journal *j); // of which applied

} // namespace undo
