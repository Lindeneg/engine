// the one translation unit that compiles stb_truetype

#include "core/memory.h" // IWYU pragma: keep

// every allocation lands in the arena passed as the pack context, and dies with
// its temp scope; there is nothing to free
#define STBTT_malloc(x, u) memory::push((memory::Arena *)(u), (size_t)(x))
#define STBTT_free(x, u) ((void)(x), (void)(u))

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
