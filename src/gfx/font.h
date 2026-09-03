#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/result.h"
#include "gfx/texture.h"

// A TrueType font baked at one pixel size into a coverage atlas, plus what a
// layout needs to place its glyphs. Baking is CPU-only, through stb_truetype
// allocating from the scratch arena; the atlas reaches the GPU on first use, so
// a font can be loaded and measured without a context.
//
// Two rows at the top of the atlas are solid white, so a solid fill can sample
// this texture and share a draw with the text beside it.
//
// One codepoint range is baked. Control characters have no glyph; anything else
// outside the range draws the fallback and warns once.

namespace font {

struct Glyph {
    f32 u0, v0, u1, v1; // atlas, normalized
    f32 x0, y0, x1, y1; // quad corners from the pen, y down; equal when blank
    f32 advance;        // pixels to the next pen position
};

struct Font {
    f32 size;        // the pixel height asked for
    f32 ascent;      // pixels above the baseline
    f32 descent;     // pixels below the baseline, positive
    f32 line_height; // baseline to baseline

    u32 first; // codepoint of glyphs[0]
    u32 count;
    Glyph *glyphs;
    u32 fallback; // index drawn for a codepoint outside the range

    u8 *pixels; // atlas x atlas coverage
    u32 atlas;  // width and height
    f32 white_u, white_v;

    texture::Texture texture; // gl 0 until texture() is asked for
};

struct Config {
    const char *file; // under the asset root; NULL when the block names none
    f32 size;
    u32 atlas;
    u32 oversample; // horizontal and vertical; 1 is off
    u32 first;
    u32 count;
};

// The `font` block of engine.cfg; NULL or missing keys take the defaults.
Config config_from(config::Node *block);

RESULT_TYPE(Font *, FontResult);

// Bakes into a, working in scratch; the two must differ. A file that is
// missing, is not a font, or whose range does not fit the atlas is an error.
FontResult load(memory::Arena *a, memory::Arena *scratch, const char *root, Config cfg);
void destroy(Font *f);

// Uploads on the first call. Needs a context.
const texture::Texture *texture(Font *f);

// NULL for a control character; the fallback, with one warning, outside the range.
const Glyph *glyph(const Font *f, u32 codepoint);

// The advance of a string, or of its first `bytes`, in pixels.
f32 width(const Font *f, const char *text);
f32 width(const Font *f, const char *text, u32 bytes);

} // namespace font
