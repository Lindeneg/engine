#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"

namespace window {

// clang-format off
#define WINDOW_KEY_LIST(EACH)                                                                      \
    EACH(A, "a") EACH(B, "b") EACH(C, "c") EACH(D, "d") EACH(E, "e") EACH(F, "f")                  \
    EACH(G, "g") EACH(H, "h") EACH(I, "i") EACH(J, "j") EACH(K, "k") EACH(L, "l")                  \
    EACH(M, "m") EACH(N, "n") EACH(O, "o") EACH(P, "p") EACH(Q, "q") EACH(R, "r")                  \
    EACH(S, "s") EACH(T, "t") EACH(U, "u") EACH(V, "v") EACH(W, "w") EACH(X, "x")                  \
    EACH(Y, "y") EACH(Z, "z")                                                                      \
    EACH(0, "0") EACH(1, "1") EACH(2, "2") EACH(3, "3") EACH(4, "4")                               \
    EACH(5, "5") EACH(6, "6") EACH(7, "7") EACH(8, "8") EACH(9, "9")                               \
    EACH(ESCAPE, "escape") EACH(SPACE, "space") EACH(ENTER, "enter") EACH(TAB, "tab")              \
    EACH(BACKSPACE, "backspace") EACH(DELETE, "delete") EACH(INSERT, "insert")                     \
    EACH(HOME, "home") EACH(END, "end") EACH(PAGE_UP, "page_up") EACH(PAGE_DOWN, "page_down")      \
    EACH(LEFT_SHIFT, "left_shift") EACH(RIGHT_SHIFT, "right_shift")                                \
    EACH(LEFT_CONTROL, "left_control") EACH(RIGHT_CONTROL, "right_control")                        \
    EACH(LEFT_ALT, "left_alt") EACH(RIGHT_ALT, "right_alt")                                        \
    EACH(UP, "up") EACH(DOWN, "down") EACH(LEFT, "left") EACH(RIGHT, "right")                      \
    EACH(F1, "f1") EACH(F2, "f2") EACH(F3, "f3") EACH(F4, "f4") EACH(F5, "f5")                     \
    EACH(F6, "f6") EACH(F7, "f7") EACH(F8, "f8") EACH(F9, "f9") EACH(F10, "f10")                   \
    EACH(F11, "f11") EACH(F12, "f12")                                                              \
    EACH(GRAVE_ACCENT, "grave_accent") EACH(MINUS, "minus") EACH(EQUAL, "equal")                   \
    EACH(LEFT_BRACKET, "left_bracket") EACH(RIGHT_BRACKET, "right_bracket")                        \
    EACH(SEMICOLON, "semicolon") EACH(APOSTROPHE, "apostrophe") EACH(COMMA, "comma")               \
    EACH(PERIOD, "period") EACH(SLASH, "slash") EACH(BACKSLASH, "backslash")
// clang-format on

// Names carry the mouse_ prefix so one config token names either a key or a
// button without the file having to say which it is.
#define WINDOW_BUTTON_LIST(EACH)                                                                   \
    EACH(LEFT, "mouse_left")                                                                       \
    EACH(RIGHT, "mouse_right") EACH(MIDDLE, "mouse_middle") EACH(4, "mouse_4") EACH(5, "mouse_5")

enum Key {
#define WINDOW_KEY_ENUM(ID, name) KEY_##ID,
    WINDOW_KEY_LIST(WINDOW_KEY_ENUM)
#undef WINDOW_KEY_ENUM
        KEY_COUNT,
};

enum Button {
#define WINDOW_BUTTON_ENUM(ID, name) BUTTON_##ID,
    WINDOW_BUTTON_LIST(WINDOW_BUTTON_ENUM)
#undef WINDOW_BUTTON_ENUM
        BUTTON_COUNT,
};

// Name tables for config files and the editor.
const char *key_name(Key k);
const char *button_name(Button b);
b32 key_from_name(const char *s, Key *out);
b32 button_from_name(const char *s, Button *out);

struct Config {
    i32 width;
    i32 height;
    const char *title; // must outlive the window
    b32 vsync;
};

// The `window` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

struct Window;

RESULT_TYPE(Window *, WindowResult);

// Creates the window and a GL 3.3 core context, and loads the GL entry points.
WindowResult create(memory::Arena *a, Config cfg);
void destroy(Window *w);

b32 should_close(Window *w);
void poll(Window *w);
void swap(Window *w);

// The title bar; a project names it once it is open.
void set_title(Window *w, const char *title);

b32 key_down(Window *w, Key k);
b32 button_down(Window *w, Button b);

// In framebuffer pixels, so it compares directly against the viewport on a
// scaled display, where GLFW's own cursor position does not.
void cursor_position(Window *w, f32 *out_x, f32 *out_y);

// Wheel ticks accumulated since the last call; reading clears the accumulator.
f32 take_scroll(Window *w);

// Typed text and key presses only arrive as events, so the window queues them
// between polls and the input layer takes them once per frame. Text is UTF-8;
// presses include auto-repeat. What overflows a queue between two polls is
// dropped, since nobody types that fast.
u32 take_text(Window *w, char *out, u32 cap); // bytes written, NUL not counted
u32 take_key_presses(Window *w, Key *out, u32 cap);

// Captured hides the cursor and locks it to the window, which is what mouse
// look needs. It also asks for raw motion where the platform has it, so the
// desktop's pointer acceleration does not sit between the mouse and the aim.
void set_cursor_captured(Window *w, b32 captured);
b32 cursor_captured(Window *w);

// Framebuffer, not window size: they differ on scaled displays.
void framebuffer_size(Window *w, i32 *out_w, i32 *out_h);

f64 time();

} // namespace window

// queue sizes between two polls
#define WINDOW_TEXT_CAP 64
#define WINDOW_KEY_EVENT_CAP 32
