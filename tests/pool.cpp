#include <string.h>

#include "logger.h"
#include "pool.h"
#include "suites.h"

struct Item {
    u32 id;
    f32 value;
};

static pool::Pool make(memory::Arena *a, u32 cap) {
    return pool::create(a, cap, sizeof(Item));
}

// index 0 is what makes handle 0 mean "nothing" everywhere, and what lets
// alloc report a full pool by returning 0
static void slot_zero_is_never_handed_out(memory::Arena *a) {
    pool::Pool p = make(a, 4);

    for (u32 i = 0; i < 3; i++)
        CHECK(pool::alloc(&p, 0) != 0);

    CHECK(pool::alloc(&p, 0) == 0); // cap 4 minus the reserved slot
    CHECK(pool::alive(&p, 0) == false);
}

static void handles_resolve_to_their_payload(memory::Arena *a) {
    pool::Pool p = make(a, 8);

    u32 i1 = pool::alloc(&p, 0);
    u32 i2 = pool::alloc(&p, 0);
    ((Item *)pool::at(&p, i1))->id = 111;
    ((Item *)pool::at(&p, i2))->id = 222;

    u32 h1 = pool::make_handle(&p, i1);
    u32 h2 = pool::make_handle(&p, i2);
    CHECK(h1 != h2);

    CHECK(((Item *)pool::resolve(&p, h1))->id == 111);
    CHECK(((Item *)pool::resolve(&p, h2))->id == 222);
}

static void alloc_zeroes_the_payload(memory::Arena *a) {
    pool::Pool p = make(a, 4);

    u32 i = pool::alloc(&p, 0);
    ((Item *)pool::at(&p, i))->id = 0xABCD;
    pool::release(&p, i);

    u32 again = pool::alloc(&p, 0);
    CHECK(again == i); // same slot back
    CHECK(((Item *)pool::at(&p, again))->id == 0);
}

// the whole reason slots carry a generation
static void recycled_slots_reject_stale_handles(memory::Arena *a) {
    pool::Pool p = make(a, 4);

    u32 idx = pool::alloc(&p, 0);
    u32 old = pool::make_handle(&p, idx);
    pool::release(&p, idx);

    u32 fresh = pool::make_handle(&p, pool::alloc(&p, 0));

    CHECK(pool_handle_index(old) == pool_handle_index(fresh));
    CHECK(pool_handle_epoch(old) != pool_handle_epoch(fresh));

    CHECK(pool::alive(&p, fresh) == true);
    CHECK(pool::alive(&p, old) == false);
    CHECK(pool::resolve(&p, old) == NULL);
}

static void scopes_release_independently(memory::Arena *a) {
    pool::Pool p = make(a, 8);

    u32 keep = pool::make_handle(&p, pool::alloc(&p, 1));
    u32 drop = pool::make_handle(&p, pool::alloc(&p, 2));

    pool::release_scope(&p, 2);

    CHECK(pool::alive(&p, keep) == true);
    CHECK(pool::alive(&p, drop) == false);
}

// asset:: wants a drawable fallback; entity:: wants NULL. Both come from here.
static void resolve_or_zero_substitutes_and_warns_once(memory::Arena *a) {
    pool::Pool p = make(a, 4);
    ((Item *)pool::at(&p, 0))->id = 999;

    u32 idx = pool::alloc(&p, 0);
    u32 stale = pool::make_handle(&p, idx);
    pool::release(&p, idx);

    logger::set_level(logger::FATAL); // the first lookup warns, by design
    CHECK(((Item *)pool::resolve_or_zero(&p, stale, "item"))->id == 999);
    CHECK(p.slots[pool_handle_index(stale)].warned == true);

    // handle 0 means "unset" rather than "broken", so it must stay silent
    pool::Pool q = make(a, 4);
    CHECK(pool::resolve_or_zero(&q, 0, "item") == pool::at(&q, 0));
    CHECK(q.slots[0].warned == false);
    logger::set_level(logger::TRACE);
}

static void iteration_visits_live_slots_only(memory::Arena *a) {
    pool::Pool p = make(a, 8);

    u32 idx[4];
    for (u32 i = 0; i < 4; i++) {
        idx[i] = pool::alloc(&p, 0);
        ((Item *)pool::at(&p, idx[i]))->id = i;
    }
    pool::release(&p, idx[1]);

    u32 seen = 0;
    u32 sum = 0;
    for (u32 i = pool::first(&p); i; i = pool::next(&p, i)) {
        seen++;
        sum += ((Item *)pool::at(&p, i))->id;
    }

    CHECK(seen == 3);
    CHECK(sum == 0 + 2 + 3);
    CHECK(p.live == 3);

    // an empty pool terminates immediately rather than spinning
    pool::Pool empty = make(a, 4);
    CHECK(pool::first(&empty) == 0);
}

test::Suite pool_suite() {
    static const test::Case cases[] = {
        {"slot_zero_is_never_handed_out", slot_zero_is_never_handed_out},
        {"handles_resolve_to_their_payload", handles_resolve_to_their_payload},
        {"alloc_zeroes_the_payload", alloc_zeroes_the_payload},
        {"recycled_slots_reject_stale_handles", recycled_slots_reject_stale_handles},
        {"scopes_release_independently", scopes_release_independently},
        {"resolve_or_zero_substitutes_and_warns_once", resolve_or_zero_substitutes_and_warns_once},
        {"iteration_visits_live_slots_only", iteration_visits_live_slots_only},
    };
    return test::Suite{.name = "pool", .cases = cases, .count = ARRAY_COUNT(cases)};
}
