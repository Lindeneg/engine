#include "ui/ui.h"

#include <stdlib.h> // strtof
#include <string.h>

#include "core/logger.h"
#include "platform/path.h"
#include "ui/textedit.h"

#define UI_CFG "ui.cfg"

#define DEFAULT_MAX_RETAINED 1024
#define DEFAULT_MAX_ID_DEPTH 32

#define DEFAULT_PADDING 6.0f
#define DEFAULT_SPACING 4.0f
#define DEFAULT_ROW_HEIGHT 22.0f
#define DEFAULT_TITLE_HEIGHT 24.0f
#define DEFAULT_BORDER 1.0f
#define DEFAULT_SCROLLBAR_WIDTH 10.0f
#define DEFAULT_INDENT 16.0f
#define DEFAULT_SCROLL_STEP 40.0f
#define DEFAULT_CHECKBOX_SIZE 14.0f
#define DEFAULT_DRAG_THRESHOLD 4.0f

// the label carried beside the pointer while dragging
#define DRAG_LABEL_CAP 64
#define DRAG_LABEL_OFFSET 12.0f

// the focused number field edits here; no number needs more
#define EDIT_CAP 64
#define FLOAT_FORMAT "%.5f"

#define TREE_OPEN_MARK "[-] "
#define TREE_CLOSED_MARK "[+] "

// where the pointer reads while mouse look owns it: over nothing
#define OFFSCREEN -1.0f

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

namespace ui {

typedef u32 Id;
#define ID_NONE 0u

static const linalg::Vec4 DEFAULT_PANEL_BACKGROUND = {0.13f, 0.14f, 0.16f, 0.96f};
static const linalg::Vec4 DEFAULT_PANEL_BORDER = {0.30f, 0.30f, 0.34f, 1.0f};
static const linalg::Vec4 DEFAULT_TITLE_BACKGROUND = {0.20f, 0.21f, 0.25f, 1.0f};
static const linalg::Vec4 DEFAULT_TEXT = {0.92f, 0.92f, 0.92f, 1.0f};
static const linalg::Vec4 DEFAULT_TEXT_DIM = {0.60f, 0.60f, 0.62f, 1.0f};
static const linalg::Vec4 DEFAULT_WIDGET = {0.24f, 0.25f, 0.29f, 1.0f};
static const linalg::Vec4 DEFAULT_WIDGET_HOT = {0.32f, 0.33f, 0.38f, 1.0f};
static const linalg::Vec4 DEFAULT_WIDGET_ACTIVE = {0.40f, 0.42f, 0.48f, 1.0f};
static const linalg::Vec4 DEFAULT_ACCENT = {0.35f, 0.65f, 1.00f, 1.0f};
static const linalg::Vec4 DEFAULT_FIELD = {0.10f, 0.10f, 0.12f, 1.0f};
static const linalg::Vec4 DEFAULT_SELECTION = {0.35f, 0.65f, 1.00f, 0.4f};

Style style_from(config::Node *first) {
    Style s{};
    s.padding = config::get_f32(first, "padding", DEFAULT_PADDING);
    s.spacing = config::get_f32(first, "spacing", DEFAULT_SPACING);
    s.row_height = config::get_f32(first, "row_height", DEFAULT_ROW_HEIGHT);
    s.title_height = config::get_f32(first, "title_height", DEFAULT_TITLE_HEIGHT);
    s.border = config::get_f32(first, "border", DEFAULT_BORDER);
    s.scrollbar_width = config::get_f32(first, "scrollbar_width", DEFAULT_SCROLLBAR_WIDTH);
    s.indent = config::get_f32(first, "indent", DEFAULT_INDENT);
    s.scroll_step = config::get_f32(first, "scroll_step", DEFAULT_SCROLL_STEP);
    s.checkbox_size = config::get_f32(first, "checkbox_size", DEFAULT_CHECKBOX_SIZE);
    s.drag_threshold = config::get_f32(first, "drag_threshold", DEFAULT_DRAG_THRESHOLD);

    s.panel_background =
        config::value_vec4(config::find(first, "panel_background"), DEFAULT_PANEL_BACKGROUND);
    s.panel_border = config::value_vec4(config::find(first, "panel_border"), DEFAULT_PANEL_BORDER);
    s.title_background =
        config::value_vec4(config::find(first, "title_background"), DEFAULT_TITLE_BACKGROUND);
    s.text = config::value_vec4(config::find(first, "text"), DEFAULT_TEXT);
    s.text_dim = config::value_vec4(config::find(first, "text_dim"), DEFAULT_TEXT_DIM);
    s.widget = config::value_vec4(config::find(first, "widget"), DEFAULT_WIDGET);
    s.widget_hot = config::value_vec4(config::find(first, "widget_hot"), DEFAULT_WIDGET_HOT);
    s.widget_active =
        config::value_vec4(config::find(first, "widget_active"), DEFAULT_WIDGET_ACTIVE);
    s.accent = config::value_vec4(config::find(first, "accent"), DEFAULT_ACCENT);
    s.field = config::value_vec4(config::find(first, "field"), DEFAULT_FIELD);
    s.selection = config::value_vec4(config::find(first, "selection"), DEFAULT_SELECTION);
    return s;
}

StyleResult load_style(memory::Arena *scratch, const char *root) {
    ASSERT(scratch && root);

    memory::ArenaTemp t = memory::temp_begin(scratch);
    const char *path = FPATH(scratch, root, UI_CFG);

    config::DocResult doc = config::load(scratch, path);
    if (doc.err != result::OK) {
        LOG_ERR("ui: cannot read %s", path);
        memory::temp_end(t);
        return StyleResult{.err = doc.err};
    }
    if (doc.val->problems) {
        LOG_ERR("ui: %s has %u structural problem(s)", path, doc.val->problems);
        memory::temp_end(t);
        return StyleResult{.err = result::ERR_PARSE};
    }

    Style s = style_from(doc.val->first);
    memory::temp_end(t);
    return StyleResult{.val = s};
}

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .max_retained = (u32)config::get_i32(body, "max_retained", DEFAULT_MAX_RETAINED),
        .max_id_depth = (u32)config::get_i32(body, "max_id_depth", DEFAULT_MAX_ID_DEPTH),
    };
}

