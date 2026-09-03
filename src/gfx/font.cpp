#include "gfx/font.h"

#include <stb_truetype.h>
#include <string.h>

#include "core/logger.h"
#include "platform/file.h"
#include "platform/path.h"

#define DEFAULT_SIZE 16.0f
#define DEFAULT_ATLAS 512
#define DEFAULT_OVERSAMPLE 2
#define DEFAULT_FIRST 32 // space
#define DEFAULT_COUNT 95 // through '~'

// stb's own STBTT_MAX_OVERSAMPLE, which its header only defines for the implementation
#define MAX_OVERSAMPLE 8

// pixels between glyphs, so linear filtering never reads a neighbour
#define ATLAS_PADDING 1

// solid rows at the top, for fills that want to share the atlas
#define WHITE_ROWS 2

// the atlas must hold the white rows and at least one padded glyph
#define MIN_ATLAS 16

#define FALLBACK_CODEPOINT '?'
#define REPLACEMENT_CODEPOINT 0xFFFDu
#define CONTROL_LIMIT 0x20u

// UTF-8 lead bytes
#define UTF8_TWO 0xC0u
#define UTF8_THREE 0xE0u
#define UTF8_FOUR 0xF0u
#define UTF8_CONTINUATION 0x80u
#define UTF8_CONTINUATION_MASK 0xC0u

namespace font {

Config config_from(config::Node *block) {
    config::Node *body = block ? block->first_child : NULL;
    return Config{
        .file = config::get(body, "file", NULL),
        .size = config::get_f32(body, "size", DEFAULT_SIZE),
        .atlas = (u32)config::get_i32(body, "atlas", DEFAULT_ATLAS),
        .oversample = (u32)config::get_i32(body, "oversample", DEFAULT_OVERSAMPLE),
        .first = (u32)config::get_i32(body, "first", DEFAULT_FIRST),
        .count = (u32)config::get_i32(body, "count", DEFAULT_COUNT),
    };
}

FontResult load(memory::Arena *a, memory::Arena *scratch, const char *root, Config cfg) {
    ASSERT(a && scratch && root);
    ASSERT_MSG(a != scratch, "font: the font cannot live in the arena it is baked from");

    if (!cfg.file) {
        LOG_ERR("font: no file is named");
        return FontResult{.err = result::ERR_INVALID};
    }
    if (cfg.size <= 0.0f || cfg.count == 0 || cfg.atlas < MIN_ATLAS) {
        LOG_ERR("font: %s: size %.1f, %u glyphs, atlas %u is not a bakeable request", cfg.file,
                (f64)cfg.size, cfg.count, cfg.atlas);
        return FontResult{.err = result::ERR_INVALID};
    }
    if (cfg.oversample < 1 || cfg.oversample > MAX_OVERSAMPLE) {
        u32 clamped = cfg.oversample < 1 ? 1 : MAX_OVERSAMPLE;
        LOG_WARN("font: %s: oversample %u is outside 1..%d, using %u", cfg.file, cfg.oversample,
                 MAX_OVERSAMPLE, clamped);
        cfg.oversample = clamped;
    }

    memory::ArenaTemp t = memory::temp_begin(scratch);

    file::BlobResult blob = file::read(scratch, FPATH(scratch, root, cfg.file));
    if (blob.err != result::OK) {
        memory::temp_end(t);
        return FontResult{.err = blob.err};
    }

    // the magic is checked first: InitFont trusts its offset and would read a
    // text file's bytes as table counts
    i32 offset = stbtt_GetFontOffsetForIndex(blob.val.data, 0);
    stbtt_fontinfo info{};
    info.userdata = scratch;
    if (offset < 0 || !stbtt_InitFont(&info, blob.val.data, offset)) {
        LOG_ERR("font: %s is not a TrueType font", cfg.file);
        memory::temp_end(t);
        return FontResult{.err = result::ERR_PARSE};
    }

    Font *f = push_struct(a, Font);
    f->size = cfg.size;
    f->first = cfg.first;
    f->count = cfg.count;
    f->atlas = cfg.atlas;
    f->pixels = push_array(a, u8, (u64)cfg.atlas * cfg.atlas);

    memset(f->pixels, 0xFF, (u64)WHITE_ROWS * cfg.atlas);
    f->white_u = 0.5f / (f32)cfg.atlas;
    f->white_v = 0.5f / (f32)cfg.atlas;

    // glyphs pack below the white rows; stb sees a shorter atlas with the full stride
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, f->pixels + (u64)WHITE_ROWS * cfg.atlas, (i32)cfg.atlas,
                         (i32)(cfg.atlas - WHITE_ROWS), (i32)cfg.atlas, ATLAS_PADDING, scratch)) {
        LOG_ERR("font: %s: the packer would not start", cfg.file);
        memory::temp_end(t);
        return FontResult{.err = result::ERR_INVALID};
    }
    stbtt_PackSetOversampling(&pc, cfg.oversample, cfg.oversample);

    stbtt_packedchar *packed = push_array(scratch, stbtt_packedchar, cfg.count);
    i32 fits = stbtt_PackFontRange(&pc, blob.val.data, 0, cfg.size, (i32)cfg.first, (i32)cfg.count,
                                   packed);
    stbtt_PackEnd(&pc);

    if (!fits) {
        LOG_ERR("font: %s: %u glyphs at %.1f px (x%u) do not fit a %u x %u atlas", cfg.file,
                cfg.count, (f64)cfg.size, cfg.oversample, cfg.atlas, cfg.atlas);
        memory::temp_end(t);
        return FontResult{.err = result::ERR_EXHAUSTED};
    }

    f32 scale = stbtt_ScaleForPixelHeight(&info, cfg.size);
    i32 ascent = 0;
    i32 descent = 0;
    i32 gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    f->ascent = (f32)ascent * scale;
    f->descent = -(f32)descent * scale;
    f->line_height = (f32)(ascent - descent + gap) * scale;

    f->glyphs = push_array(a, Glyph, cfg.count);
    for (u32 i = 0; i < cfg.count; i++) {
        // the packer's rows start under the white ones
        packed[i].y0 = (u16)(packed[i].y0 + WHITE_ROWS);
        packed[i].y1 = (u16)(packed[i].y1 + WHITE_ROWS);

        f32 x = 0.0f;
        f32 y = 0.0f;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(packed, (i32)cfg.atlas, (i32)cfg.atlas, (i32)i, &x, &y, &q, 0);

        f->glyphs[i] = Glyph{q.s0, q.t0, q.s1, q.t1, q.x0, q.y0, q.x1, q.y1, x};
    }

    b32 has_fallback =
        FALLBACK_CODEPOINT >= cfg.first && (u32)FALLBACK_CODEPOINT < cfg.first + cfg.count;
    f->fallback = has_fallback ? (u32)FALLBACK_CODEPOINT - cfg.first : 0;

    memory::temp_end(t);

    LOG_INFO("font: %s at %.1f px, %u glyphs in a %u x %u atlas, line height %.1f", cfg.file,
             (f64)cfg.size, cfg.count, cfg.atlas, cfg.atlas, (f64)f->line_height);
    return FontResult{.val = f};
}

