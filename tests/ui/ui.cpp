#include "ui/ui.h"

#include <math.h>
#include <string.h>

#include "suites.h"

#define FB_W 800
#define FB_H 600

static const canvas::Rect PANEL = {0.0f, 0.0f, 400.0f, 300.0f};

static font::Font *shared_font(memory::Arena *scratch) {
    static font::Font *f = NULL;
    if (f)
        return f;

    static memory::Arena arena;
    arena = memory::create(MiB(1));
    font::Config cfg = font::config_from(NULL);
    cfg.file = "fonts/arial.ttf";
    cfg.atlas = 256;
    cfg.oversample = 1;
    f = MUST(font::load(&arena, scratch, CL_ASSET_ROOT, cfg));
    return f;
}

static void setup(memory::Arena *a) {
    canvas::init(a, canvas::config_from(NULL));
    ui::init(a, ui::config_from(NULL), ui::style_from(NULL), shared_font(a));
}

static void teardown() {
    ui::shutdown();
    canvas::shutdown();
}

// the centre of row i's first column, from the style
static linalg::Vec2 row(canvas::Rect panel, u32 i, f32 x_offset) {
    const ui::Style *s = ui::style();
    f32 y = panel.y + s->title_height + s->padding + (f32)i * (s->row_height + s->spacing) +
            s->row_height * 0.5f;
    return linalg::Vec2{panel.x + s->padding + x_offset, y};
}

static ui::Input at(linalg::Vec2 p) {
    ui::Input in{};
    in.mouse = p;
    return in;
}

static ui::Input press(linalg::Vec2 p) {
    ui::Input in = at(p);
    in.mouse_down = true;
    in.mouse_pressed = true;
    return in;
}

static ui::Input hold(linalg::Vec2 p) {
    ui::Input in = at(p);
    in.mouse_down = true;
    return in;
}

static ui::Input release(linalg::Vec2 p) {
    ui::Input in = at(p);
    in.mouse_released = true;
    return in;
}

static void begin(ui::Input in) {
    canvas::begin_frame(FB_W, FB_H);
    ui::begin_frame(&in);
    ui::begin_panel("Test", PANEL);
}

static void finish() {
    ui::end_panel();
    ui::end_frame();
}

static void button_clicks_on_release(memory::Arena *a) {
    setup(a);
    linalg::Vec2 p = row(PANEL, 0, 50.0f);

    // hover, press, release: the click lands on the release, once
    begin(at(p));
    CHECK(!ui::button("Go"));
    finish();

    begin(press(p));
    CHECK(!ui::button("Go"));
    CHECK(ui::wants_mouse());
    finish();

    begin(release(p));
    CHECK(ui::button("Go"));
    finish();

    begin(at(p));
    CHECK(!ui::button("Go"));
    finish();

    // a release off the button is not a click
    begin(press(p));
    ui::button("Go");
    finish();
    begin(release(linalg::Vec2{-10.0f, -10.0f}));
    CHECK(!ui::button("Go"));
    finish();

    teardown();
}

static void checkbox_toggles(memory::Arena *a) {
    setup(a);
    linalg::Vec2 p = row(PANEL, 0, 5.0f);
    b32 value = false;

    begin(at(p));
    ui::checkbox("Flag", &value);
    finish();
    begin(press(p));
    ui::checkbox("Flag", &value);
    finish();
    begin(release(p));
    CHECK(ui::checkbox("Flag", &value));
    finish();
    CHECK(value);

    teardown();
}

static void slider_drags_to_the_pointer(memory::Arena *a) {
    setup(a);
    const ui::Style *s = ui::style();
    f32 row_w = PANEL.w - 2.0f * s->padding - s->scrollbar_width;
    linalg::Vec2 mid = row(PANEL, 0, row_w * 0.5f);
    f32 value = 0.0f;

    begin(at(mid));
    ui::slider("v", &value, 0.0f, 10.0f);
    finish();
    begin(press(mid));
    CHECK(ui::slider("v", &value, 0.0f, 10.0f));
    finish();
    CHECK(fabsf(value - 5.0f) < 0.05f);

    // still held, the pointer keeps driving it past the row's edge, clamped
    begin(hold(linalg::Vec2{PANEL.w + 100.0f, mid.y}));
    ui::slider("v", &value, 0.0f, 10.0f);
    finish();
    CHECK(value == 10.0f);

    teardown();
}