Input input_from(const input::State *s) {
    ASSERT(s);

    Input in{};
    if (input::mouse_look(s)) {
        in.mouse = linalg::Vec2{OFFSCREEN, OFFSCREEN};
        return in;
    }

    in.mouse = input::cursor(s);
    in.mouse_down = input::button_down(s, window::BUTTON_LEFT);
    in.mouse_pressed = input::button_pressed(s, window::BUTTON_LEFT);
    in.mouse_released = input::button_released(s, window::BUTTON_LEFT);
    in.scroll = input::scroll(s);
    in.text = input::text(s);
    for (u32 k = 0; k < window::KEY_COUNT; k++)
        in.key_pressed[k] = input::key_pressed(s, (window::Key)k) ? 1 : 0;
    in.shift = input::key_down(s, window::KEY_LEFT_SHIFT) ||
               input::key_down(s, window::KEY_RIGHT_SHIFT);
    in.control = input::key_down(s, window::KEY_LEFT_CONTROL) ||
                 input::key_down(s, window::KEY_RIGHT_CONTROL);
    return in;
}

// what a panel or a tree node keeps between frames
struct Retained {
    Id id;
    f32 scroll;
    f32 max_scroll; // from the last frame's extent, so a wheel tick is clamped before layout
    b32 open;
};

struct Panel {
    Id id;
    canvas::Rect rect;
    canvas::Rect content; // inside the padding and beside the scrollbar
    f32 padding;          // the style's, unless the panel asked for its own
    f32 scroll;
    f32 cursor_y; // top of the next row
    f32 max_y;    // bottom of the lowest row so far
    u32 columns;
    u32 column;
    f32 weights[UI_MAX_COLUMNS]; // each column's share of the row, summing to 1
    f32 indent;
};

static struct {
    Config cfg;
    Style style;
    font::Font *font;

    Retained *retained; // NULL until init
    u32 retained_count;

    Id *id_stack;
    u32 id_depth;

    Input in;
    b32 in_frame;

    Id hot;           // from last frame
    Id hot_candidate; // being decided this frame
    Id active;        // holds the button
    Id focus;         // has the keyboard
    b32 focus_seen;   // the focused field was drawn this frame
    b32 mouse_over_panel;

    Panel panel;
    b32 in_panel;

