#include "strmap.h"

#include <string.h>

static u64 fnv1a(const char *s) {
    u64 h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (u8)*s++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static u64 next_pow2(u64 v) {
    if (v < 8)
        return 8;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

namespace strmap {

// returns the matching slot, or the first empty one if the key is absent
static u64 probe(const Map *m, const char *key, u64 hash) {
    u64 i = hash & m->mask;
    while (m->slots[i].key) {
        if (m->slots[i].hash == hash && strcmp(m->slots[i].key, key) == 0)
            break;
        i = (i + 1) & m->mask;
    }
    return i;
}

Map create(memory::Arena *a, u64 max_entries) {
    u64 cap = next_pow2((max_entries * 10) / 7 + 1);
    return Map{
        .slots = push_array(a, Slot, cap),
        .cap = cap,
        .mask = cap - 1,
    };
}

b32 insert(Map *m, const char *key, u32 value) {
    ASSERT(m && key);

    u64 hash = fnv1a(key);
    u64 i = probe(m, key, hash);

    if (m->slots[i].key) {
        m->slots[i].value = value;
        return false;
    }

    ASSERT_MSG((m->count + 1) * 10 <= m->cap * 7, "strmap full: %llu of %llu slots",
               (unsigned long long)m->count, (unsigned long long)m->cap);

    m->slots[i] = Slot{.key = key, .hash = hash, .value = value};
    m->count++;
    return true;
}

u32 *find(Map *m, const char *key) {
    ASSERT(m && key);

    u64 i = probe(m, key, fnv1a(key));
    return m->slots[i].key ? &m->slots[i].value : NULL;
}

b32 remove(Map *m, const char *key) {
    ASSERT(m && key);

    u64 hole = probe(m, key, fnv1a(key));
    if (!m->slots[hole].key)
        return false;

    // Knuth 6.4 algorithm R: pull a later entry of the cluster into the hole
    // when that leaves it at or after its ideal slot, else its probe chain breaks
    for (u64 j = (hole + 1) & m->mask; m->slots[j].key; j = (j + 1) & m->mask) {
        u64 ideal = m->slots[j].hash & m->mask;
        if (((hole - ideal) & m->mask) < ((j - ideal) & m->mask)) {
            m->slots[hole] = m->slots[j];
            hole = j;
        }
    }

    m->slots[hole].key = NULL;
    m->count--;
    return true;
}

void clear(Map *m) {
    ASSERT(m);

    memset(m->slots, 0, m->cap * sizeof(Slot));
    m->count = 0;
}

} // namespace strmap
