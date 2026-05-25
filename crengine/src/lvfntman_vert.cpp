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

#include <ft2build.h>
#include FT_FREETYPE_H

// Match the local macro in lvfntman.cpp (round + truncate from 26.6 fixed).
#define FONT_METRIC_TO_PX(x)    (((x)+32) >> 6)

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