    // the focused field's cursor, how far its text is scrolled to keep the
    // cursor in view, and a number field's text while it is edited
    u32 cursor;
    u32 anchor;
    f32 scroll;
    char edit[EDIT_CAP];

    // a drag: where the press was, what is being carried, and where it is over
    linalg::Vec2 press_at;
    b32 drag_active;
    u32 drag_payload;
    u32 drop_candidate;
    char drag_label[DRAG_LABEL_CAP];
} g;

void init(memory::Arena *a, Config cfg, Style style, font::Font *font) {
    ASSERT(a && font);
    ASSERT_MSG(!g.retained, "ui::init called twice");
    ASSERT_MSG(cfg.max_retained && cfg.max_id_depth, "ui: zero capacity");

    g.cfg = cfg;
    g.style = style;
    g.font = font;
    g.retained = push_array(a, Retained, cfg.max_retained);
    g.id_stack = push_array(a, Id, cfg.max_id_depth);
}

void shutdown() { memset(&g, 0, sizeof(g)); }

const Style *style() { return &g.style; }

static Id hash_bytes(const void *p, u64 n, Id seed) {
    const u8 *b = (const u8 *)p;
    Id h = seed;
    for (u64 i = 0; i < n; i++)
        h = (h ^ b[i]) * FNV_PRIME;
    return h ? h : 1; // ID_NONE is reserved
}

static Id top_id() { return g.id_depth ? g.id_stack[g.id_depth - 1] : FNV_OFFSET; }

static Id make_id(const char *s) { return hash_bytes(s, strlen(s), top_id()); }

static void push(Id id) {
    ASSERT_MSG(g.id_depth < g.cfg.max_id_depth, "ui: ids nested deeper than %u",
               g.cfg.max_id_depth);
    g.id_stack[g.id_depth++] = id;
}

void push_id(const char *s) {
    ASSERT(s);
    push(make_id(s));
}

void push_id(u32 v) { push(hash_bytes(&v, sizeof(v), top_id())); }

void pop_id() {
    ASSERT_MSG(g.id_depth > 0, "ui: pop_id without a push");
    g.id_depth--;
}

static Retained *retained(Id id, b32 *out_created) {
    for (u32 i = 0; i < g.retained_count; i++) {
        if (g.retained[i].id == id) {
            *out_created = false;
            return &g.retained[i];
        }
    }

    *out_created = true;
    if (g.retained_count == g.cfg.max_retained) {
        LOG_WARN_ONCE("ui: more than %u panels and tree nodes; the rest forget their state",
                      g.cfg.max_retained);
        static Retained forgetful;
        forgetful = Retained{.id = id};
        return &forgetful;
    }

    Retained *r = &g.retained[g.retained_count++];
    *r = Retained{.id = id};
    return r;
}

static void text_left(canvas::Rect r, const char *s, linalg::Vec4 color, f32 inset);

void begin_frame(const Input *in) {
    ASSERT(in);
    ASSERT_MSG(g.retained, "ui: init before begin_frame");
    ASSERT_MSG(!g.in_frame, "ui: begin_frame twice");

    g.in = *in;
    g.in_frame = true;
    g.mouse_over_panel = false;
    g.focus_seen = false;
    g.drop_candidate = 0;

    // escape drops the payload where nobody sees it, and the press with it
    if (g.drag_active && in->key_pressed[window::KEY_ESCAPE]) {
        g.drag_active = false;
        g.drag_payload = 0;
        g.active = ID_NONE;
    }
}

void end_frame() {
    ASSERT_MSG(g.in_frame, "ui: end_frame without begin_frame");
    ASSERT_MSG(!g.in_panel, "ui: a panel was begun and never ended");
    ASSERT_MSG(g.id_depth == 0, "ui: an id was pushed and never popped");

    g.hot = g.hot_candidate;
    g.hot_candidate = ID_NONE;

    // nothing holds the button once it is up, whatever the widget saw
    if (!g.in.mouse_down)
        g.active = ID_NONE;

    // a field that was not drawn this frame cannot keep the keyboard
    if (g.focus && !g.focus_seen)
        g.focus = ID_NONE;

    // the drag rides beside the pointer, and ends with the button
    if (g.drag_active) {
        const Style *s = &g.style;
        f32 w = font::width(g.font, g.drag_label) + 2.0f * s->padding;
        canvas::Rect tag{g.in.mouse.x + DRAG_LABEL_OFFSET, g.in.mouse.y + DRAG_LABEL_OFFSET, w,
                         s->row_height};
        canvas::rect(tag, s->widget_active);
        text_left(tag, g.drag_label, s->text, s->padding);

        if (!g.in.mouse_down) {
            g.drag_active = false;
            g.drag_payload = 0;
        }
    }

    g.in_frame = false;
}

