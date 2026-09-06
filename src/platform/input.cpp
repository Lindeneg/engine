#include "platform/input.h"

#include <string.h>

#include "core/config.h"
#include "core/logger.h"
#include "core/str.h"
#include "core/strmap.h"

#define MAP_KEY "map"

namespace input {

struct ActionDef {
    const char *name;
    u32 map; // the map it was declared in

    Binding bindings[INPUT_MAX_BINDINGS];
    u32 binding_count;

    b8 down;
    b8 prev;
    b8 latched; // a press waiting for a fixed step to take it
};

struct MapDef {
    const char *name;
    u32 first_action; // actions are filled map by map, so a map's are contiguous
    u32 count;
    b32 enabled;

    // set when the map is switched on: the next update adopts whatever is
    // already held instead of reporting it as a fresh press
    b32 resync;
};

struct State {
    MapDef *maps;
    u32 map_count;
    u32 max_maps;

    ActionDef *actions;
    u32 action_count;
    u32 max_actions;

    strmap::Map by_map;    // name -> map id
    strmap::Map by_action; // name -> action id

    linalg::Vec2 cursor;
    linalg::Vec2 delta;
    f32 scroll;

    // the raw devices, sampled each update
    char text[WINDOW_TEXT_CAP];
    b8 key_pressed[window::KEY_COUNT];
    b8 key_down[window::KEY_COUNT];
    b8 button_down[window::BUTTON_COUNT];
    b8 button_prev[window::BUTTON_COUNT];

