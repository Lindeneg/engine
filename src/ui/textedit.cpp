#include "ui/textedit.h"

#include <string.h>

#include "core/str.h"

namespace textedit {

Buffer create(char *text, u32 cap) {
    ASSERT(text && cap);
    Buffer b{};
    b.text = text;
    b.cap = cap;
    b.len = (u32)strlen(text);
    ASSERT_MSG(b.len < cap, "textedit: the text is longer than its cap");
    b.cursor = b.len;
    b.anchor = b.len;
    return b;
}

static void place(Buffer *b, u32 at, b32 select) {
    b->cursor = at;
    if (!select)
        b->anchor = at;
}

void set(Buffer *b, const char *text) {
    ASSERT(b && text);
    u32 n = (u32)strlen(text);
    while (n > b->cap - 1)
        n = str::utf8_prev(text, n);
    memcpy(b->text, text, n);
    b->text[n] = 0;
    b->len = n;
    place(b, n, false);
}

b32 has_selection(const Buffer *b) {
    ASSERT(b);
    return b->cursor != b->anchor;
}

void selection(const Buffer *b, u32 *out_lo, u32 *out_hi) {
    ASSERT(b && out_lo && out_hi);
    *out_lo = MIN(b->cursor, b->anchor);
    *out_hi = MAX(b->cursor, b->anchor);
}

void select_all(Buffer *b) {
    ASSERT(b);
    b->anchor = 0;
    b->cursor = b->len;
}

static void erase_range(Buffer *b, u32 lo, u32 hi) {
    memmove(b->text + lo, b->text + hi, b->len - hi + 1); // the NUL comes along
    b->len -= hi - lo;
    place(b, lo, false);
}

void insert(Buffer *b, const char *utf8) {
    ASSERT(b && utf8);

    if (has_selection(b)) {
        u32 lo, hi;
        selection(b, &lo, &hi);
        erase_range(b, lo, hi);
    }

    // what does not fit is dropped whole codepoints at a time
    u32 n = (u32)strlen(utf8);
    u32 room = b->cap - 1 - b->len;
    while (n > room)
        n = str::utf8_prev(utf8, n);
    if (n == 0)
        return;

    memmove(b->text + b->cursor + n, b->text + b->cursor, b->len - b->cursor + 1);
    memcpy(b->text + b->cursor, utf8, n);
    b->len += n;
    place(b, b->cursor + n, false);
}

b32 erase_backward(Buffer *b) {
    ASSERT(b);

    if (has_selection(b)) {
        u32 lo, hi;
        selection(b, &lo, &hi);
        erase_range(b, lo, hi);
        return true;
    }
    if (b->cursor == 0)
        return false;

    erase_range(b, str::utf8_prev(b->text, b->cursor), b->cursor);
    return true;
}

b32 erase_forward(Buffer *b) {
    ASSERT(b);

    if (has_selection(b)) {
        u32 lo, hi;
        selection(b, &lo, &hi);
        erase_range(b, lo, hi);
        return true;
    }
    if (b->cursor == b->len)
        return false;

    const char *p = b->text + b->cursor;
    str::utf8_next(&p);
    erase_range(b, b->cursor, (u32)(p - b->text));
    return true;
}

// letters, digits, underscore, and every non-ASCII byte, so a multibyte
// sequence is never split by a word step
static b32 is_word(char c) {
    u8 u = (u8)c;
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           u >= 0x80;
}

static u32 word_start(const Buffer *b, u32 from) {
    u32 i = from;
    while (i > 0 && !is_word(b->text[i - 1]))
        i--;
    while (i > 0 && is_word(b->text[i - 1]))
        i--;
    return i;
}

static u32 word_end(const Buffer *b, u32 from) {
    u32 i = from;
    while (i < b->len && !is_word(b->text[i]))
        i++;
    while (i < b->len && is_word(b->text[i]))
        i++;
    return i;
}

void move(Buffer *b, i32 direction, b32 by_word, b32 select) {
    ASSERT(b && (direction == -1 || direction == 1));

    if (!select && has_selection(b)) {
        u32 lo, hi;
        selection(b, &lo, &hi);
        place(b, direction < 0 ? lo : hi, false);
        return;
    }

    u32 at;
    if (direction < 0) {
        at = by_word ? word_start(b, b->cursor) : str::utf8_prev(b->text, b->cursor);
    } else if (by_word) {
        at = word_end(b, b->cursor);
    } else if (b->cursor < b->len) {
        const char *p = b->text + b->cursor;
        str::utf8_next(&p);
        at = (u32)(p - b->text);
    } else {
        at = b->len;
    }
    place(b, at, select);
}

void home(Buffer *b, b32 select) {
    ASSERT(b);
    place(b, 0, select);
}

void end(Buffer *b, b32 select) {
    ASSERT(b);
    place(b, b->len, select);
}

} // namespace textedit
