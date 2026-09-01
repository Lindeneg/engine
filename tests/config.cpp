#include <string.h>

#include "config.h"
#include "logger.h"
#include "suites.h"

// parse tokenizes in place, so the text has to be writable and outlive the Doc.
static char *text(memory::Arena *a, const char *src) {
    u64 n = strlen(src);
    char *out = push_array_nz(a, char, n + 1);
    memcpy(out, src, n + 1);
    return out;
}

static void flat_keys_and_values(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "name mesh\n"
                                                  "size 4 8 15\n"),
                                        "test"));

    config::Node *name = config::find(d->first, "name");
    CHECK(name != NULL);
    CHECK(name->value_count == 1);
    CHECK(strcmp(config::value(name, 0, "?"), "mesh") == 0);

    config::Node *size = config::find(d->first, "size");
    CHECK(size->value_count == 3);
    CHECK(config::value_i32(size, 2, -1) == 15);
}

static void blocks_nest(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "shader \"mesh\" {\n"
                                                  "    vert mesh.vert\n"
                                                  "    frag mesh.frag\n"
                                                  "}\n"
                                                  "after 1\n"),
                                        "test"));

    config::Node *sh = config::find(d->first, "shader");
    CHECK(sh != NULL);
    CHECK(strcmp(config::value(sh, 0, "?"), "mesh") == 0);
    CHECK(strcmp(config::value(config::find(sh->first_child, "vert"), 0, "?"), "mesh.vert") == 0);

    // the block must close back to the top level, not swallow what follows
    CHECK(config::find(d->first, "after") != NULL);
    CHECK(config::find(sh->first_child, "after") == NULL);
}

// the manifest declares one block per shader, so scanning repeats matters
static void repeated_keys_are_walkable(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "shader a {\n"
                                                  "    x 1\n"
                                                  "}\n"
                                                  "shader b {\n"
                                                  "}\n"
                                                  "shader c {\n"
                                                  "}\n"),
                                        "test"));

    u32 n = 0;
    const char *last = NULL;
    for (config::Node *s = config::find(d->first, "shader"); s;
         s = config::find_next(s, "shader")) {
        last = config::value(s, 0, "?");
        n++;
    }

    CHECK(n == 3);
    CHECK(strcmp(last, "c") == 0);
}

static void quotes_keep_spaces(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "title \"My Game Window\" 720\n"), "test"));

    config::Node *t = config::find(d->first, "title");
    CHECK(t->value_count == 2);
    CHECK(strcmp(config::value(t, 0, "?"), "My Game Window") == 0);
    CHECK(config::value_i32(t, 1, 0) == 720);
}

static void comments_and_blank_lines_vanish(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "# leading comment\n"
                                                  "\n"
                                                  "   \t \n"
                                                  "keep 1 # trailing comment\n"
                                                  "#keep 2\n"),
                                        "test"));

    config::Node *k = config::find(d->first, "keep");
    CHECK(k != NULL);
    CHECK(k->value_count == 1); // the comment is not a second value
    CHECK(config::value_i32(k, 0, -1) == 1);
    CHECK(config::find_next(k, "keep") == NULL);
}

// the editor will write these files on Windows
static void crlf_is_handled(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "vert mesh.vert\r\nfrag mesh.frag\r\n"), "test"));

    CHECK(strcmp(config::value(config::find(d->first, "vert"), 0, "?"), "mesh.vert") == 0);
    CHECK(strcmp(config::value(config::find(d->first, "frag"), 0, "?"), "mesh.frag") == 0);
}

static void typed_reads_fall_back(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "scale 0.857\n"
                                                  "z_up yes\n"
                                                  "flat off\n"
                                                  "junk hello\n"),
                                        "test"));

    CHECK(config::value_f32(config::find(d->first, "scale"), 0, 0.0f) == 0.857f);
    CHECK(config::value_b32(config::find(d->first, "z_up"), 0, false) == true);
    CHECK(config::value_b32(config::find(d->first, "flat"), 0, true) == false);

    logger::set_level(logger::FATAL); // the bad conversions warn, by design
    CHECK(config::value_f32(config::find(d->first, "junk"), 0, 2.5f) == 2.5f);
    logger::set_level(logger::TRACE);

    // absent key, and present key with an out-of-range index
    CHECK(config::value_i32(config::find(d->first, "nope"), 0, 42) == 42);
    CHECK(config::value_i32(config::find(d->first, "scale"), 9, 42) == 42);
    CHECK(strcmp(config::value(NULL, 0, "fallback"), "fallback") == 0);
}

// a file a newer editor wrote must still parse: unknown keys are skipped, not
// rejected, or an engine update becomes a hard dependency on a file update
static void unknown_keys_are_ignored(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "shader mesh {\n"
                                                  "    vert mesh.vert\n"
                                                  "    tessellation_domain quads\n"
                                                  "}\n"),
                                        "test"));

    config::Node *sh = config::find(d->first, "shader");
    CHECK(config::find(sh->first_child, "vert") != NULL);
    CHECK(config::find(sh->first_child, "tessellation_domain") != NULL);
}

static void malformed_input_survives(memory::Arena *a) {
    logger::set_level(logger::FATAL); // all three of these warn

    config::Doc *stray = MUST(config::parse(a, text(a, "a 1\n}\nb 2\n"), "test"));
    CHECK(config::find(stray->first, "a") != NULL);
    CHECK(config::find(stray->first, "b") != NULL);

    config::Doc *unclosed = MUST(config::parse(a, text(a, "outer {\n    inner 1\n"), "test"));
    config::Node *outer = config::find(unclosed->first, "outer");
    CHECK(config::find(outer->first_child, "inner") != NULL);

    config::Doc *headless = MUST(config::parse(a, text(a, "{\n}\n"), "test"));
    CHECK(headless->first == NULL);

    logger::set_level(logger::TRACE);
}

test::Suite config_suite() {
    static const test::Case cases[] = {
        {"flat_keys_and_values", flat_keys_and_values},
        {"blocks_nest", blocks_nest},
        {"repeated_keys_are_walkable", repeated_keys_are_walkable},
        {"quotes_keep_spaces", quotes_keep_spaces},
        {"comments_and_blank_lines_vanish", comments_and_blank_lines_vanish},
        {"crlf_is_handled", crlf_is_handled},
        {"typed_reads_fall_back", typed_reads_fall_back},
        {"unknown_keys_are_ignored", unknown_keys_are_ignored},
        {"malformed_input_survives", malformed_input_survives},
    };
    return test::Suite{.name = "config", .cases = cases, .count = ARRAY_COUNT(cases)};
}
