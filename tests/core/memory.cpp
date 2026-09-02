#include <string.h>

#include "core/memory.h"
#include "platform/platform.h"
#include "suites.h"

static void push_respects_alignment(memory::Arena *a) {
    u8 *p1 = (u8 *)memory::push(a, 1, 1);
    u8 *p2 = (u8 *)memory::push(a, 8, 64);
    u8 *p3 = (u8 *)memory::push(a, 8); // ARENA_DEFAULT_ALIGN

    CHECK(((uintptr_t)p2 % 64) == 0);
    CHECK(((uintptr_t)p3 % ARENA_DEFAULT_ALIGN) == 0);
    CHECK(p2 > p1 && p3 > p2);
    CHECK(a->used >= 1 + 8 + 8);
}

static void push_zeroes_reused_bytes(memory::Arena *a) {
    memory::ArenaTemp t = memory::temp_begin(a);
    u8 *p = push_array(a, u8, 256);
    memset(p, 0xAB, 256);
    memory::temp_end(t);

    u8 *q = push_array(a, u8, 256);
    CHECK(q == p); // same bytes handed out again
    b32 clean = true;
    for (u32 i = 0; i < 256; i++)
        clean = clean && q[i] == 0;
    CHECK(clean);
}

static void temp_scopes_nest(memory::Arena *a) {
    u64 base = a->used;

    memory::ArenaTemp outer = memory::temp_begin(a);
    memory::push(a, 100);
    u64 after_first = a->used;

    memory::ArenaTemp inner = memory::temp_begin(a);
    memory::push(a, 100);
    CHECK(a->used > after_first);
    memory::temp_end(inner);
    CHECK(a->used == after_first);

    memory::temp_end(outer);
    CHECK(a->used == base);
}

static void reset_keeps_peak(memory::Arena *) {
    memory::Arena ar = memory::create(KiB(64));
    memory::push(&ar, KiB(10));
    CHECK(ar.used >= KiB(10));

    memory::reset(&ar);
    CHECK(ar.used == 0);
    CHECK(ar.peak >= KiB(10));
}

static void commit_grows_in_chunks(memory::Arena *) {
    memory::Arena ar = memory::create(MiB(1));
    CHECK(ar.committed == 0);

    memory::push(&ar, 1);
    CHECK(ar.committed == ARENA_COMMIT_CHUNK);

    memory::push(&ar, ARENA_COMMIT_CHUNK); // crosses into the second chunk
    CHECK(ar.committed == 2 * ARENA_COMMIT_CHUNK);
    CHECK(ar.committed >= ar.used);
    CHECK(ar.committed <= ar.cap);

    // the commit is never lost on reset; the next push reuses it
    memory::reset(&ar);
    memory::push(&ar, 1);
    CHECK(ar.committed == 2 * ARENA_COMMIT_CHUNK);
}

static void cap_is_page_aligned(memory::Arena *) {
    memory::Arena ar = memory::create(KiB(1));
    CHECK(ar.cap >= KiB(1));
    CHECK(ar.cap % platform::page_size() == 0);
}

static void string_helpers_copy(memory::Arena *a) {
    const char *src = "characters/soldier.glb";
    char *s = memory::push_str(a, src);
    CHECK(s != src);
    CHECK(strcmp(s, src) == 0);

    u8 bytes[] = {1, 2, 3, 4};
    u8 *b = (u8 *)memory::push_bytes(a, bytes, sizeof(bytes));
    CHECK(b != bytes && memcmp(b, bytes, sizeof(bytes)) == 0);

    char *f = memory::push_fmt(a, "%s@%u", "clip.glb", 7u);
    CHECK(strcmp(f, "clip.glb@7") == 0);
    CHECK(strcmp(memory::push_fmt(a, "%s", ""), "") == 0);
}

test::Suite memory_suite() {
    static const test::Case cases[] = {
        CASE(push_respects_alignment),
        CASE(push_zeroes_reused_bytes),
        CASE(temp_scopes_nest),
        CASE(reset_keeps_peak),
        CASE(commit_grows_in_chunks),
        CASE(cap_is_page_aligned),
        CASE(string_helpers_copy),
    };
    return test::Suite{.name = "memory", .cases = cases, .count = ARRAY_COUNT(cases)};
}
