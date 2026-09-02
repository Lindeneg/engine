#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "logger.h"

#define INDENT "    "

namespace config {

struct Line {
    char *toks[CONFIG_MAX_TOKENS];
    b8 quoted[CONFIG_MAX_TOKENS];
    u32 count;
    const char *comment;
    b8 overflow;
    b8 unterminated;
};

static b32 is_space(char c) { return c == ' ' || c == '\t'; }

// splits in place: NULs over separators, escapes collapsed, the comment set aside
static void tokenize(char *line, Line *out) {
    *out = Line{};
    char *p = line;

    while (*p) {
        while (is_space(*p))
            p++;
        if (!*p)
            break;

        if (*p == '#') {
            p++;
            while (is_space(*p))
                p++;
            char *end = p + strlen(p);
            while (end > p && is_space(end[-1]))
                end--;
            *end = 0;
            out->comment = p;
            break;
        }

        char *tok;
        b8 q = false;
        if (*p == '"') {
            q = true;
            tok = ++p;
            char *w = p;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
                    p++;
                *w++ = *p++;
            }
            if (*p)
                p++;
            else
                out->unterminated = true;
            *w = 0;
        } else {
            tok = p;
            while (*p && !is_space(*p))
                p++;
            if (*p)
                *p++ = 0;
        }

        if (out->count == CONFIG_MAX_TOKENS) {
            out->overflow = true;
            return;
        }
        out->toks[out->count] = tok;
        out->quoted[out->count] = q;
        out->count++;
    }
}

static Node *add_node(memory::Arena *a, Node ***tail, Node *parent, u32 line) {
    Node *n = push_struct(a, Node);
    n->parent = parent;
    n->line = line;
    **tail = n;
    *tail = &n->next_sibling;
    return n;
}

DocResult parse(memory::Arena *a, char *text, const char *name) {
    ASSERT(a && text);

    Doc *doc = push_struct(a, Doc);
    doc->name = name ? name : "<memory>";

    Node *parent = NULL;
    Node **tail = &doc->first;
    Node **tail_stack[CONFIG_MAX_DEPTH];
    u32 depth = 0;

    // nonzero inside a rejected block, consumed to its matching '}'
    u32 skip_depth = 0;

    char *p = text;
    u32 line_no = 0;

    while (*p) {
        char *line = p;
        while (*p && *p != '\n')
            p++;

        char *line_end = p;
        if (*p)
            p++;
        if (line_end > line && line_end[-1] == '\r')
            line_end--;
        *line_end = 0;
        line_no++;

        Line ln;
        tokenize(line, &ln);
        if (ln.unterminated) {
            LOG_WARN("config: %s:%u has an unterminated quote", doc->name, line_no);
            doc->problems++;
        }
        if (ln.overflow) {
            LOG_WARN("config: %s:%u has more than %u tokens, the rest are dropped", doc->name,
                     line_no, (u32)CONFIG_MAX_TOKENS);
            doc->problems++;
        }

        u32 n = ln.count;
        b32 closes = n > 0 && !ln.quoted[0] && strcmp(ln.toks[0], "}") == 0;
        b32 opens = n > 0 && !ln.quoted[n - 1] && strcmp(ln.toks[n - 1], "{") == 0;

        if (skip_depth) {
            if (closes)
                skip_depth--;
            else if (opens)
                skip_depth++;
            continue;
        }

        if (n == 0) {
            Node *node = add_node(a, &tail, parent, line_no);
            node->comment = ln.comment;
            continue;
        }

        if (closes) {
            if (depth == 0) {
                LOG_WARN("config: %s:%u unmatched '}'", doc->name, line_no);
                doc->problems++;
                continue;
            }
            tail = tail_stack[--depth];
            parent = parent->parent;
            continue;
        }

        if (opens)
            n--;
        if (n == 0) {
            LOG_WARN("config: %s:%u block has no key, ignored", doc->name, line_no);
            doc->problems++;
            skip_depth = 1;
            continue;
        }

        Node *node = add_node(a, &tail, parent, line_no);
        node->key = ln.toks[0];
        node->comment = ln.comment;
        node->value_count = n - 1;
        if (node->value_count) {
            node->values = push_array(a, const char *, node->value_count);
            for (u32 i = 0; i < node->value_count; i++)
                node->values[i] = ln.toks[i + 1];
        }

        if (!opens)
            continue;

        if (depth == CONFIG_MAX_DEPTH) {
            LOG_WARN("config: %s:%u nested deeper than %d, the block's body is ignored",
                     doc->name, line_no, CONFIG_MAX_DEPTH);
            doc->problems++;
            skip_depth = 1;
            continue;
        }

        node->is_block = true;
        tail_stack[depth++] = tail;
        tail = &node->first_child;
        parent = node;
    }

    if (depth != 0) {
        LOG_WARN("config: %s ended with %u block(s) still open", doc->name, depth);
        doc->problems++;
    }

    return DocResult{.val = doc};
}

DocResult load(memory::Arena *a, const char *path) {
    ASSERT(a && path);

    file::BlobResult blob = file::read(a, path);
    if (blob.err != result::OK)
        return DocResult{.err = blob.err};

    return parse(a, (char *)blob.val.data, path);
}

Node *find(Node *first, const char *key) {
    for (Node *n = first; n; n = n->next_sibling)
        if (n->key && strcmp(n->key, key) == 0)
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

    // *end must be the terminator: "1.5x" is a typo, not 1.5
    char *end = NULL;
    f32 out = strtof(v, &end);
    if (end == v || *end != 0) {
        LOG_WARN("config: %u: '%s' is not a number", n->line, v);
        return fallback;
    }
    return out;
}

