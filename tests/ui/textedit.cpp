#include "ui/textedit.h"

#include <string.h>

#include "suites.h"

#define CAP 16

static void insert_and_erase_by_codepoint(memory::Arena *) {
    char text[CAP] = "";
    textedit::Buffer b = textedit::create(text, CAP);

    textedit::insert(&b, "ab");
    textedit::insert(&b, "\xC3\xA9"); // é
    CHECK(strcmp(text, "ab\xC3\xA9") == 0);
    CHECK(b.len == 4 && b.cursor == 4);

    CHECK(textedit::erase_backward(&b));
    CHECK(strcmp(text, "ab") == 0);
    CHECK(b.cursor == 2);

    textedit::home(&b, false);
    CHECK(textedit::erase_forward(&b));
    CHECK(strcmp(text, "b") == 0);

    CHECK(!textedit::erase_backward(&b)); // nothing before the cursor
    textedit::end(&b, false);
    CHECK(!textedit::erase_forward(&b));
}

static void selection_is_replaced_whole(memory::Arena *) {
    char text[CAP] = "hello world";
    textedit::Buffer b = textedit::create(text, CAP);

    textedit::home(&b, false);
    textedit::move(&b, 1, true, true); // select "hello"
    CHECK(textedit::has_selection(&b));
    u32 lo, hi;
    textedit::selection(&b, &lo, &hi);
    CHECK(lo == 0 && hi == 5);

    textedit::insert(&b, "bye");
    CHECK(strcmp(text, "bye world") == 0);
    CHECK(!textedit::has_selection(&b) && b.cursor == 3);

    textedit::select_all(&b);
    CHECK(textedit::erase_backward(&b));
    CHECK(text[0] == 0 && b.len == 0);
}

static void movement_by_word_and_collapse(memory::Arena *) {
    char text[CAP] = "one two";
    textedit::Buffer b = textedit::create(text, CAP);

    textedit::move(&b, -1, true, false);
    CHECK(b.cursor == 4);
    textedit::move(&b, -1, true, false);
    CHECK(b.cursor == 0);
    textedit::move(&b, 1, true, false);
    CHECK(b.cursor == 3);

    // a selection collapses toward the side moved to
    textedit::home(&b, false);
    textedit::move(&b, 1, true, true);
    textedit::move(&b, -1, false, false);
    CHECK(b.cursor == 0 && !textedit::has_selection(&b));
    textedit::move(&b, 1, true, true);
    textedit::move(&b, 1, false, false);
    CHECK(b.cursor == 3 && !textedit::has_selection(&b));

    // by codepoint steps over a whole multibyte sequence
    textedit::set(&b, "\xE2\x82\xAC" "x");
    textedit::home(&b, false);
    textedit::move(&b, 1, false, false);
    CHECK(b.cursor == 3);
}

static void cap_drops_whole_codepoints(memory::Arena *) {
    char text[6] = "";
    textedit::Buffer b = textedit::create(text, 6);

    textedit::insert(&b, "abcd\xC3\xA9"); // 6 bytes; only 5 fit, and é is 2
    CHECK(strcmp(text, "abcd") == 0);
    CHECK(b.len == 4);

    textedit::set(&b, "\xC3\xA9\xC3\xA9\xC3\xA9");
    CHECK(b.len == 4); // two of the three
}

test::Suite textedit_suite() {
    static const test::Case cases[] = {
        CASE(insert_and_erase_by_codepoint),
        CASE(selection_is_replaced_whole),
        CASE(movement_by_word_and_collapse),
        CASE(cap_drops_whole_codepoints),
    };
    return test::Suite{.name = "textedit", .cases = cases, .count = ARRAY_COUNT(cases)};
}
