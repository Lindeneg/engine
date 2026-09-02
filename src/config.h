#pragma once

#include "common.h"
#include "linalg.h"
#include "memory.h"
#include "result.h"

// A small block-structured text format for everything the user configures at
// edit time: which shaders exist, how an asset is imported, what a material
// overrides. The editor writes these files and the engine reads them; both
// go through this module, so a file survives a round trip.
//
//   # comment to end of line
//   shader mesh {
//       vert     mesh.vert      # trailing comments are kept too
//       features SKINNED ALPHA_MASK
//   }
//
// Line-oriented: a node is a key, zero or more value tokens, and an optional
// child block. '{' and '}' are their own tokens and only bare ones are
// structure -- a quoted "{" is a value. A value is quoted when it contains
// whitespace, '#', '{', '}', '"' or '\'; inside quotes, \" and \\ escape.
// Comment lines and blank lines are nodes with a NULL key, so a writer emits
// them where they were. Output is canonical (4-space indent, single spaces),
// not byte-preserving.
//
// Deliberately not JSON. This has to stay hand-editable and diffable, and a
// reader must be able to skip keys it does not recognise -- an engine reading a
// file a newer editor wrote should ignore the extra metadata, not reject the
// file. Unknown keys are therefore never an error. Structural damage (a stray
// '}', an unclosed block) is counted in Doc.problems and left to the caller:
// a shader manifest refuses it, a material sidecar shrugs.

#define CONFIG_MAX_DEPTH 16
#define CONFIG_MAX_TOKENS 32

namespace config {

struct Node {
    const char *key; // NULL for a comment or blank line
    const char **values;
    u32 value_count;
    const char *comment; // text after '#', or NULL
    u32 line;            // 1-based source line; 0 for a node added in memory
    b32 is_block;        // written with braces, even when the body is empty

    Node *parent; // NULL at top level
    Node *first_child;
    Node *next_sibling;
};

struct Doc {
    Node *first;      // top-level nodes
    const char *name; // source path, for diagnostics
    u32 problems;     // structural errors the parser warned about
};

RESULT_TYPE(Doc *, DocResult);

// Tokenizes in place, so text must be writable and NUL-terminated, and it has
// to outlive the Doc: keys and values point into it rather than being copied.
DocResult parse(memory::Arena *a, char *text, const char *name);

// Reads path into a and parses it. Everything -- text and nodes -- lives in a.
DocResult load(memory::Arena *a, const char *path);

// first is any node in a sibling chain, usually doc->first or n->first_child.
// Comment and blank nodes never match.
Node *find(Node *first, const char *key);

// Continues a scan past from, for a key that repeats within one block.
Node *find_next(Node *from, const char *key);

// All return the fallback when the node or the index is missing, so a reader
// reads as a straight run of gets instead of a ladder of null checks.
const char *value(Node *n, u32 index, const char *fallback);
f32 value_f32(Node *n, u32 index, f32 fallback);
i32 value_i32(Node *n, u32 index, i32 fallback);
b32 value_b32(Node *n, u32 index, b32 fallback); // true/yes/on/1

// The first value of `key` within a block: what nearly every reader wants.
const char *get(Node *block, const char *key, const char *fallback);
f32 get_f32(Node *block, const char *key, f32 fallback);
i32 get_i32(Node *block, const char *key, i32 fallback);
b32 get_b32(Node *block, const char *key, b32 fallback);

// Component-wise, each falling back on its own; "color 1 0.5" keeps fallback.z.
linalg::Vec3 value_vec3(Node *n, linalg::Vec3 fallback);
linalg::Vec4 value_vec4(Node *n, linalg::Vec4 fallback);

// Editing. Strings are copied into a, so callers may pass stack buffers. A
// node appended under a parent makes that parent a block. unlink detaches a
// node; its bytes stay until the arena resets.
Doc *create(memory::Arena *a, const char *name);
Node *append(memory::Arena *a, Doc *doc, Node *parent, const char *key,
             const char *const *values, u32 count);
void set_values(memory::Arena *a, Node *n, const char *const *values, u32 count);
void unlink(Doc *doc, Node *n);

// Canonical text of the whole document, NUL-terminated, pushed into a.
char *write(memory::Arena *a, const Doc *doc);
result::Error save(memory::Arena *scratch, const Doc *doc, const char *path);

} // namespace config