i32 value_i32(Node *n, u32 index, i32 fallback) {
    const char *v = value(n, index, NULL);
    if (!v)
        return fallback;

    // strtoll: long is 32-bit on Windows and 64-bit on Linux
    char *end = NULL;
    long long out = strtoll(v, &end, 10);
    if (end == v || *end != 0 || out < INT32_MIN || out > INT32_MAX) {
        LOG_WARN("config: %u: '%s' is not a 32-bit integer", n->line, v);
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

    LOG_WARN("config: %u: '%s' is not a boolean", n->line, v);
    return fallback;
}

const char *get(Node *block, const char *key, const char *fallback) {
    return value(find(block, key), 0, fallback);
}

f32 get_f32(Node *block, const char *key, f32 fallback) {
    return value_f32(find(block, key), 0, fallback);
}

i32 get_i32(Node *block, const char *key, i32 fallback) {
    return value_i32(find(block, key), 0, fallback);
}

b32 get_b32(Node *block, const char *key, b32 fallback) {
    return value_b32(find(block, key), 0, fallback);
}

linalg::Vec3 value_vec3(Node *n, linalg::Vec3 fallback) {
    return linalg::Vec3{
        value_f32(n, 0, fallback.x),
        value_f32(n, 1, fallback.y),
        value_f32(n, 2, fallback.z),
    };
}

linalg::Vec4 value_vec4(Node *n, linalg::Vec4 fallback) {
    return linalg::Vec4{
        value_f32(n, 0, fallback.x),
        value_f32(n, 1, fallback.y),
        value_f32(n, 2, fallback.z),
        value_f32(n, 3, fallback.w),
    };
}

Doc *create(memory::Arena *a, const char *name) {
    ASSERT(a);
    Doc *doc = push_struct(a, Doc);
    doc->name = name ? memory::push_str(a, name) : "<memory>";
    return doc;
}

void set_values(memory::Arena *a, Node *n, const char *const *values, u32 count) {
    ASSERT(a && n && (values || count == 0));

    n->value_count = count;
    n->values = count ? push_array(a, const char *, count) : NULL;
    for (u32 i = 0; i < count; i++)
        n->values[i] = memory::push_str(a, values[i]);
}

Node *append(memory::Arena *a, Doc *doc, Node *parent, const char *key,
             const char *const *values, u32 count) {
    ASSERT(a && doc && key);

    Node *n = push_struct(a, Node);
    n->key = memory::push_str(a, key);
    n->parent = parent;
    set_values(a, n, values, count);

    Node **tail = parent ? &parent->first_child : &doc->first;
    while (*tail)
        tail = &(*tail)->next_sibling;
    *tail = n;

    if (parent)
        parent->is_block = true;
    return n;
}

void unlink(Doc *doc, Node *n) {
    ASSERT(doc && n);

    Node **slot = n->parent ? &n->parent->first_child : &doc->first;
    while (*slot && *slot != n)
        slot = &(*slot)->next_sibling;
    if (*slot)
        *slot = n->next_sibling;

    n->parent = NULL;
    n->next_sibling = NULL;
}

// one contiguous arena run; nothing else may push between the first put and the NUL
struct Out {
    memory::Arena *a;
};

static void put(Out *o, const char *s, u64 n) {
    memcpy(memory::push_nz(o->a, n, 1), s, n);
}

static void put_str(Out *o, const char *s) { put(o, s, strlen(s)); }

static void put_indent(Out *o, u32 depth) {
    for (u32 i = 0; i < depth; i++)
        put_str(o, INDENT);
}

static b32 needs_quotes(const char *s) {
    if (!*s)
        return true;
    for (const char *c = s; *c; c++)
        if (is_space(*c) || *c == '#' || *c == '{' || *c == '}' || *c == '"' || *c == '\\')
            return true;
    return false;
}

static void put_token(Out *o, const char *s) {
    if (!needs_quotes(s)) {
        put_str(o, s);
        return;
    }

    put(o, "\"", 1);
    for (const char *c = s; *c; c++) {
        if (*c == '"' || *c == '\\')
            put(o, "\\", 1);
        put(o, c, 1);
    }
    put(o, "\"", 1);
}

static void write_nodes(Out *o, const Node *n, u32 depth) {
    for (; n; n = n->next_sibling) {
        if (!n->key) {
            if (n->comment) {
                put_indent(o, depth);
                put_str(o, "# ");
                put_str(o, n->comment);
            }
            put(o, "\n", 1);
            continue;
        }

        put_indent(o, depth);
        put_token(o, n->key);
        for (u32 i = 0; i < n->value_count; i++) {
            put(o, " ", 1);
            put_token(o, n->values[i]);
        }
        if (n->is_block)
            put_str(o, " {");
        if (n->comment) {
            put_str(o, " # ");
            put_str(o, n->comment);
        }
        put(o, "\n", 1);

        if (n->is_block) {
            write_nodes(o, n->first_child, depth + 1);
            put_indent(o, depth);
            put_str(o, "}\n");
        }
    }
}

char *write(memory::Arena *a, const Doc *doc) {
    ASSERT(a && doc);

    Out o{a};
    char *start = (char *)memory::push_nz(a, 0, 1);
    write_nodes(&o, doc->first, 0);
    put(&o, "", 1); // the NUL
    return start;
}

result::Error save(memory::Arena *scratch, const Doc *doc, const char *path) {
    ASSERT(scratch && doc && path);

    memory::ArenaTemp t = memory::temp_begin(scratch);
    char *text = write(scratch, doc);
    result::Error e = file::write(path, text, strlen(text));
    memory::temp_end(t);
    return e;
}

} // namespace config