b32 wants_mouse() { return g.mouse_over_panel || g.active != ID_NONE; }
b32 wants_keyboard() { return g.focus != ID_NONE; }
b32 dragging() { return g.active != ID_NONE; }

static b32 contains(canvas::Rect r, linalg::Vec2 p) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

// Where the pointer is this frame, within the panel's visible content.
static b32 inside(canvas::Rect r) {
    return contains(r, g.in.mouse) && (!g.in_panel || contains(g.panel.content, g.in.mouse));
}

// Registers the item for next frame's hot, and answers whether it is hot now.
// A press is gated on this, so only the topmost item under the pointer takes
// it; a release is judged on inside(), where the pointer is right now.
static b32 hovered(Id id, canvas::Rect r) {
    if (inside(r))
        g.hot_candidate = id;
    return g.hot == id;
}

// text vertically centred in a row
static f32 baseline_in(canvas::Rect r) {
    f32 text_h = g.font->ascent + g.font->descent;
    return r.y + (r.h - text_h) * 0.5f + g.font->ascent;
}

static void text_left(canvas::Rect r, const char *s, linalg::Vec4 color, f32 inset) {
    canvas::text(g.font, r.x + inset, baseline_in(r), s, color);
}

static void text_center(canvas::Rect r, const char *s, linalg::Vec4 color) {
    f32 w = font::width(g.font, s);
    canvas::text(g.font, r.x + (r.w - w) * 0.5f, baseline_in(r), s, color);
}

static linalg::Vec4 face_color(Id id) {
    if (g.active == id)
        return g.style.widget_active;
    if (g.hot == id)
        return g.style.widget_hot;
    return g.style.widget;
}

void begin_panel(const char *title, canvas::Rect r) { begin_panel(title, r, g.style.padding); }

void begin_panel(const char *title, canvas::Rect r, f32 padding) {
    ASSERT(title);
    ASSERT_MSG(g.in_frame, "ui: begin_panel outside a frame");
    ASSERT_MSG(!g.in_panel, "ui: panels do not nest");

    push_id(title);
    Id id = top_id();

    b32 created = false;
    Retained *st = retained(id, &created);

    const Style *s = &g.style;

    // the panel body takes the pointer from anything drawn under it
    if (contains(r, g.in.mouse)) {
        g.mouse_over_panel = true;
        g.hot_candidate = id;
        if (g.in.scroll != 0.0f)
            st->scroll -= g.in.scroll * s->scroll_step;
        if (st->scroll < 0.0f)
            st->scroll = 0.0f;
        if (st->scroll > st->max_scroll)
            st->scroll = st->max_scroll; // last frame's extent; end_panel refines it
    }

    canvas::rect(r, s->panel_background);
    canvas::Rect title_bar{r.x, r.y, r.w, s->title_height};
    canvas::rect(title_bar, s->title_background);
    text_left(title_bar, title, s->text, s->padding);
    canvas::rect_outline(r, s->border, s->panel_border);

    Panel *p = &g.panel;
    p->id = id;
    p->rect = r;
    p->padding = padding;
    p->content = canvas::Rect{
        r.x + padding,
        r.y + s->title_height + padding,
        r.w - 2.0f * padding - s->scrollbar_width,
        r.h - s->title_height - 2.0f * padding,
    };
    p->scroll = st->scroll;
    p->cursor_y = p->content.y - st->scroll;
    p->max_y = p->cursor_y;
    p->columns = 1;
    p->column = 0;
    p->weights[0] = 1.0f;
    p->indent = 0.0f;
    g.in_panel = true;

    canvas::push_clip(p->content);
}

