#include "gfx/drawkey.h"

#include "suites.h"

static void sort_entries(memory::Arena *a, drawkey::Entry *e, u32 n) {
    drawkey::Entry *tmp = push_array(a, drawkey::Entry, n);
    drawkey::sort(e, tmp, n);
}

static void transparent_sorts_far_to_near(memory::Arena *a) {
    drawkey::Entry e[] = {
        {drawkey::transparent(0, 0, 1.0f), 1},
        {drawkey::transparent(0, 0, 10.0f), 10},
        {drawkey::transparent(0, 0, 5.0f), 5},
        {drawkey::transparent(0, 0, 0.0f), 0}, // on the camera: drawn last
    };
    sort_entries(a, e, ARRAY_COUNT(e));

    CHECK(e[0].item == 10 && e[1].item == 5 && e[2].item == 1 && e[3].item == 0);
}

static void opaque_groups_program_then_state_then_depth(memory::Arena *a) {
    drawkey::Entry e[] = {
        {drawkey::opaque(1, 0, 0, 0, 1.0f), 4}, // other program: last
        {drawkey::opaque(0, 5, 0, 0, 1.0f), 3}, // same program, later state
        {drawkey::opaque(0, 1, 0, 0, 7.0f), 2}, // same state, further
        {drawkey::opaque(0, 1, 0, 0, 3.0f), 1}, // same state, nearer: first
    };
    sort_entries(a, e, ARRAY_COUNT(e));

    CHECK(e[0].item == 1 && e[1].item == 2 && e[2].item == 3 && e[3].item == 4);
}

static void opaque_depth_only_breaks_ties(memory::Arena *a) {
    // a nearer draw with a different mesh must not jump ahead of the mesh group
    drawkey::Entry e[] = {
        {drawkey::opaque(0, 0, 2, 0, 0.5f), 2},
        {drawkey::opaque(0, 0, 1, 0, 200.0f), 1},
    };
    sort_entries(a, e, ARRAY_COUNT(e));
    CHECK(e[0].item == 1 && e[1].item == 2);

    // and depth saturates rather than wrapping
    CHECK(drawkey::opaque(0, 0, 0, 0, 1e9f) > drawkey::opaque(0, 0, 0, 0, 1.0f));
    CHECK(drawkey::opaque(0, 0, 0, 0, 1e9f) == drawkey::opaque(0, 0, 0, 0, 1e6f));
}

static void radix_sort_is_stable_and_complete(memory::Arena *a) {
    const u32 n = 1000;
    drawkey::Entry *e = push_array(a, drawkey::Entry, n);

    // keys touch every byte so no pass is skipped, and repeat so stability shows
    for (u32 i = 0; i < n; i++) {
        u64 k = (u64)((i * 7919u) % 97u);
        e[i].key = (k << 56) | (k << 24) | k;
        e[i].item = i;
    }
    sort_entries(a, e, n);

    b32 ordered = true;
    b32 stable = true;
    for (u32 i = 1; i < n; i++) {
        ordered = ordered && e[i - 1].key <= e[i].key;
        if (e[i - 1].key == e[i].key)
            stable = stable && e[i - 1].item < e[i].item;
    }
    CHECK(ordered);
    CHECK(stable);

    // every item exactly once
    u32 seen[n] = {};
    for (u32 i = 0; i < n; i++)
        seen[e[i].item]++;
    b32 complete = true;
    for (u32 i = 0; i < n; i++)
        complete = complete && seen[i] == 1;
    CHECK(complete);
}

static void tiny_inputs_are_no_ops(memory::Arena *a) {
    drawkey::Entry one[] = {{42, 7}};
    sort_entries(a, one, 1);
    CHECK(one[0].key == 42 && one[0].item == 7);

    drawkey::Entry *none = push_array(a, drawkey::Entry, 1);
    sort_entries(a, none, 0);
}

test::Suite drawkey_suite() {
    static const test::Case cases[] = {
        CASE(transparent_sorts_far_to_near), CASE(opaque_groups_program_then_state_then_depth),
        CASE(opaque_depth_only_breaks_ties), CASE(radix_sort_is_stable_and_complete),
        CASE(tiny_inputs_are_no_ops),
    };
    return test::Suite{.name = "drawkey", .cases = cases, .count = ARRAY_COUNT(cases)};
}
