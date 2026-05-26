// =============================================================================
// TTB (top-to-bottom) glyph metrics machinery for vertical-rl/lr rendering.
//
// Fork-only file.  No upstream counterpart.  Implements
// LVFontVertGlyphMetricsCache (declared in lvfntman_vert.h): a per-face
// cache of vertical glyph metrics read via FT_LOAD_VERTICAL_LAYOUT.
//
// Why this exists:
//   The default load flags give horizontal hmtx metrics (bitmap_top =
//   distance from baseline up to bitmap top).  For TTB layout we want
//   vmtx metrics (vertBearingX/Y = offsets from the *vertical* glyph
//   origin to the bitmap top-left).  Fonts designed primarily for TTB
//   (Hiragino, Yu Mincho) have consistent vmtx but per-glyph variable
//   hmtx — so positioning by hmtx in vertical mode produces uneven
//   inter-character spacing.  Querying vmtx fixes that.
//
//   Cache hit ratio is ~100% within a single book session (one entry
//   per glyph index per face), so the FT_Load_Glyph cost is paid once.
// =============================================================================

#include "../include/lvfntman_vert.h"
#include "../include/lvfntman.h"
#include "../include/lvdrawbuf.h"

#include <ft2build.h>
#include FT_FREETYPE_H

// Match the local macro in lvfntman.cpp (round + truncate from 26.6 fixed).
#define FONT_METRIC_TO_PX(x)    (((x)+32) >> 6)

// Returns true for characters that need explicit 90° CW rotation when
// drawn in vertical-rl mode (CSS text-orientation: mixed).
//
// CJK, Hiragana, Katakana, and Fullwidth/Halfwidth Forms are naturally
// upright (or substituted via +vert) and therefore NOT rotated.
// All other scripts (Latin, Greek, Cyrillic, digits, ASCII punctuation …)
// are "horizontal" scripts and must be laid sideways in vertical text.
bool needsVerticalRotation90CW(lChar32 c)
{
    // --- Horizontal-script characters: ROTATE ---
    if (c >= 0x0021 && c <= 0x007E) return true; // ASCII printable
    if (c >= 0x00A1 && c <= 0x024F) return true; // Latin-1 Supp + Latin Extended-A/B
    if (c >= 0x0250 && c <= 0x036F) return true; // IPA + Spacing Modifier + Diacritics
    if (c >= 0x0370 && c <= 0x03FF) return true; // Greek and Coptic
    if (c >= 0x0400 && c <= 0x04FF) return true; // Cyrillic

    // Special Japanese horizontal marks:
    switch (c) {
        case 0x30FC: // ー KATAKANA-HIRAGANA PROLONGED SOUND MARK
        case 0x301C: // 〜 WAVE DASH
        case 0xFF5E: // ～ FULLWIDTH TILDE
        case 0x2014: // — EM DASH
        case 0x2015: // ― HORIZONTAL BAR
        case 0xFF0D: // － FULLWIDTH HYPHEN-MINUS
        case 0x2025: // ‥ TWO DOT LEADER
        case 0x2026: // … HORIZONTAL ELLIPSIS
            return true;
        default:
            break;
    }

    // --- CJK / East Asian scripts: do NOT rotate ---
    if (c >= 0x2E80 && c <= 0x9FFF) return false; // CJK radicals … CJK Unified
    if (c >= 0xAC00 && c <= 0xD7A3) return false; // Hangul syllables
    if (c >= 0xF900 && c <= 0xFAFF) return false; // CJK Compat Ideographs
    if (c >= 0xFF00 && c <= 0xFFEF) return false; // Halfwidth / Fullwidth Forms
    if (c >= 0x20000)               return false; // CJK Extension B–F, etc.

    return false;
}

// Draw a glyph rotated 90° clockwise into buf.
// Used as a fallback when the font lacks a +vert OpenType substitution for
// characters that need vertical orientation (e.g. ー drawn as a horizontal
// dash must become a vertical bar).
// Only works for 8-bit grayscale glyphs (bmp_pixelformat != 4).
// The visual centre of the glyph is preserved at the original (glyph_x, glyph_y)
// position, keeping it centred in its em-square column.
void drawGlyphItemRotated90CW(LVDrawBuf * buf, int glyph_x, int glyph_y,
        LVFontGlyphCacheItem * item, const lUInt32 * palette)
{
    if (item->bmp_pixelformat == 4)
        return; // colour glyphs cannot be rotated; caller should guard against this
    int orig_w = item->bmp_width;
    int orig_h = item->bmp_height;
    if (orig_w <= 0 || orig_h <= 0)
        return;
    // After 90° CW rotation the dimensions are swapped.
    int rot_w = orig_h;
    int rot_h = orig_w;
    // Use a stack buffer for glyphs up to 64×64 px; heap otherwise.
    lUInt8 stack_buf[64 * 64];
    lUInt8 * rot = (rot_w * rot_h <= (int)sizeof(stack_buf))
                 ? stack_buf : new lUInt8[rot_w * rot_h];
    // 90° CW: dst[ny][nx] = src[orig_h - 1 - nx][ny]
    // Use bmp_pitch (row stride) for source indexing; it may exceed bmp_width.
    int src_pitch = item->bmp_pitch > 0 ? item->bmp_pitch : orig_w;
    const lUInt8 * src = item->bmp;
    for (int ny = 0; ny < rot_h; ny++) {
        for (int nx = 0; nx < rot_w; nx++) {
            rot[ny * rot_w + nx] = src[(orig_h - 1 - nx) * src_pitch + ny];
        }
    }
    // Keep the visual centre of the bitmap at the same screen position.
    int adj_x = glyph_x + (orig_w - rot_w) / 2;
    int adj_y = glyph_y + (orig_h - rot_h) / 2;
    buf->Draw(adj_x, adj_y, rot, rot_w, rot_h, palette);
    if (rot != stack_buf)
        delete[] rot;
}

bool LVFontVertGlyphMetricsCache::get(FT_Face face, lUInt32 glyph_index,
                                      VertGlyphMetrics & out)
{
    if (!face)
        return false;

    // Cache the FT_HAS_VERTICAL probe once per face.  Fonts without
    // a vhea table (DroidSansMono, Latin-only fonts, etc.) skip TTB
    // metrics entirely; the caller falls back to horizontal-metric
    // placement.  No hash table is allocated for such faces.
    if (!_checked) {
        _has_vert = FT_HAS_VERTICAL(face) != 0;
        _checked = true;
    }
    if (!_has_vert)
        return false;

    // Lazy allocation: defer the hash table until the first vertical
    // lookup actually arrives, so faces never used in vertical mode
    // (most fonts in horizontal-only documents) cost nothing.
    if (!_cache)
        _cache = new LVHashTable<lUInt32, VertGlyphMetrics>(256);

    if (_cache->get(glyph_index, out))
        return true;

    // FT_LOAD_NO_BITMAP: we only want metrics — the bitmap was already
    // rendered and cached via the standard horizontal load path.
    int err = FT_Load_Glyph(face, glyph_index,
                            FT_LOAD_DEFAULT | FT_LOAD_VERTICAL_LAYOUT |
                            FT_LOAD_NO_BITMAP);
    if (err)
        return false;

    out.origin_x = (lInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertBearingX);
    out.origin_y = (lInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertBearingY);
    out.advance  = (lUInt16) FONT_METRIC_TO_PX(face->glyph->metrics.vertAdvance);
    _cache->set(glyph_index, out);
    return true;
}

void LVFontVertGlyphMetricsCache::clear()
{
    if (_cache)
        _cache->clear();
    _checked = false;
    _has_vert = false;
}