void end_panel() {
    ASSERT_MSG(g.in_panel, "ui: end_panel without begin_panel");
    Panel *p = &g.panel;
    const Style *s = &g.style;

    canvas::pop_clip();

    b32 created = false;
    Retained *st = retained(p->id, &created);

    f32 content_h = p->max_y - (p->content.y - p->scroll);
    f32 visible_h = p->content.h;
    f32 max_scroll = MAX(content_h - visible_h, 0.0f);
    st->max_scroll = max_scroll;
    if (st->scroll > max_scroll)
        st->scroll = max_scroll;

    if (content_h > visible_h) {
        canvas::Rect track{
            p->rect.x + p->rect.w - p->padding - s->scrollbar_width,
            p->content.y,
            s->scrollbar_width,
            p->content.h,
        };
        canvas::rect(track, s->field);

        f32 thumb_h = MAX(track.h * visible_h / content_h, s->scrollbar_width);
        f32 thumb_y = track.y + (track.h - thumb_h) * (max_scroll > 0.0f ? st->scroll / max_scroll : 0.0f);
        canvas::rect(canvas::Rect{track.x, thumb_y, track.w, thumb_h}, s->widget);
    }

    g.in_panel = false;
    pop_id();
}

static void wrap_row(Panel *p) {
    p->column = 0;
    p->cursor_y += g.style.row_height + g.style.spacing;
}

static canvas::Rect next_rect() {
    ASSERT_MSG(g.in_panel, "ui: a widget outside a panel");
    Panel *p = &g.panel;
    const Style *s = &g.style;

    // the spacing comes off the top, and what is left is shared by weight
    f32 avail = p->content.w - p->indent - s->spacing * (f32)(p->columns - 1);
    f32 x = p->content.x + p->indent;
    for (u32 c = 0; c < p->column; c++)
        x += avail * p->weights[c] + s->spacing;

    canvas::Rect r{x, p->cursor_y, avail * p->weights[p->column], s->row_height};

    p->column++;
    if (p->column == p->columns)
        wrap_row(p);
    p->max_y = MAX(p->max_y, r.y + r.h);
    return r;
}

void columns(u32 n, const f32 *weights) {
    ASSERT_MSG(g.in_panel, "ui: columns outside a panel");
    ASSERT_MSG(n >= 1 && n <= UI_MAX_COLUMNS, "ui: %u columns; the cap is %d", n, UI_MAX_COLUMNS);
    Panel *p = &g.panel;
    if (p->column != 0)
        wrap_row(p);
    p->columns = n;

    f32 total = 0.0f;
    for (u32 i = 0; i < n; i++)
        total += weights ? weights[i] : 1.0f;
    ASSERT_MSG(total > 0.0f, "ui: column weights sum to nothing");
    for (u32 i = 0; i < n; i++)
        p->weights[i] = (weights ? weights[i] : 1.0f) / total;
}

void columns(u32 n) { columns(n, NULL); }

void label(const char *text) {
    ASSERT(text);
    canvas::Rect r = next_rect();
    text_left(r, text, g.style.text, 0.0f);
}

void label_dim(const char *text) {
    ASSERT(text);
    canvas::Rect r = next_rect();
    text_left(r, text, g.style.text_dim, 0.0f);
}

void spacer() { next_rect(); }

b32 button(const char *label_) {
    ASSERT(label_);
    canvas::Rect r = next_rect();
    Id id = make_id(label_);

    b32 hov = hovered(id, r);
    if (hov && g.in.mouse_pressed)
        g.active = id;

    b32 clicked = false;
    if (g.active == id && g.in.mouse_released) {
        clicked = inside(r);
        g.active = ID_NONE;
    }

    canvas::rect(r, face_color(id));
    text_center(r, label_, g.style.text);
    return clicked;
}

b32 checkbox(const char *label_, b32 *value) {
    ASSERT(label_ && value);
    canvas::Rect r = next_rect();
    Id id = make_id(label_);
    const Style *s = &g.style;

    b32 hov = hovered(id, r);
    if (hov && g.in.mouse_pressed)
        g.active = id;

    b32 toggled = false;
    if (g.active == id && g.in.mouse_released) {
        if (inside(r)) {
            *value = !*value;
            toggled = true;
        }
        g.active = ID_NONE;
    }

    canvas::Rect box{r.x, r.y + (r.h - s->checkbox_size) * 0.5f, s->checkbox_size,
                     s->checkbox_size};
    canvas::rect(box, face_color(id));
    if (*value) {
        f32 inset = s->checkbox_size * 0.25f;
        canvas::rect(canvas::Rect{box.x + inset, box.y + inset, box.w - 2.0f * inset,
                                  box.h - 2.0f * inset},
                     s->accent);
    }
    text_left(r, label_, s->text, s->checkbox_size + s->padding);
    return toggled;
}