    b32 mouse_look;
    b32 warp; // swallow the next delta: the cursor moved without the mouse moving
};

// modifier names in a chord, and the bits they set
#define MODIFIER_COUNT 3
static const char *MODIFIER_NAMES[MODIFIER_COUNT] = {"control", "shift", "alt"};
static const u32 MODIFIER_BITS[MODIFIER_COUNT] = {INPUT_MODIFIER_CONTROL, INPUT_MODIFIER_SHIFT,
                                                   INPUT_MODIFIER_ALT};
static const window::Key MODIFIER_KEYS[MODIFIER_COUNT][2] = {
    {window::KEY_LEFT_CONTROL, window::KEY_RIGHT_CONTROL},
    {window::KEY_LEFT_SHIFT, window::KEY_RIGHT_SHIFT},
    {window::KEY_LEFT_ALT, window::KEY_RIGHT_ALT},
};

// the last part of a token: a button or a key
static b32 source_from_name(const char *s, Binding *out) {
    // buttons first: their names are all prefixed, so there is nothing to
    // disambiguate against the key names
    window::Button b;
    if (window::button_from_name(s, &b)) {
        out->is_button = true;
        out->code = (u32)b;
        return true;
    }

    window::Key k;
    if (window::key_from_name(s, &k)) {
        out->is_button = false;
        out->code = (u32)k;
        return true;
    }

    return false;
}

b32 parse_binding(const char *token, Binding *out) {
    ASSERT(token && out);

    char buf[INPUT_BINDING_TEXT_CAP];
    if (snprintf(buf, sizeof(buf), "%s", token) >= (i32)sizeof(buf))
        return false;

    Binding b{};
    char *part = buf;
    for (;;) {
        char *plus = strchr(part, '+');
        if (!plus)
            break;
        *plus = 0;

        i32 m = str::name_index(MODIFIER_NAMES, MODIFIER_COUNT, part);
        if (m < 0)
            return false;
        b.modifiers |= MODIFIER_BITS[m];
        part = plus + 1;
    }

    if (!source_from_name(part, &b))
        return false;
    *out = b;
    return true;
}

void binding_text(Binding b, char *out, u32 cap) {
    ASSERT(out && cap);

    i32 n = 0;
    for (u32 m = 0; m < MODIFIER_COUNT; m++)
        if (b.modifiers & MODIFIER_BITS[m])
            n += snprintf(out + n, cap - (u32)n, "%s+", MODIFIER_NAMES[m]);

    const char *name = b.is_button ? window::button_name((window::Button)b.code)
                                   : window::key_name((window::Key)b.code);
    snprintf(out + n, cap - (u32)n, "%s", name);
}

static b32 binding_down(window::Window *w, Binding b) {
    for (u32 m = 0; m < MODIFIER_COUNT; m++) {
        if (!(b.modifiers & MODIFIER_BITS[m]))
            continue;
        if (!window::key_down(w, MODIFIER_KEYS[m][0]) && !window::key_down(w, MODIFIER_KEYS[m][1]))
            return false;
    }
    return b.is_button ? window::button_down(w, (window::Button)b.code)
                       : window::key_down(w, (window::Key)b.code);
}

static u32 lookup(strmap::Map *by_name, const char *name) {
    if (!name)
        return 0;

    u32 *v = strmap::find(by_name, name);
    return v ? *v : 0;
}

// The action behind an id, or NULL when the id is dead or its map is off.
static const ActionDef *def(const State *s, Action a) {
    if (!s || !a.v || a.v > s->action_count)
        return NULL;

    const ActionDef *ad = &s->actions[a.v];
    return s->maps[ad->map].enabled ? ad : NULL;
}

static void read_map(State *s, memory::Arena *a, config::Node *mn, const char *path) {
    const char *mname = config::value(mn, 0, NULL);
    if (!mname) {
        LOG_WARN("input: %s: a map with no name is ignored", path);
        return;
    }

    if (lookup(&s->by_map, mname)) {
        LOG_WARN("input: %s: map '%s' is declared twice; the second is ignored", path, mname);
        return;
    }

    if (s->map_count == s->max_maps) {
        LOG_WARN("input: %s: more than %u maps across the files read; '%s' is ignored", path,
                 s->max_maps, mname);
        return;
    }

    u32 mid = ++s->map_count;
    MapDef *m = &s->maps[mid];
    m->name = memory::push_str(a, mname);
    m->enabled = true;
    m->first_action = s->action_count + 1;
    strmap::insert(&s->by_map, m->name, mid);

    for (config::Node *an = mn->first_child; an; an = an->next_sibling) {
        if (!an->key) // a comment or a blank line
            continue;

        if (lookup(&s->by_action, an->key)) {
            LOG_WARN("input: %s: action '%s' is declared twice; the second is ignored", path,
                     an->key);
            continue;
        }

        if (s->action_count == s->max_actions) {
            LOG_WARN("input: %s: more than %u actions across the files read; '%s' is ignored",
                     path, s->max_actions, an->key);
            continue;
        }

        u32 aid = ++s->action_count;
        ActionDef *ad = &s->actions[aid];
        ad->name = memory::push_str(a, an->key);
        ad->map = mid;
        m->count++;
        strmap::insert(&s->by_action, ad->name, aid);

        for (u32 i = 0; i < an->value_count; i++) {
            Binding src;
            if (!parse_binding(an->values[i], &src)) {
                LOG_WARN("input: %s: '%s' is not a key, a button or a chord, in action '%s'",
                         path, an->values[i], ad->name);
                continue;
            }

            if (ad->binding_count == INPUT_MAX_BINDINGS) {
                LOG_WARN("input: %s: action '%s' takes at most %d bindings; '%s' is ignored", path,
                         ad->name, INPUT_MAX_BINDINGS, an->values[i]);
                break;
            }

            ad->bindings[ad->binding_count++] = src;
        }

        // it still exists, so the editor shows it as unbound rather than losing it
        if (!ad->binding_count)
            LOG_WARN("input: %s: action '%s' has no usable binding", path, ad->name);
    }
}

// every map block of one file into the state; the file is infrastructure
static result::Error read_file(State *s, memory::Arena *a, memory::Arena *scratch,
                               const char *path) {
    memory::ArenaTemp t = memory::temp_begin(scratch);

    config::DocResult dr = config::load(scratch, path);
    if (dr.err != result::OK) {
        LOG_ERR("input: cannot read %s", path);
        memory::temp_end(t);
        return dr.err;
    }

    config::Doc *doc = dr.val;
    if (doc->problems) {
        LOG_ERR("input: %s has %u structural problem(s)", path, doc->problems);
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    u32 before = s->map_count;
    for (config::Node *mn = config::find(doc->first, MAP_KEY); mn;
         mn = config::find_next(mn, MAP_KEY))
        read_map(s, a, mn, path);

    memory::temp_end(t);

    if (s->map_count == before) {
        LOG_ERR("input: %s declares no maps", path);
        return result::ERR_PARSE;
    }
    return result::OK;
}

StateResult init(memory::Arena *a, memory::Arena *scratch, const char *path, u32 max_maps,
                 u32 max_actions) {
    ASSERT(a && scratch && path);
    ASSERT_MSG(a != scratch, "input: the state cannot live in the arena it is parsed from");
    ASSERT_MSG(max_maps && max_actions, "input: zero capacity");

    State *s = push_struct(a, State);
    s->max_maps = max_maps;
    s->max_actions = max_actions;

    // index 0 in both tables is the reserved "nothing" id
    s->maps = push_array(a, MapDef, max_maps + 1);
    s->actions = push_array(a, ActionDef, max_actions + 1);

    s->by_map = strmap::create(a, max_maps);
    s->by_action = strmap::create(a, max_actions);

    // no delta across the first frame: the cursor has not moved, we just looked
    s->warp = true;

    result::Error e = read_file(s, a, scratch, path);
    if (e != result::OK)
        return StateResult{.err = e};

    LOG_INFO("input: %u action(s) in %u map(s) from %s", s->action_count, s->map_count, path);
    return StateResult{.val = s};
}

result::Error add_maps(State *s, memory::Arena *a, memory::Arena *scratch, const char *path) {
    ASSERT(s && a && scratch && path);

    u32 maps_before = s->map_count;
    u32 actions_before = s->action_count;
    result::Error e = read_file(s, a, scratch, path);
    if (e != result::OK)
        return e;

    LOG_INFO("input: %u action(s) in %u map(s) added from %s", s->action_count - actions_before,
             s->map_count - maps_before, path);
    return result::OK;
}

Mark mark(const State *s) {
    ASSERT(s);
    return Mark{s->map_count, s->action_count};
}

void rewind(State *s, Mark m) {
    ASSERT(s && m.maps <= s->map_count && m.actions <= s->action_count);

    for (u32 i = m.actions + 1; i <= s->action_count; i++) {
        strmap::remove(&s->by_action, s->actions[i].name);
        s->actions[i] = ActionDef{};
    }
    for (u32 i = m.maps + 1; i <= s->map_count; i++) {
        strmap::remove(&s->by_map, s->maps[i].name);
        s->maps[i] = MapDef{};
    }

    if (s->map_count != m.maps)
        LOG_INFO("input: %u map(s) with %u action(s) dropped", s->map_count - m.maps,
                 s->action_count - m.actions);
    s->map_count = m.maps;
    s->action_count = m.actions;
}

void update(State *s, window::Window *w) {
    ASSERT(s && w);

    for (u32 mi = 1; mi <= s->map_count; mi++) {
        MapDef *m = &s->maps[mi];

        for (u32 i = 0; i < m->count; i++) {
            ActionDef *ad = &s->actions[m->first_action + i];

            if (!m->enabled) {
                ad->down = 0;
                ad->prev = 0;
                ad->latched = 0;
                continue;
            }

            b8 now = 0;
            for (u32 b = 0; b < ad->binding_count && !now; b++)
                now = binding_down(w, ad->bindings[b]) ? 1 : 0;

            // on the first update after the map came back, whatever is held was
            // held before us: adopt it rather than calling it a press
            ad->prev = m->resync ? now : ad->down;
            ad->down = now;

            if (now && !ad->prev)
                ad->latched = 1;
        }

        m->resync = false;
    }

    f32 cx = 0.0f;
    f32 cy = 0.0f;
    window::cursor_position(w, &cx, &cy);

    linalg::Vec2 p{cx, cy};
    s->delta = s->warp ? linalg::Vec2{0.0f, 0.0f} : linalg::vec2_sub(p, s->cursor);
    s->cursor = p;
    s->warp = false;

    s->scroll = window::take_scroll(w);

    window::take_text(w, s->text, sizeof(s->text));

    memset(s->key_pressed, 0, sizeof(s->key_pressed));
    window::Key presses[WINDOW_KEY_EVENT_CAP];
    u32 n = window::take_key_presses(w, presses, ARRAY_COUNT(presses));
    for (u32 i = 0; i < n; i++)
        s->key_pressed[presses[i]] = 1;

    for (u32 k = 0; k < window::KEY_COUNT; k++)
        s->key_down[k] = window::key_down(w, (window::Key)k) ? 1 : 0;

    for (u32 b = 0; b < window::BUTTON_COUNT; b++) {
        s->button_prev[b] = s->button_down[b];
        s->button_down[b] = window::button_down(w, (window::Button)b) ? 1 : 0;
    }
}

Action find(State *s, const char *name) {
    ASSERT(s);

    u32 i = lookup(&s->by_action, name);
    if (!i) {
        LOG_WARN("input: no map declares an action named '%s'", name ? name : "");
        return ACTION_NONE;
    }

    return Action{i};
}

Map find_map(State *s, const char *name) {
    ASSERT(s);

    u32 i = lookup(&s->by_map, name);
    if (!i) {
        LOG_WARN("input: no file declares a map named '%s'", name ? name : "");
        return MAP_NONE;
    }

    return Map{i};
}

b32 down(const State *s, Action a) {
    const ActionDef *ad = def(s, a);
    return ad && ad->down;
}

b32 pressed(const State *s, Action a) {
    const ActionDef *ad = def(s, a);
    return ad && ad->down && !ad->prev;
}

b32 released(const State *s, Action a) {
    const ActionDef *ad = def(s, a);
    return ad && !ad->down && ad->prev;
}

b32 consume_pressed(State *s, Action a) {
    if (!def(s, a))
        return false;

    ActionDef *ad = &s->actions[a.v];
    if (!ad->latched)
        return false;

    ad->latched = 0;
    return true;
}

void set_map_enabled(State *s, Map m, b32 on) {
    ASSERT(s);

    if (!m.v || m.v > s->map_count)
        return;

    MapDef *md = &s->maps[m.v];
    if ((md->enabled != 0) == (on != 0))
        return;

    md->enabled = on ? 1 : 0;
    md->resync = on ? 1 : 0;

    // nothing in flight survives the switch off
    if (!on)
        for (u32 i = 0; i < md->count; i++) {
            ActionDef *ad = &s->actions[md->first_action + i];
            ad->down = 0;
            ad->prev = 0;
            ad->latched = 0;
        }
}

b32 map_enabled(const State *s, Map m) {
    ASSERT(s);
    return m.v && m.v <= s->map_count && s->maps[m.v].enabled;
}

u32 map_count(const State *s) {
    ASSERT(s);
    return s->map_count;
}

Map map_at(const State *s, u32 i) {
    ASSERT(s);
    return i < s->map_count ? Map{i + 1} : MAP_NONE;
}

const char *map_name(const State *s, Map m) {
    ASSERT(s);
    return m.v && m.v <= s->map_count ? s->maps[m.v].name : "";
}

u32 action_count(const State *s, Map m) {
    ASSERT(s);
    return m.v && m.v <= s->map_count ? s->maps[m.v].count : 0;
}

Action action_at(const State *s, Map m, u32 i) {
    ASSERT(s);

    if (!m.v || m.v > s->map_count || i >= s->maps[m.v].count)
        return ACTION_NONE;

    return Action{s->maps[m.v].first_action + i};
}

const char *action_name(const State *s, Action a) {
    ASSERT(s);
    return a.v && a.v <= s->action_count ? s->actions[a.v].name : "";
}

u32 binding_count(const State *s, Action a) {
    ASSERT(s);
    return a.v && a.v <= s->action_count ? s->actions[a.v].binding_count : 0;
}

Binding binding_at(const State *s, Action a, u32 i) {
    ASSERT(s);

    if (!a.v || a.v > s->action_count || i >= s->actions[a.v].binding_count)
        return Binding{};

    return s->actions[a.v].bindings[i];
}

linalg::Vec2 cursor(const State *s) {
    ASSERT(s);
    return s->cursor;
}

linalg::Vec2 cursor_delta(const State *s) {
    ASSERT(s);
    return s->delta;
}

f32 scroll(const State *s) {
    ASSERT(s);
    return s->scroll;
}

const char *text(const State *s) {
    ASSERT(s);
    return s->text;
}

b32 key_pressed(const State *s, window::Key k) {
    ASSERT(s && k < window::KEY_COUNT);
    return s->key_pressed[k];
}

b32 key_down(const State *s, window::Key k) {
    ASSERT(s && k < window::KEY_COUNT);
    return s->key_down[k];
}

b32 button_down(const State *s, window::Button b) {
    ASSERT(s && b < window::BUTTON_COUNT);
    return s->button_down[b];
}

b32 button_pressed(const State *s, window::Button b) {
    ASSERT(s && b < window::BUTTON_COUNT);
    return s->button_down[b] && !s->button_prev[b];
}

b32 button_released(const State *s, window::Button b) {
    ASSERT(s && b < window::BUTTON_COUNT);
    return !s->button_down[b] && s->button_prev[b];
}

void set_mouse_look(State *s, window::Window *w, b32 on) {
    ASSERT(s && w);

    if ((s->mouse_look != 0) != (on != 0))
        s->warp = true;

    s->mouse_look = on ? 1 : 0;
    window::set_cursor_captured(w, on);
}

b32 mouse_look(const State *s) {
    ASSERT(s);
    return s->mouse_look;
}

} // namespace input
