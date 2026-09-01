#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "logger.h"

namespace config {

// Splits one line into whitespace-separated tokens, writing NULs over the
// separators. A '#' only starts a comment at a token boundary, so a value may
// contain one; quote a value to keep spaces in it.
static u32 tokenize(char *line, char **out, u32 max) {
    u32 n = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            break;

        char *tok;
        if (*p == '"') {
            tok = ++p;
            while (*p && *p != '"')
                p++;
            if (*p)
                *p++ = 0;
        } else {
            tok = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = 0;
        }

        if (n == max)
            return n; // caller reports; the line is otherwise well formed
        out[n++] = tok;
    }

    return n;
}

DocResult parse(memory::Arena *a, char *text, const char *name) {
    ASSERT(a && text);

    Doc *doc = push_struct(a, Doc);
    doc->name = name ? name : "<memory>";

    // Where the next sibling goes, one entry per open block. Storing the tail
    // pointer rather than the parent node keeps append at O(1) without a
    // last-child field that nothing else would use.
    Node **tail_stack[CONFIG_MAX_DEPTH];
    u32 depth = 0;
    Node **tail = &doc->first;

    char *p = text;
    u32 line_no = 0;

    while (*p) {
        char *line = p;
        while (*p && *p != '\n')
            p++;

        char *line_end = p;
        if (*p)
            p++; // step over the newline
        if (line_end > line && line_end[-1] == '\r')
            line_end--;
        *line_end = 0;
        line_no++;

        char *toks[CONFIG_MAX_TOKENS];
        u32 n = tokenize(line, toks, ARRAY_COUNT(toks));
        if (n == 0)
            continue;
        if (n == (u32)ARRAY_COUNT(toks))
            LOG_WARN("config: %s:%u has more than %u tokens, the rest are dropped", doc->name,
                     line_no, (u32)ARRAY_COUNT(toks));

        if (strcmp(toks[0], "}") == 0) {
            if (depth == 0) {
                LOG_WARN("config: %s:%u unmatched '}'", doc->name, line_no);
                continue;
            }
            tail = tail_stack[--depth];
            continue;
        }

        b32 opens = strcmp(toks[n - 1], "{") == 0;
        if (opens)
            n--;
        if (n == 0) {
            LOG_WARN("config: %s:%u block has no key", doc->name, line_no);
            continue;
        }

        Node *node = push_struct(a, Node);
        node->key = toks[0];
        node->value_count = n - 1;
        if (node->value_count) {
            node->values = push_array(a, const char *, node->value_count);
            for (u32 i = 0; i < node->value_count; i++)
                node->values[i] = toks[i + 1];
        }

        *tail = node;
        tail = &node->next_sibling;

        if (!opens)
            continue;

        if (depth == CONFIG_MAX_DEPTH) {
            LOG_WARN("config: %s:%u nested deeper than %d, block ignored", doc->name, line_no,
                     CONFIG_MAX_DEPTH);
            continue;
        }
        tail_stack[depth++] = tail;
        tail = &node->first_child;
    }

    if (depth != 0)
        LOG_WARN("config: %s ended with %u block(s) still open", doc->name, depth);

    return DocResult{.val = doc};
}

DocResult load(memory::Arena *a, const char *path) {
    ASSERT(a && path);

    // The text stays in a for the life of the Doc: nodes point into it.
    file::BlobResult blob = file::read(a, path);
    if (blob.err != result::OK)
        return DocResult{.err = blob.err};

    return parse(a, (char *)blob.val.data, path);
}

Node *find(Node *first, const char *key) {
    for (Node *n = first; n; n = n->next_sibling)
        if (strcmp(n->key, key) == 0)
            return n;
    return NULL;
}

Node *find_next(Node *from, const char *key) {
    return from ? find(from->next_sibling, key) : NULL;
}

const char *value(Node *n, u32 index, const char *fallback) {
    if (!n || index >= n->value_count)
        return fallback;
    return n->values[index];
}

f32 value_f32(Node *n, u32 index, f32 fallback) {
    const char *v = value(n, index, NULL);
    if (!v)
        return fallback;

    char *end = NULL;
    f32 out = strtof(v, &end);
    if (end == v) {
        LOG_WARN("config: '%s' is not a number", v);
        return fallback;
    }
    return out;
}

i32 value_i32(Node *n, u32 index, i32 fallback) {
    const char *v = value(n, index, NULL);
    if (!v)
        return fallback;

    char *end = NULL;
    long out = strtol(v, &end, 10);
    if (end == v) {
        LOG_WARN("config: '%s' is not an integer", v);
        return fallback;
    }
    return (i32)out;
}

b32 value_b32(Node *n, u32 index, b32 fallback) {
    const char *v = value(n, index, NULL);
    if (!v)
        return fallback;

    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "on") == 0 ||
        strcmp(v, "1") == 0)
        return true;
    if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "off") == 0 ||
        strcmp(v, "0") == 0)
        return false;

    LOG_WARN("config: '%s' is not a boolean", v);
    return fallback;
}

} // namespace config
