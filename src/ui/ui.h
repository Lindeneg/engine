#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "math/linalg.h"
#include "platform/input.h"
#include "platform/window.h"

// Immediate-mode widgets over the canvas: the editor's panels, and any other
// screen that wants a button. A widget is a call; it draws itself and answers
// what happened. Nothing is retained between frames except what has to be --
// which item is hot, which is active or focused, a panel's scroll, and whether
// a tree node is open -- and that state is keyed by an id hashed from the label
// and the id stack, so two "Delete" buttons in a list are told apart by
// pushing the row's index around them.
//
// Hot is resolved at the end of a frame from whichever item under the pointer
// was drawn last, and used the next frame: one frame of latency, in exchange
// for a later panel correctly winning over one it overlaps.
//
//     ui::Input in = ui::input_from(input);
//     ui::begin_frame(&in);
//     ui::begin_panel("Inspector", rect);
//     ui::columns(2);
//     ui::label("Speed");
//     ui::slider("speed", &speed, 0.0f, 10.0f);
//     ui::end_panel();
//     ui::end_frame();
//
// Layout is a vertical cursor: each widget takes the next row, or the next
// column of it. Panels are placed by the caller and scroll with the wheel.

namespace ui {

// One frame's devices. input_from fills it from the input layer; a test fills
// it by hand.
struct Input {
    linalg::Vec2 mouse; // framebuffer pixels
    b32 mouse_down;     // the left button
    b32 mouse_pressed;
    b32 mouse_released;
    f32 scroll;                        // wheel ticks
    const char *text;                  // typed this frame as UTF-8; NULL or "" for none
    b8 key_pressed[window::KEY_COUNT]; // auto-repeat included
    b32 shift;
    b32 control;
};

// With mouse look on the pointer belongs to the game: nothing is hovered or pressed.
Input input_from(const input::State *s);

// assets/ui.cfg; sizes in pixels, colours 0..1
struct Style {
    f32 padding;      // inside panels and fields
    f32 spacing;      // between rows and columns
    f32 row_height;   // of every widget
    f32 title_height; // a panel's title bar
    f32 border;
    f32 scrollbar_width;
    f32 indent;        // per open tree node
    f32 scroll_step;   // pixels per wheel tick
    f32 checkbox_size; // the box, centred in its row

    linalg::Vec4 panel_background;
    linalg::Vec4 panel_border;
    linalg::Vec4 title_background;
    linalg::Vec4 text;
    linalg::Vec4 text_dim;
    linalg::Vec4 widget; // a button's face, a tree node's header, a scrollbar's thumb
    linalg::Vec4 widget_hot;
    linalg::Vec4 widget_active;
    linalg::Vec4 accent; // a check mark, a slider's fill, a focused field's border
    linalg::Vec4 field;  // a text or number field's face, a scrollbar's track
    linalg::Vec4 selection;
};

// The document's top level; NULL or missing keys take the defaults.
Style style_from(config::Node *first);

RESULT_TYPE(Style, StyleResult);

// Reads assets/ui.cfg. A missing or damaged file is a broken deployment.
StyleResult load_style(memory::Arena *scratch, const char *root);

struct Config {
    u32 max_retained; // panels and tree nodes remembered at once
    u32 max_id_depth;
};

// The `ui` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

void init(memory::Arena *a, Config cfg, Style style, font::Font *font);
void shutdown();

// Inside canvas's frame. end_frame resolves hot and drops stale focus.
void begin_frame(const Input *in);
void end_frame();

// True while the pointer is over a panel or an item holds the button; true
// while a field has keyboard focus. What that means for the game's own maps
// is the caller's decision.
b32 wants_mouse();
b32 wants_keyboard();

void push_id(const char *s);
void push_id(u32 v);
void pop_id();

// Only one panel at a time. The title is the panel's id.
void begin_panel(const char *title, canvas::Rect r);
void end_panel();

// Rows fill left to right across n columns, then wrap. 1 is one widget per row.
void columns(u32 n);

void label(const char *text);
b32 button(const char *label);         // true on the click
b32 checkbox(const char *label, b32 *value); // true when toggled
void separator();

// The field widgets draw no label: pair them with label() in a second column.
// The string is only their identity, unique among siblings. Each returns true
// when the value changed. A slider drags; the fields edit in place, commit on
// enter or on a click elsewhere, and a number field reverts on escape.
b32 slider(const char *id, f32 *value, f32 lo, f32 hi);
b32 float_field(const char *id, f32 *value);
b32 text_field(const char *id, char *buf, u32 cap);

// A collapsing header. True when open, and then everything up to tree_pop is
// indented and scoped under its id. open_by_default applies the first time it is seen.
b32 tree_node(const char *label, b32 open_by_default);
void tree_pop();

const Style *style();

} // namespace ui
