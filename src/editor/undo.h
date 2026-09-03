#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/memory.h"
#include "scene/entity.h"

// The undo journal: a list of what changed, each entry holding enough to put
// it back, replayed through the entity and descriptor tables. It knows nothing
// about widgets: the inspector records what it changed, and the journal
// restores it.
//
// Entries name entities by id, never by handle, so an entity can be destroyed
// and brought back and its older history still applies. A change is recorded
// after it was made in place. With `continuing`, an edit of the same target on
// top of the journal is merged into, so a slider drag or a run of keystrokes is
// one entry; close() ends the merging.
//
// Creating and deleting carry the entity's subtree as the text a save would
// write, in a blob of a fixed size; a subtree that does not fit is not
// undoable, which the record call says by returning false. Reparenting carries
// both parents, the joint, and the local transform on either side.

// the largest thing a field entry carries: a whole component, or an entity's name
#define UNDO_VALUE_CAP 256

// the component index that means the entity's own fields
#define UNDO_ENTITY (-1)

namespace undo {

struct Journal;

// depth entries, each with blob_bytes for a subtree's text; scratch is for
// writing and parsing that text
Journal *create(memory::Arena *a, memory::Arena *scratch, u32 depth, u32 blob_bytes);

// A field already changed in place. component is an entity::Component, or
// UNDO_ENTITY with an offset into the Entity itself.
void record_field(Journal *j, u32 id, i32 component, u32 offset, u32 size, const void *before,
                  const void *after, b32 continuing);

// A component just added, with its bytes as they stand; one about to be
// removed, with the bytes it had.
void record_add(Journal *j, u32 id, entity::Component c, const void *bytes);
void record_remove(Journal *j, u32 id, entity::Component c, const void *bytes);

// An entity just created, or one about to be destroyed, with its whole
// subtree. False when the subtree does not fit the blob; the caller then knows
// the journal cannot follow and should clear it.
b32 record_create(Journal *j, entity::World *w, entity::Handle h);
b32 record_delete(Journal *j, entity::World *w, entity::Handle h);

// A reparent already done. Parents by id, 0 for none; the joint the child hung
// from before, or NULL; the local transform on either side of the move.
void record_reparent(Journal *j, u32 id, u32 old_parent, u32 new_parent, const char *old_joint,
                     const entity::Transform *before, const entity::Transform *after);

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

// False when there was nothing to do. An entry whose entity is gone is skipped
// with a warning; the journal still moves past it.
b32 undo(Journal *j, entity::World *w, asset::Manager *m);
b32 redo(Journal *j, entity::World *w, asset::Manager *m);

u32 count(const Journal *j);  // entries held
u32 cursor(const Journal *j); // of which applied

} // namespace undo