void separator() {
    canvas::Rect r = next_rect();
    canvas::rect(canvas::Rect{r.x, r.y + r.h * 0.5f, r.w, g.style.border}, g.style.panel_border);
}

b32 slider(const char *id_, f32 *value, f32 lo, f32 hi) {
    ASSERT(id_ && value);
    ASSERT_MSG(hi > lo, "ui: slider '%s' has an empty range", id_);
    canvas::Rect r = next_rect();
    Id id = make_id(id_);
    const Style *s = &g.style;

    b32 hov = hovered(id, r);
    if (hov && g.in.mouse_pressed)
        g.active = id;

    b32 changed = false;
    if (g.active == id) {
        f32 t = (g.in.mouse.x - r.x) / r.w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        f32 v = lo + t * (hi - lo);
        if (v != *value) {
            *value = v;
            changed = true;
        }
        if (g.in.mouse_released)
            g.active = ID_NONE;
    }

    f32 t = (*value - lo) / (hi - lo);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    canvas::rect(r, face_color(id));
    canvas::rect(canvas::Rect{r.x, r.y, r.w * t, r.h}, s->accent);

    char text[EDIT_CAP];
    snprintf(text, sizeof(text), FLOAT_FORMAT, (f64)*value);
    text_center(r, text, s->text);
    return changed;
}

// the editing keys against the focused field; true when the text changed
static b32 edit_keys(textedit::Buffer *b) {
    const Input *in = &g.in;
    b32 changed = false;

    if (in->text && in->text[0]) {
        textedit::insert(b, in->text);
        changed = true;
    }
    if (in->key_pressed[window::KEY_BACKSPACE])
        changed = textedit::erase_backward(b) || changed;
    if (in->key_pressed[window::KEY_DELETE])
        changed = textedit::erase_forward(b) || changed;
    if (in->key_pressed[window::KEY_LEFT])
        textedit::move(b, -1, in->control, in->shift);
    if (in->key_pressed[window::KEY_RIGHT])
        textedit::move(b, 1, in->control, in->shift);
    if (in->key_pressed[window::KEY_HOME])
        textedit::home(b, in->shift);
    if (in->key_pressed[window::KEY_END])
        textedit::end(b, in->shift);
    if (in->control && in->key_pressed[window::KEY_A])
        textedit::select_all(b);

    return changed;
}

static void draw_field(canvas::Rect r, const char *text, b32 focused, u32 cursor, u32 anchor) {
    const Style *s = &g.style;

    canvas::rect(r, s->field);
    canvas::rect_outline(r, s->border, focused ? s->accent : s->panel_border);

    // the text is clipped inside the padding and, while editing, scrolled so
    // the cursor is always in view: a long path is edited at its end, not blind
    canvas::Rect inner{r.x + s->padding, r.y, r.w - 2.0f * s->padding, r.h};
    f32 scroll = 0.0f;
    if (focused) {
        f32 cursor_x = font::width(g.font, text, cursor);
        f32 total_w = font::width(g.font, text);
        if (cursor_x - g.scroll > inner.w)
            g.scroll = cursor_x - inner.w;
        if (cursor_x < g.scroll)
            g.scroll = cursor_x;
        f32 max_scroll = MAX(total_w - inner.w, 0.0f);
        if (g.scroll > max_scroll)
            g.scroll = max_scroll;
        scroll = g.scroll;
    }

    canvas::push_clip(inner);
    f32 x = inner.x - scroll;
    f32 text_h = g.font->ascent + g.font->descent;
    f32 text_y = r.y + (r.h - text_h) * 0.5f;

    if (focused && cursor != anchor) {
        u32 lo = MIN(cursor, anchor);
        u32 hi = MAX(cursor, anchor);
        f32 x0 = x + font::width(g.font, text, lo);
        f32 x1 = x + font::width(g.font, text, hi);
        canvas::rect(canvas::Rect{x0, text_y, x1 - x0, text_h}, s->selection);
    }

    canvas::text(g.font, x, baseline_in(r), text, s->text);

    if (focused) {
        f32 cx = x + font::width(g.font, text, cursor);
        canvas::rect(canvas::Rect{cx, text_y, s->border, text_h}, s->text);
    }
    canvas::pop_clip();
}