static void text_field_takes_typed_text(memory::Arena *a) {
    setup(a);
    linalg::Vec2 p = row(PANEL, 0, 20.0f);
    char buf[16] = "";

    begin(at(p));
    ui::text_field("name", buf, sizeof(buf));
    finish();
    begin(press(p));
    ui::text_field("name", buf, sizeof(buf));
    finish();
    CHECK(ui::wants_keyboard());

    ui::Input typing = at(p);
    typing.text = "ab";
    begin(typing);
    CHECK(ui::text_field("name", buf, sizeof(buf)));
    finish();
    CHECK(strcmp(buf, "ab") == 0);

    ui::Input backspace = at(p);
    backspace.key_pressed[window::KEY_BACKSPACE] = 1;
    begin(backspace);
    CHECK(ui::text_field("name", buf, sizeof(buf)));
    finish();
    CHECK(strcmp(buf, "a") == 0);

    ui::Input enter = at(p);
    enter.key_pressed[window::KEY_ENTER] = 1;
    begin(enter);
    ui::text_field("name", buf, sizeof(buf));
    finish();
    CHECK(!ui::wants_keyboard());

    teardown();
}

static void float_field_commits_and_reverts(memory::Arena *a) {
    setup(a);
    linalg::Vec2 p = row(PANEL, 0, 20.0f);
    f32 value = 1.0f;

    // focusing selects the whole number, so typing replaces it
    begin(at(p));
    ui::float_field("x", &value);
    finish();
    begin(press(p));
    ui::float_field("x", &value);
    finish();

    ui::Input typing = at(p);
    typing.text = "2.5";
    begin(typing);
    ui::float_field("x", &value);
    finish();
    CHECK(value == 1.0f); // not yet

    ui::Input enter = at(p);
    enter.key_pressed[window::KEY_ENTER] = 1;
    begin(enter);
    CHECK(ui::float_field("x", &value));
    finish();
    CHECK(value == 2.5f);
    CHECK(!ui::wants_keyboard());

    // escape throws the edit away
    begin(at(p));
    ui::float_field("x", &value);
    finish();
    begin(press(p));
    ui::float_field("x", &value);
    finish();
    typing.text = "9";
    begin(typing);
    ui::float_field("x", &value);
    finish();
    ui::Input escape = at(p);
    escape.key_pressed[window::KEY_ESCAPE] = 1;
    begin(escape);
    CHECK(!ui::float_field("x", &value));
    finish();
    CHECK(value == 2.5f);

    teardown();
}

static void ids_separate_same_labels(memory::Arena *a) {
    setup(a);
    linalg::Vec2 second = row(PANEL, 1, 50.0f);

    b32 first_hit = false;
    b32 second_hit = false;
    ui::Input frames[] = {at(second), press(second), release(second)};
    for (u32 i = 0; i < ARRAY_COUNT(frames); i++) {
        begin(frames[i]);
        ui::push_id(1u);
        first_hit = ui::button("Delete") || first_hit;
        ui::pop_id();
        ui::push_id(2u);
        second_hit = ui::button("Delete") || second_hit;
        ui::pop_id();
        finish();
    }
    CHECK(!first_hit);
    CHECK(second_hit);

    teardown();
}

static void tree_node_remembers_being_opened(memory::Arena *a) {
    setup(a);
    linalg::Vec2 header = row(PANEL, 0, 30.0f);

    begin(at(header));
    CHECK(!ui::tree_node("Details", false));
    finish();

    begin(press(header));
    ui::tree_node("Details", false);
    finish();
    begin(release(header));
    if (ui::tree_node("Details", false))
        ui::tree_pop();
    finish();

    begin(at(linalg::Vec2{-1.0f, -1.0f}));
    b32 open = ui::tree_node("Details", false);
    if (open)
        ui::tree_pop();
    CHECK(open);

    // the default only applies the first time a node is seen
    b32 fresh = ui::tree_node("Other", true);
    if (fresh)
        ui::tree_pop();
    CHECK(fresh);
    finish();

    teardown();
}

