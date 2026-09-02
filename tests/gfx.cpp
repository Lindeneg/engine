#include "gfx.h"
#include "suites.h"

// every combination must have its own key, and fit the 6 bits the draw key
// gives it
static void state_keys_are_distinct_and_small(memory::Arena *) {
    u32 seen[64] = {};
    u32 count = 0;

    for (u32 c = 0; c < gfx::CULL_COUNT; c++) {
        for (u32 b = 0; b < gfx::BLEND_COUNT; b++) {
            for (u32 dt = 0; dt < 2; dt++) {
                for (u32 dw = 0; dw < 2; dw++) {
                    gfx::RenderState s{(gfx::CullMode)c, (gfx::BlendMode)b, (b8)dt, (b8)dw};
                    u32 key = gfx::state_key(s);
                    CHECK(key < 64);
                    CHECK(seen[key] == 0);
                    seen[key]++;
                    count++;
                }
            }
        }
    }
    CHECK(count == (u32)gfx::CULL_COUNT * (u32)gfx::BLEND_COUNT * 4);
}

static void names_round_trip(memory::Arena *) {
    for (u32 i = 0; i < gfx::CULL_COUNT; i++) {
        gfx::CullMode m = gfx::CULL_COUNT;
        CHECK(gfx::cull_from_name(gfx::cull_name((gfx::CullMode)i), &m));
        CHECK(m == (gfx::CullMode)i);
    }
    for (u32 i = 0; i < gfx::BLEND_COUNT; i++) {
        gfx::BlendMode m = gfx::BLEND_COUNT;
        CHECK(gfx::blend_from_name(gfx::blend_name((gfx::BlendMode)i), &m));
        CHECK(m == (gfx::BlendMode)i);
    }

    gfx::CullMode unchanged = gfx::CULL_FRONT;
    CHECK(!gfx::cull_from_name("sideways", &unchanged));
    CHECK(unchanged == gfx::CULL_FRONT);
}

static void default_state_draws_something(memory::Arena *) {
    gfx::RenderState s = gfx::state_default();
    CHECK(s.depth_test && s.depth_write);
    CHECK(s.blend == gfx::BLEND_NONE);
    CHECK(s.cull == gfx::CULL_BACK);
}

test::Suite gfx_suite() {
    static const test::Case cases[] = {
        CASE(state_keys_are_distinct_and_small),
        CASE(names_round_trip),
        CASE(default_state_draws_something),
    };
    return test::Suite{.name = "gfx", .cases = cases, .count = ARRAY_COUNT(cases)};
}
