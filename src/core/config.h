#pragma once

#include "core/common.h"
#include "core/memory.h"
#include "core/result.h"
#include "math/linalg.h"

// The block-structured text format for everything configured at edit time. The
// editor writes these files and the engine reads them, both through here, so a
// file survives a round trip.
//
//   # comment to end of line
//   shader mesh {
//       vert     mesh.vert      # trailing comments are kept too
//       features SKINNED ALPHA_MASK
//   }
//
// A node is a key, zero or more value tokens, and an optional child block. Only
// bare '{' and '}' are structure; a quoted "{" is a value. A value is quoted
// when it contains whitespace, '#', '{', '}', '"' or '\'; inside quotes, \" and
// \\ escape. Comment and blank lines are nodes with a NULL key, so a writer
// emits them where they were. Output is canonical, not byte-preserving.
//
// Unknown keys are never an error, so an engine can read a file a newer editor
// wrote. Structural damage is counted in Doc.problems and left to the caller: a
// shader manifest refuses it, a material sidecar shrugs.

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

// Tokenizes text in place; it must be writable and outlive the Doc, which points into it.
DocResult parse(memory::Arena *a, char *text, const char *name);

DocResult load(memory::Arena *a, const char *path);

// first is any node in a sibling chain; comment and blank nodes never match.
Node *find(Node *first, const char *key);

// Continues past from, for a key that repeats within one block.
Node *find_next(Node *from, const char *key);

// The fallback when the node or the index is missing.
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

// Editing. Strings are copied into a. Appending under a parent makes it a
// block; unlink leaves the node's bytes until the arena resets.
Doc *create(memory::Arena *a, const char *name);
Node *append(memory::Arena *a, Doc *doc, Node *parent, const char *key, const char *const *values,
             u32 count);
void set_values(memory::Arena *a, Node *n, const char *const *values, u32 count);
void unlink(Doc *doc, Node *n);

// Canonical text of the whole document, NUL-terminated, pushed into a.
char *write(memory::Arena *a, const Doc *doc);
result::Error save(memory::Arena *scratch, const Doc *doc, const char *path);

} // namespace config
