#include "platform/input.h"

#include <string.h>

#include "suites.h"

static void plain_tokens_parse(memory::Arena *) {
    input::Binding b;

    CHECK(input::parse_binding("z", &b));
    CHECK(!b.is_button && b.code == window::KEY_Z && b.modifiers == 0);

    CHECK(input::parse_binding("mouse_left", &b));
    CHECK(b.is_button && b.code == window::BUTTON_LEFT && b.modifiers == 0);

    CHECK(!input::parse_binding("mouse_wheel", &b));
    CHECK(!input::parse_binding("", &b));
}

static void chords_name_their_modifiers(memory::Arena *) {
    input::Binding b;

    CHECK(input::parse_binding("control+z", &b));
    CHECK(!b.is_button && b.code == window::KEY_Z && b.modifiers == INPUT_MODIFIER_CONTROL);

    CHECK(input::parse_binding("control+shift+mouse_left", &b));
    CHECK(b.is_button && b.code == window::BUTTON_LEFT);
    CHECK(b.modifiers == (INPUT_MODIFIER_CONTROL | INPUT_MODIFIER_SHIFT));

    // a modifier is only ever a prefix, and the last part must be a key
    CHECK(!input::parse_binding("z+control", &b));
    CHECK(!input::parse_binding("control+", &b));
    CHECK(!input::parse_binding("ctrl+z", &b));
    CHECK(!input::parse_binding("control", &b));
}

static void text_round_trips(memory::Arena *) {
    const char *tokens[] = {"z", "mouse_left", "control+z", "control+shift+mouse_left",
                            "alt+f4"};
    for (u32 i = 0; i < ARRAY_COUNT(tokens); i++) {
        input::Binding b;
        CHECK(input::parse_binding(tokens[i], &b));

        char text[INPUT_BINDING_TEXT_CAP];
        input::binding_text(b, text, sizeof(text));
        CHECK(strcmp(text, tokens[i]) == 0);
    }
}

test::Suite input_suite() {
    static const test::Case cases[] = {
        CASE(plain_tokens_parse),
        CASE(chords_name_their_modifiers),
        CASE(text_round_trips),
    };
    return test::Suite{.name = "input", .cases = cases, .count = ARRAY_COUNT(cases)};
}
