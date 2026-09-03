#pragma once

#include "core/common.h"

// A single line of UTF-8 under edit: a cursor, a selection anchor, and the
// operations the editing keys map to. The bytes belong to the caller. The text
// never grows past cap, and what would not fit is dropped. Offsets are bytes,
// always on a codepoint boundary. No drawing and no input here, so a widget
// drives it and a test drives it the same way.

namespace textedit {

struct Buffer {
    char *text; // NUL-terminated
    u32 cap;    // bytes, the NUL included
    u32 len;    // bytes before the NUL
    u32 cursor;
    u32 anchor; // the other end of the selection; equal to cursor when there is none
};

// Adopts what is already in text, cursor at its end.
Buffer create(char *text, u32 cap);

// Replaces everything, cursor at the end.
void set(Buffer *b, const char *text);

b32 has_selection(const Buffer *b);
void selection(const Buffer *b, u32 *out_lo, u32 *out_hi);
void select_all(Buffer *b);

// Each returns whether the text changed.
void insert(Buffer *b, const char *utf8); // replaces the selection, or inserts at the cursor
b32 erase_backward(Buffer *b);            // the selection, or the codepoint before the cursor
b32 erase_forward(Buffer *b);             // the selection, or the codepoint after it

// direction is -1 or +1: one codepoint, or one word. With select the anchor
// stays put; without it a selection collapses toward the side moved to.
void move(Buffer *b, i32 direction, b32 by_word, b32 select);
void home(Buffer *b, b32 select);
void end(Buffer *b, b32 select);

} // namespace textedit