// takes the keyboard for a field; select_all is what a number wants, since the
// next keystroke replaces it
static void focus(Id id, u32 len, b32 select_all) {
    g.focus = id;
    g.cursor = len;
    g.anchor = select_all ? 0 : len;
    g.scroll = 0.0f; // draw_field scrolls to the cursor on the first frame
}

// A field over a formatted value: shows `shown` when idle, edits a copy of it
// while focused, and on commit hands the text back for the caller to parse.
// Focusing selects the whole value, since the next keystroke usually replaces it.
static b32 value_field(const char *id_, const char *shown, char *out_text, u32 cap) {
    canvas::Rect r = next_rect();
    Id id = make_id(id_);
    const Input *in = &g.in;

    b32 hov = hovered(id, r);
    if (hov && in->mouse_pressed && g.focus != id) {
        snprintf(g.edit, sizeof(g.edit), "%s", shown);
        focus(id, (u32)strlen(g.edit), true);
    }

    if (g.focus != id) {
        draw_field(r, shown, false, 0, 0);
        return false;
    }

    g.focus_seen = true;

    textedit::Buffer b = textedit::create(g.edit, sizeof(g.edit));
    b.cursor = MIN(g.cursor, b.len);
    b.anchor = MIN(g.anchor, b.len);
    edit_keys(&b);
    g.cursor = b.cursor;
    g.anchor = b.anchor;

    b32 committed = false;
    if (in->key_pressed[window::KEY_ENTER] || (in->mouse_pressed && !hov)) {
        snprintf(out_text, cap, "%s", g.edit);
        committed = true;
        g.focus = ID_NONE;
    } else if (in->key_pressed[window::KEY_ESCAPE]) {
        g.focus = ID_NONE;
    }

    draw_field(r, g.edit, g.focus == id, g.cursor, g.anchor);
    return committed;
}

b32 float_field(const char *id_, f32 *value) {
    ASSERT(id_ && value);

    char shown[EDIT_CAP];
    snprintf(shown, sizeof(shown), FLOAT_FORMAT, (f64)*value);

    char text[EDIT_CAP];
    if (!value_field(id_, shown, text, sizeof(text)))
        return false;

    char *end = NULL;
    f32 v = strtof(text, &end);
    if (end == text || v == *value)
        return false;
    *value = v;
    return true;
}

b32 int_field(const char *id_, i32 *value) {
    ASSERT(id_ && value);

    char shown[EDIT_CAP];
    snprintf(shown, sizeof(shown), "%d", *value);

    char text[EDIT_CAP];
    if (!value_field(id_, shown, text, sizeof(text)))
        return false;

    char *end = NULL;
    i32 v = (i32)strtol(text, &end, 10);
    if (end == text || v == *value)
        return false;
    *value = v;
    return true;
}

b32 enum_field(const char *id_, const char *const *names, u32 count, i32 *index) {
    ASSERT(id_ && names && count && index);

    // an index outside the table shows the first name rather than reading past it
    u32 shown = (u32)*index < count ? (u32)*index : 0;

    push_id(id_);
    b32 clicked = button(names[shown]);
    pop_id();
    if (!clicked)
        return false;

    *index = (i32)((shown + 1) % count);
    return true;
}

b32 text_field(const char *id_, char *buf, u32 cap) {
    ASSERT(id_ && buf && cap);
    canvas::Rect r = next_rect();
    Id id = make_id(id_);
    const Input *in = &g.in;

    b32 hov = hovered(id, r);
    if (hov && in->mouse_pressed && g.focus != id)
        focus(id, (u32)strlen(buf), false);

    if (g.focus != id) {
        draw_field(r, buf, false, 0, 0);
        return false;
    }

    g.focus_seen = true;

    textedit::Buffer b = textedit::create(buf, cap);
    b.cursor = MIN(g.cursor, b.len);
    b.anchor = MIN(g.anchor, b.len);
    b32 changed = edit_keys(&b);
    g.cursor = b.cursor;
    g.anchor = b.anchor;

    if (in->key_pressed[window::KEY_ENTER] || in->key_pressed[window::KEY_ESCAPE] ||
        (in->mouse_pressed && !hov))
        g.focus = ID_NONE;

    draw_field(r, buf, g.focus == id, g.cursor, g.anchor);
    return changed;
}

