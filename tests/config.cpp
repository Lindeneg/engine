#include <string.h>

#include "config.h"
#include "suites.h"

// parse tokenizes in place, so the text has to be writable and outlive the Doc.
static char *text(memory::Arena *a, const char *src) { return memory::push_str(a, src); }

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
    CHECK(d->problems == 0);
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
    CHECK(sh->is_block);
    CHECK(strcmp(config::value(sh, 0, "?"), "mesh") == 0);
    CHECK(strcmp(config::get(sh->first_child, "vert", "?"), "mesh.vert") == 0);

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

static void escapes_inside_quotes(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "say \"he said \\\"hi\\\" \\\\ done\"\n"), "test"));

    CHECK(strcmp(config::get(d->first, "say", "?"), "he said \"hi\" \\ done") == 0);
    CHECK(d->problems == 0);
}

static void comments_and_blank_lines_are_kept_but_not_keys(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "# leading comment\n"
                                                  "\n"
                                                  "keep 1 # trailing comment\n"
                                                  "#keep 2\n"),
                                        "test"));

    config::Node *k = config::find(d->first, "keep");
    CHECK(k != NULL);
    CHECK(k->value_count == 1); // the comment is not a second value
    CHECK(strcmp(k->comment, "trailing comment") == 0);
    CHECK(config::find_next(k, "keep") == NULL);

    // in order: comment, blank, keep, comment
    config::Node *n = d->first;
    CHECK(n->key == NULL && strcmp(n->comment, "leading comment") == 0);
    n = n->next_sibling;
    CHECK(n->key == NULL && n->comment == NULL);
    n = n->next_sibling;
    CHECK(n == k);
    n = n->next_sibling;
    CHECK(n->key == NULL && strcmp(n->comment, "keep 2") == 0);
    CHECK(n->next_sibling == NULL);
}

// the editor will write these files on Windows
static void crlf_is_handled(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "vert mesh.vert\r\nfrag mesh.frag\r\n"), "test"));

    CHECK(strcmp(config::get(d->first, "vert", "?"), "mesh.vert") == 0);
    CHECK(strcmp(config::get(d->first, "frag", "?"), "mesh.frag") == 0);
}

static void typed_reads_fall_back(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "scale 0.857\n"
                                                  "z_up yes\n"
                                                  "flat off\n"
                                                  "junk hello\n"),
                                        "test"));

    CHECK(config::get_f32(d->first, "scale", 0.0f) == 0.857f);
    CHECK(config::get_b32(d->first, "z_up", false) == true);
    CHECK(config::get_b32(d->first, "flat", true) == false);

    test::mute();
    CHECK(config::get_f32(d->first, "junk", 2.5f) == 2.5f);
    CHECK(test::unmute() == 1); // the bad conversion warns, by design

    // absent key, and present key with an out-of-range index
    CHECK(config::get_i32(d->first, "nope", 42) == 42);
    CHECK(config::value_i32(config::find(d->first, "scale"), 9, 42) == 42);
    CHECK(strcmp(config::value(NULL, 0, "fallback"), "fallback") == 0);
}

// a file a newer editor wrote must still parse: unknown keys are skipped, not rejected
static void unknown_keys_are_ignored(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "shader mesh {\n"
                                                  "    vert mesh.vert\n"
                                                  "    tessellation_domain quads\n"
                                                  "}\n"),
                                        "test"));

    config::Node *sh = config::find(d->first, "shader");
    CHECK(config::find(sh->first_child, "vert") != NULL);
    CHECK(config::find(sh->first_child, "tessellation_domain") != NULL);
    CHECK(d->problems == 0);
}

static void malformed_input_survives_and_is_counted(memory::Arena *a) {
    test::mute();

    config::Doc *stray = MUST(config::parse(a, text(a, "a 1\n}\nb 2\n"), "test"));
    CHECK(config::find(stray->first, "a") != NULL);
    CHECK(config::find(stray->first, "b") != NULL);
    CHECK(stray->problems == 1);

    config::Doc *unclosed = MUST(config::parse(a, text(a, "outer {\n    inner 1\n"), "test"));
    config::Node *outer = config::find(unclosed->first, "outer");
    CHECK(config::find(outer->first_child, "inner") != NULL);
    CHECK(unclosed->problems == 1);

    config::Doc *headless = MUST(config::parse(a, text(a, "{\n}\n"), "test"));
    CHECK(headless->first == NULL);
    CHECK(headless->problems == 1);

    config::Doc *open_quote = MUST(config::parse(a, text(a, "k \"never closed\n"), "test"));
    CHECK(strcmp(config::get(open_quote->first, "k", "?"), "never closed") == 0);
    CHECK(open_quote->problems == 1);

    CHECK(test::unmute() == 4);
}

// a rejected block's body is skipped, not hoisted, or its '}' would close the parent
static void rejected_blocks_are_skipped_whole(memory::Arena *a) {
    test::mute();
    config::Doc *d = MUST(config::parse(a, text(a, "outer {\n"
                                                  "    {\n"
                                                  "        stray 1\n"
                                                  "    }\n"
                                                  "    inner 2\n"
                                                  "}\n"
                                                  "after 3\n"),
                                        "test"));
    CHECK(test::unmute() == 1);

    config::Node *outer = config::find(d->first, "outer");
    CHECK(config::find(outer->first_child, "stray") == NULL);
    CHECK(config::find(outer->first_child, "inner") != NULL);
    CHECK(config::find(d->first, "after") != NULL);
    CHECK(config::find(d->first, "inner") == NULL);
}