void destroy(Font *f) {
    ASSERT(f);
    if (f->texture.gl)
        texture::destroy(&f->texture);
}

const texture::Texture *texture(Font *f) {
    ASSERT(f);
    if (!f->texture.gl)
        f->texture = texture::create(f->atlas, f->atlas, texture::FORMAT_COVERAGE,
                                     texture::FILTER_LINEAR, f->pixels);
    return &f->texture;
}

const Glyph *glyph(const Font *f, u32 codepoint) {
    ASSERT(f);

    if (codepoint < CONTROL_LIMIT)
        return NULL;
    if (codepoint >= f->first && codepoint < f->first + f->count)
        return &f->glyphs[codepoint - f->first];

    LOG_WARN_ONCE("font: U+%04X is outside the baked range %u..%u; drawing the fallback glyph",
                  codepoint, f->first, f->first + f->count - 1);
    return &f->glyphs[f->fallback];
}

u32 next_codepoint(const char **s) {
    ASSERT(s && *s);

    u8 c = (u8) * *s;
    if (!c)
        return 0;
    (*s)++;

    u32 extra = 0;
    u32 cp = c;
    if (c >= UTF8_FOUR) {
        extra = 3;
        cp = c & 0x07u;
    } else if (c >= UTF8_THREE) {
        extra = 2;
        cp = c & 0x0Fu;
    } else if (c >= UTF8_TWO) {
        extra = 1;
        cp = c & 0x1Fu;
    } else if (c >= UTF8_CONTINUATION) {
        return REPLACEMENT_CODEPOINT; // a stray continuation byte
    }

    for (u32 i = 0; i < extra; i++) {
        u8 cc = (u8) * *s;
        if ((cc & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION)
            return REPLACEMENT_CODEPOINT; // truncated; the offending byte is read next
        (*s)++;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    return cp;
}

f32 width(const Font *f, const char *text) {
    ASSERT(f && text);

    f32 w = 0.0f;
    for (u32 cp = next_codepoint(&text); cp; cp = next_codepoint(&text)) {
        const Glyph *g = glyph(f, cp);
        if (g)
            w += g->advance;
    }
    return w;
}

} // namespace font