b32 tree_node(const char *label_, b32 open_by_default) {
    ASSERT(label_);
    canvas::Rect r = next_rect();
    Id id = make_id(label_);
    const Style *s = &g.style;

    b32 created = false;
    Retained *st = retained(id, &created);
    if (created)
        st->open = open_by_default;

    b32 hov = hovered(id, r);
    if (hov && g.in.mouse_pressed)
        g.active = id;
    if (g.active == id && g.in.mouse_released) {
        if (inside(r))
            st->open = !st->open;
        g.active = ID_NONE;
    }

    canvas::rect(r, face_color(id));
    f32 mark_w = canvas::text(g.font, r.x + s->padding, baseline_in(r),
                              st->open ? TREE_OPEN_MARK : TREE_CLOSED_MARK, s->text_dim);
    text_left(r, label_, s->text, s->padding + mark_w);

    if (!st->open)
        return false;

    push(id);
    g.panel.indent += s->indent;
    return true;
}

b32 tree_item(const char *label_, b32 has_children, b32 selected, b32 *out_open, u32 payload) {
    ASSERT(label_ && out_open);
    canvas::Rect r = next_rect();
    Id id = make_id(label_);
    Id mark_id = hash_bytes("mark", 4, id);
    const Style *s = &g.style;

    b32 created = false;
    Retained *st = retained(id, &created);
    if (!has_children)
        st->open = false;

    f32 mark_w = font::width(g.font, TREE_OPEN_MARK);
    canvas::Rect mark{r.x, r.y, s->padding + mark_w, r.h};
    canvas::Rect body{mark.x + mark.w, r.y, r.w - mark.w, r.h};

    // two targets in one row: the mark, then the rest
    b32 mark_hov = has_children && hovered(mark_id, mark);
    if (mark_hov && g.in.mouse_pressed)
        g.active = mark_id;
    if (g.active == mark_id && g.in.mouse_released) {
        if (inside(mark))
            st->open = !st->open;
        g.active = ID_NONE;
    }

    b32 hov = hovered(id, body);
    if (hov && g.in.mouse_pressed) {
        g.active = id;
        g.press_at = g.in.mouse;
    }

    // held and moved far enough, the press becomes a drag and stops being a click
    if (payload && g.active == id && !g.drag_active) {
        linalg::Vec2 moved = linalg::vec2_sub(g.in.mouse, g.press_at);
        if (moved.x * moved.x + moved.y * moved.y > s->drag_threshold * s->drag_threshold) {
            g.drag_active = true;
            g.drag_payload = payload;
            snprintf(g.drag_label, sizeof(g.drag_label), "%s", label_);
        }
    }

    b32 clicked = false;
    if (g.active == id && g.in.mouse_released) {
        clicked = inside(body) && !g.drag_active;
        g.active = ID_NONE;
    }

    // while something else is carried, this row is where it may land
    b32 target = g.drag_active && payload && payload != g.drag_payload && inside(r);
    if (target)
        g.drop_candidate = payload;

    linalg::Vec4 face = selected ? s->widget_active : face_color(id);
    canvas::rect(r, target ? s->widget_hot : face);
    if (has_children)
        canvas::text(g.font, r.x + s->padding, baseline_in(r),
                     st->open ? TREE_OPEN_MARK : TREE_CLOSED_MARK, s->text_dim);
    text_left(body, label_, s->text, 0.0f);

    *out_open = st->open;
    if (st->open) {
        push(id);
        g.panel.indent += s->indent;
    }
    return clicked;
}

b32 dropped(u32 *out_payload, u32 *out_target) {
    ASSERT(out_payload && out_target);
    if (!g.drag_active || !g.in.mouse_released)
        return false;

    *out_payload = g.drag_payload;
    *out_target = g.drop_candidate;
    return true;
}

void tree_pop() {
    ASSERT_MSG(g.in_panel, "ui: tree_pop outside a panel");
    ASSERT_MSG(g.panel.indent >= g.style.indent, "ui: tree_pop without an open tree_node");
    g.panel.indent -= g.style.indent;
    pop_id();
}

} // namespace ui
