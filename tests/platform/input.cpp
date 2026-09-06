#include "platform/input.h"

#include <string.h>

#include "platform/path.h"
#include "project/project.h"
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

// the engine's maps at init, a project's added after; both resolve by name
static void maps_are_added_after_init(memory::Arena *a) {
    memory::Arena state = memory::create(KiB(64));
    input::State *s =
        MUST(input::init(&state, a, FPATH(a, CL_ENGINE_ROOT, "input.cfg"), 8, 128));
    CHECK(input::find(s, "quit").v != 0);

    test::mute();
    CHECK(input::find(s, "move_forward").v == 0);
    CHECK(test::unmute() == 1);

    CHECK(input::add_maps(s, &state, a,
                          FPATH(a, CL_ENGINE_ROOT, PROJECT_TEMPLATE_DIR, "input.cfg")) ==
          result::OK);
    CHECK(input::find(s, "move_forward").v != 0);
    CHECK(input::find_map(s, "gameplay").v != 0);
    CHECK(input::map_count(s) == 3);

    // the file again: every map is a duplicate, so nothing is added and it says so
    test::mute();
    CHECK(input::add_maps(s, &state, a,
                          FPATH(a, CL_ENGINE_ROOT, PROJECT_TEMPLATE_DIR, "input.cfg")) ==
          result::ERR_PARSE);
    CHECK(test::unmute() >= 1);

    // rewinding to before the project's maps drops them, names and all, so
    // another project's file can declare the same names
    input::State *again = MUST(input::init(&state, a, FPATH(a, CL_ENGINE_ROOT, "input.cfg"), 8, 128));
    input::Mark before = input::mark(again);
    CHECK(input::add_maps(again, &state, a,
                          FPATH(a, CL_ENGINE_ROOT, PROJECT_TEMPLATE_DIR, "input.cfg")) ==
          result::OK);
    input::Action fire = input::find(again, "fire");
    CHECK(fire.v != 0);
    input::rewind(again, before);
    CHECK(input::map_count(again) == before.maps);
    CHECK(!input::down(again, fire));
    test::mute();
    CHECK(input::find(again, "fire").v == 0);
    CHECK(test::unmute() == 1);
    CHECK(input::add_maps(again, &state, a,
                          FPATH(a, CL_ENGINE_ROOT, PROJECT_TEMPLATE_DIR, "input.cfg")) ==
          result::OK);
    CHECK(input::find(again, "fire").v != 0);
    CHECK(input::find(again, "quit").v != 0);

    // and a state with room for one map keeps only the first
    memory::Arena small = memory::create(KiB(64));
    test::mute();
    input::State *one = MUST(input::init(&small, a, FPATH(a, CL_ENGINE_ROOT, "input.cfg"), 1, 128));
    CHECK(test::unmute() == 1);
    CHECK(input::map_count(one) == 1);
}

test::Suite input_suite() {
    static const test::Case cases[] = {
        CASE(plain_tokens_parse),
        CASE(chords_name_their_modifiers),
        CASE(text_round_trips),
        CASE(maps_are_added_after_init),
    };
    return test::Suite{.name = "input", .cases = cases, .count = ARRAY_COUNT(cases)};
}