static void panel_scrolls_with_the_wheel(memory::Arena *a) {
    setup(a);
    const ui::Style *s = ui::style();
    canvas::Rect small{0.0f, 0.0f, 300.0f, 120.0f};
    linalg::Vec2 inside = row(small, 0, 50.0f);

    // one tick down moves every row up by a step; the button at row 3 is then
    // where row 3 minus a step used to be. The first frame only measures the
    // panel, since a tick is clamped against the extent of the frame before
    ui::Input wheel = at(inside);
    wheel.scroll = -1.0f;
    linalg::Vec2 target = row(small, 3, 50.0f);
    target.y -= s->scroll_step;

    ui::Input frames[] = {at(inside), wheel, at(target), press(target), release(target)};
    b32 hit = false;
    for (u32 i = 0; i < ARRAY_COUNT(frames); i++) {
        canvas::begin_frame(FB_W, FB_H);
        ui::begin_frame(&frames[i]);
        ui::begin_panel("Long", small);
        for (u32 r = 0; r < 3; r++)
            ui::label("row");
        hit = ui::button("Deep") || hit;
        for (u32 r = 0; r < 30; r++)
            ui::label("filler");
        ui::end_panel();
        ui::end_frame();
    }
    CHECK(hit);
    CHECK(ui::wants_mouse());

    teardown();
}

static void later_panel_wins_the_pointer(memory::Arena *a) {
    setup(a);
    canvas::Rect r{0.0f, 0.0f, 200.0f, 100.0f};
    linalg::Vec2 p = row(r, 0, 40.0f);

    b32 under = false;
    b32 over = false;
    ui::Input frames[] = {at(p), press(p), release(p)};
    for (u32 i = 0; i < ARRAY_COUNT(frames); i++) {
        canvas::begin_frame(FB_W, FB_H);
        ui::begin_frame(&frames[i]);
        ui::begin_panel("Under", r);
        under = ui::button("Hit") || under;
        ui::end_panel();
        ui::begin_panel("Over", r);
        over = ui::button("Hit") || over;
        ui::end_panel();
        ui::end_frame();
    }
    CHECK(!under);
    CHECK(over);

    teardown();
}

static void int_and_enum_fields(memory::Arena *a) {
    setup(a);
    linalg::Vec2 p = row(PANEL, 0, 20.0f);
    i32 value = 3;

    begin(at(p));
    ui::int_field("n", &value);
    finish();
    begin(press(p));
    ui::int_field("n", &value);
    finish();

    ui::Input typing = at(p);
    typing.text = "42";
    begin(typing);
    ui::int_field("n", &value);
    finish();

    ui::Input enter = at(p);
    enter.key_pressed[window::KEY_ENTER] = 1;
    begin(enter);
    CHECK(ui::int_field("n", &value));
    finish();
    CHECK(value == 42);

    // the enum steps around its names on each click
    static const char *names[] = {"low", "high"};
    i32 index = 1;
    ui::Input frames[] = {at(p), press(p), release(p)};
    b32 changed = false;
    for (u32 i = 0; i < ARRAY_COUNT(frames); i++) {
        begin(frames[i]);
        changed = ui::enum_field("mode", names, ARRAY_COUNT(names), &index) || changed;
        finish();
    }
    CHECK(changed);
    CHECK(index == 0);

    teardown();
}

static void style_reads_the_file_shape(memory::Arena *a) {
    config::Doc *doc = MUST(config::parse(
        a, memory::push_str(a, "row_height 30\naccent 1 0 0\n"), "ui.cfg"));

    ui::Style s = ui::style_from(doc->first);
    CHECK(s.row_height == 30.0f);
    CHECK(s.accent.x == 1.0f && s.accent.y == 0.0f && s.accent.w == 1.0f);
    CHECK(s.padding > 0.0f); // a default stood in

    CHECK(ui::load_style(a, CL_ASSET_ROOT).err == result::OK);
}

test::Suite ui_suite() {
    static const test::Case cases[] = {
        CASE(button_clicks_on_release),
        CASE(checkbox_toggles),
        CASE(slider_drags_to_the_pointer),
        CASE(text_field_takes_typed_text),
        CASE(float_field_commits_and_reverts),
        CASE(ids_separate_same_labels),
        CASE(tree_node_remembers_being_opened),
        CASE(panel_scrolls_with_the_wheel),
        CASE(later_panel_wins_the_pointer),
        CASE(int_and_enum_fields),
        CASE(style_reads_the_file_shape),
    };
    return test::Suite{.name = "ui", .cases = cases, .count = ARRAY_COUNT(cases)};
}
