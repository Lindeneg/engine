#pragma once

#include "common.h"
#include "memory.h"
#include "result.h"

// A small block-structured text format for everything the user configures at
// edit time: which shaders exist, how an asset is imported, what a material
// overrides. The editor writes these files; the engine only reads them.
//
//   # comment to end of line
//   shader "mesh" {
//       vert     mesh.vert
//       frag     mesh.frag
//       features SKINNED ALPHA_MASK
//   }
//
// A node is a key, zero or more value tokens, and an optional child block. It
// is line-oriented: a node ends at the newline, and '{' / '}' are their own
// whitespace-separated tokens. Quote a value to keep spaces in it.
//
// Deliberately not JSON. This has to stay hand-editable and diffable, and a
// reader must be able to skip keys it does not recognise -- an engine reading a
// file a newer editor wrote should ignore the extra metadata, not reject the
// file. So unknown keys are never an error here; they are simply not looked up.

#define CONFIG_MAX_DEPTH 16
#define CONFIG_MAX_TOKENS 32

namespace config {

struct Node {
    const char *key;
    const char **values;
    u32 value_count;

    Node *first_child;
    Node *next_sibling;
};

struct Doc {
    Node *first;      // top-level nodes
    const char *name; // source path, for diagnostics
};

RESULT_TYPE(Doc *, DocResult);

// Tokenizes in place, so text must be writable and NUL-terminated, and it has
// to outlive the Doc: keys and values point into it rather than being copied.
DocResult parse(memory::Arena *a, char *text, const char *name);

// Reads path into a and parses it. Everything -- text and nodes -- lives in a.
DocResult load(memory::Arena *a, const char *path);

// first is any node in a sibling chain, usually doc->first or n->first_child.
Node *find(Node *first, const char *key);

// Continues a scan past from, for a key that repeats within one block.
Node *find_next(Node *from, const char *key);

// All return the fallback when the node or the index is missing, so a reader
// reads as a straight run of gets instead of a ladder of null checks.
const char *value(Node *n, u32 index, const char *fallback);
f32 value_f32(Node *n, u32 index, f32 fallback);
i32 value_i32(Node *n, u32 index, i32 fallback);
b32 value_b32(Node *n, u32 index, b32 fallback); // true/yes/on/1

} // namespace config
