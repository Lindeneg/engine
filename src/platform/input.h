#pragma once

#include "core/common.h"
#include "core/memory.h"
#include "core/result.h"
#include "math/linalg.h"
#include "platform/window.h"

// Devices turned into intent. What an action *is* -- its name, which map it
// belongs to, which keys produce it -- is entirely data, in assets/input.cfg.
// Nothing here knows what "jump" means; code that cares resolves the name once
// at startup and holds the id:
//
//     Action jump = input::find(in, "jump");     // once
//     if (input::consume_pressed(in, jump)) ...  // per fixed step
//
// A map is a named group of actions that can be switched off as a unit. It is
// how the editor stays out of the game's way: with the gameplay map disabled,
// WASD stops driving the player and no gameplay code has to know a panel opened.
// Disabling drops everything in flight, and enabling resyncs against the keys
// already held, so a button released while a panel was open cannot fire late
// and one still held cannot fire twice.
//
// Edges and the fixed step. One frame runs zero, one or several fixed steps, so
// simulation code must never read pressed(): at 120 Hz behind a 60 Hz frame a
// jump would fire twice, and on a frame that runs no step at all it would be
// dropped. A press is latched where it happens and taken by whichever fixed
// step reaches it first -- consume_pressed() -- so it happens exactly once.
// Frame-time code (menus, the editor) reads pressed() and leaves the latch
// alone. Held state -- down() -- is the same on either clock and needs neither.

// bindings per action; a second and third spelling of "forward" is normal
#define INPUT_MAX_BINDINGS 4

// chord modifiers, either side of the keyboard
#define INPUT_MODIFIER_CONTROL (1u << 0)
#define INPUT_MODIFIER_SHIFT (1u << 1)
#define INPUT_MODIFIER_ALT (1u << 2)

// "control+shift+mouse_left" is the longest token, with room to spare
#define INPUT_BINDING_TEXT_CAP 64

namespace input {

// One way to produce an action: a key or a button, with the modifiers that
// must be held with it. A binding without modifiers fires whatever else is
// held, so sprinting does not stop walking.
struct Binding {
    b32 is_button;
    u32 code; // a window::Key or a window::Button
    u32 modifiers;
};

// "z", "mouse_left", "control+z", "control+shift+s": modifiers first, joined
// with '+', the key or button last. False for anything else.
b32 parse_binding(const char *token, Binding *out);

// The token that parses back to it.
void binding_text(Binding b, char *out, u32 cap);

// Ids into the map and action tables; 0 is nothing, as with entity::Handle.
// An action that does not exist reads as never held, so a missing binding
// degrades to a control that does nothing rather than to a crash.
struct Map {
    u32 v;
};

struct Action {
    u32 v;
};

constexpr Map MAP_NONE = {0};
constexpr Action ACTION_NONE = {0};

struct State;

RESULT_TYPE(State *, StateResult);

// Reads assets/input.cfg. A file that is missing, will not parse, or declares
// no maps is a broken deployment, not a content problem: without bindings
// nothing can be played. Inside a map the engine is forgiving -- an unknown key
// name is skipped with a warning, and an action with no usable binding still
// exists, so the editor can show it as unbound rather than lose it.
StateResult init(memory::Arena *a, memory::Arena *scratch, const char *root);

// Samples the devices and works out this frame's edges. Call once per frame,
// after window::poll.
void update(State *s, window::Window *w);

// By name, once, at startup. ACTION_NONE and a warning when the map file does
// not define it; MAP_NONE likewise. Action names are unique across all maps.
// Not const: the name tables are strmaps, and looking one up is a write path.
Action find(State *s, const char *name);
Map find_map(State *s, const char *name);

b32 down(const State *s, Action a);

// This frame's edges. Frame-time code only; see the note above.
b32 pressed(const State *s, Action a);
b32 released(const State *s, Action a);

// Takes the latched press if there is one, clearing it. For the fixed step.
b32 consume_pressed(State *s, Action a);

// Actions in a disabled map read as not held and latch nothing.
void set_map_enabled(State *s, Map m, b32 on);
b32 map_enabled(const State *s, Map m);

// Enumeration, so the editor and a controls screen list what exists rather
// than hardcoding it.
u32 map_count(const State *s);
Map map_at(const State *s, u32 i);
const char *map_name(const State *s, Map m);
u32 action_count(const State *s, Map m);
Action action_at(const State *s, Map m, u32 i);
const char *action_name(const State *s, Action a);
u32 binding_count(const State *s, Action a);
Binding binding_at(const State *s, Action a, u32 i);

// Framebuffer pixels, and the movement since the last frame. Both are device
// state rather than actions, so a disabled map does not hide them. While mouse
// look is on the cursor is locked and only the delta means anything.
linalg::Vec2 cursor(const State *s);
linalg::Vec2 cursor_delta(const State *s);

// Wheel ticks this frame.
f32 scroll(const State *s);

// The raw devices, for widgets: a text field takes typed text and editing keys
// by key rather than by action, since those are conventions and not bindings,
// and a panel reads the buttons whatever the maps say. Text is this frame's
// UTF-8, "" when nothing was typed. A key press includes auto-repeat, so a
// held backspace keeps deleting.
const char *text(const State *s);
b32 key_pressed(const State *s, window::Key k);
b32 key_down(const State *s, window::Key k);
b32 button_down(const State *s, window::Button b);
b32 button_pressed(const State *s, window::Button b);
b32 button_released(const State *s, window::Button b);

// Hides and locks the cursor for mouse look, and asks the platform for raw
// motion. The delta across the change is swallowed, so turning it on does not
// fling the view.
void set_mouse_look(State *s, window::Window *w, b32 on);
b32 mouse_look(const State *s);

} // namespace input