static void quoted_braces_are_values(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "open \"{\"\n"
                                                  "close \"}\"\n"
                                                  "next 1\n"),
                                        "test"));

    CHECK(strcmp(config::get(d->first, "open", "?"), "{") == 0);
    CHECK(strcmp(config::get(d->first, "close", "?"), "}") == 0);
    CHECK(!config::find(d->first, "open")->is_block);
    CHECK(config::find(d->first, "next") != NULL);
}

static void numbers_reject_trailing_junk(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "f 1.5x\n"
                                                  "i 12abc\n"
                                                  "big 99999999999\n"),
                                        "test"));

    test::mute();
    CHECK(config::get_f32(d->first, "f", -1.0f) == -1.0f);
    CHECK(config::get_i32(d->first, "i", -1) == -1);
    CHECK(config::get_i32(d->first, "big", -1) == -1);
    CHECK(test::unmute() == 3);
}

static void vectors_fall_back_per_component(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "color 1 0.5\n"), "test"));

    linalg::Vec3 c = config::value_vec3(config::find(d->first, "color"), linalg::Vec3{9, 9, 9});
    CHECK(c.x == 1.0f && c.y == 0.5f && c.z == 9.0f);

    linalg::Vec3 missing = config::value_vec3(NULL, linalg::Vec3{1, 2, 3});
    CHECK(missing.x == 1.0f && missing.y == 2.0f && missing.z == 3.0f);
}

static void nodes_know_their_line_and_parent(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "\n"
                                                  "outer {\n"
                                                  "    inner {\n"
                                                  "        leaf 1\n"
                                                  "    }\n"
                                                  "}\n"),
                                        "test"));

    config::Node *outer = config::find(d->first, "outer");
    config::Node *inner = config::find(outer->first_child, "inner");
    config::Node *leaf = config::find(inner->first_child, "leaf");

    CHECK(outer->line == 2 && inner->line == 3 && leaf->line == 4);
    CHECK(outer->parent == NULL);
    CHECK(inner->parent == outer);
    CHECK(leaf->parent == inner);
}

// everything a person wrote comes back out in canonical form, and a second pass changes nothing
static void round_trip_is_stable(memory::Arena *a) {
    const char *src = "# top comment\n"
                      "\n"
                      "shader   \"mesh\" {\n"
                      "    vert mesh.vert   # per line\n"
                      "    features SKINNED ALPHA_MASK\n"
                      "    empty {\n"
                      "    }\n"
                      "}\n"
                      "title \"My Game Window\" 720\n"
                      "weird \"{\" \"a b\" \"\" \"q\\\"q\" \"back\\\\slash\"\n";

    const char *canonical = "# top comment\n"
                            "\n"
                            "shader mesh {\n"
                            "    vert mesh.vert # per line\n"
                            "    features SKINNED ALPHA_MASK\n"
                            "    empty {\n"
                            "    }\n"
                            "}\n"
                            "title \"My Game Window\" 720\n"
                            "weird \"{\" \"a b\" \"\" \"q\\\"q\" \"back\\\\slash\"\n";

    config::Doc *d1 = MUST(config::parse(a, text(a, src), "test"));
    char *w1 = config::write(a, d1);
    CHECK(strcmp(w1, canonical) == 0);

    config::Doc *d2 = MUST(config::parse(a, text(a, w1), "test"));
    char *w2 = config::write(a, d2);
    CHECK(strcmp(w1, w2) == 0);
    CHECK(d2->problems == 0);

    config::Node *weird = config::find(d2->first, "weird");
    CHECK(weird->value_count == 5);
    CHECK(strcmp(weird->values[3], "q\"q") == 0);
    CHECK(strcmp(weird->values[4], "back\\slash") == 0);
}

static void edits_append_and_unlink(memory::Arena *a) {
    config::Doc *d = MUST(config::parse(a, text(a, "keep 1\n"
                                                  "drop 2\n"),
                                        "test"));

    const char *vals[] = {"has space", "plain"};
    config::Node *mat = config::append(a, d, NULL, "material", vals, 2);
    const char *one[] = {"0.5"};
    config::append(a, d, mat, "roughness", one, 1);

    config::unlink(d, config::find(d->first, "drop"));

    CHECK(strcmp(config::write(a, d), "keep 1\n"
                                      "material \"has space\" plain {\n"
                                      "    roughness 0.5\n"
                                      "}\n") == 0);
    CHECK(mat->is_block);
    CHECK(config::find(mat->first_child, "roughness")->parent == mat);
    CHECK(config::find(d->first, "drop") == NULL);
}

test::Suite config_suite() {
    static const test::Case cases[] = {
        CASE(flat_keys_and_values),
        CASE(blocks_nest),
        CASE(repeated_keys_are_walkable),
        CASE(quotes_keep_spaces),
        CASE(escapes_inside_quotes),
        CASE(comments_and_blank_lines_are_kept_but_not_keys),
        CASE(crlf_is_handled),
        CASE(typed_reads_fall_back),
        CASE(unknown_keys_are_ignored),
        CASE(malformed_input_survives_and_is_counted),
        CASE(rejected_blocks_are_skipped_whole),
        CASE(quoted_braces_are_values),
        CASE(numbers_reject_trailing_junk),
        CASE(vectors_fall_back_per_component),
        CASE(nodes_know_their_line_and_parent),
        CASE(round_trip_is_stable),
        CASE(edits_append_and_unlink),
    };
    return test::Suite{.name = "config", .cases = cases, .count = ARRAY_COUNT(cases)};
}
