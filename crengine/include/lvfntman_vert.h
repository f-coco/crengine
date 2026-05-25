// =============================================================================
// TTB (top-to-bottom) glyph metrics cache for vertical-rl/lr rendering.
//
// Fork-only header.  No upstream counterpart.  Extracted to keep the
// per-face vertical-metrics machinery out of lvfntman.cpp and reduce
// soft-fork divergence.
//
// Used by DrawTextString in vertical mode when the font has vhea/vmtx
// tables (FT_HAS_VERTICAL): glyphs are positioned by vertBearingX/Y +
// vertAdvance instead of the horizontal hmtx bearings.  This yields
// uniform placement across all fonts, including ones (like Hiragino
// Mincho Pro) whose horizontal bitmap_top varies per glyph.
// =============================================================================
#ifndef __LV_FNT_MAN_VERT_H_INCLUDED__
#define __LV_FNT_MAN_VERT_H_INCLUDED__

#include "lvtypes.h"
#include "lvhashtable.h"

// Forward-declare FT_Face so this header doesn't pull in FreeType.
struct FT_FaceRec_;
typedef struct FT_FaceRec_ * FT_Face;

struct VertGlyphMetrics {
    lInt16  origin_x;   // vertBearingX in pixels
    lInt16  origin_y;   // vertBearingY in pixels
    lUInt16 advance;    // vertAdvance in pixels
};

// Per-face TTB glyph-metrics cache.  Lazily populated via
// FT_LOAD_VERTICAL_LAYOUT on first lookup of each glyph.  Held as a
// member of LVFreeTypeFace so lifetime tracks the face.
//
// Memory: the underlying hash table is allocated lazily on first
// successful lookup, so fonts without vhea (DroidSansMono, Latin-only
// fonts) and faces never used in vertical mode pay no allocation cost.
class LVFontVertGlyphMetricsCache {
public:
    LVFontVertGlyphMetricsCache() : _cache(NULL), _has_vert(false), _checked(false) {}
    ~LVFontVertGlyphMetricsCache() { delete _cache; }

    // Returns true and fills `out` if `face` has vhea/vmtx and the
    // glyph's vertical metrics are available; false otherwise.
    // Caller falls back to horizontal-metric placement when this
    // returns false.
    bool get(FT_Face face, lUInt32 glyph_index, VertGlyphMetrics & out);

    void clear();

private:
    LVHashTable<lUInt32, VertGlyphMetrics> * _cache;  // lazily allocated
    bool _has_vert;   // FT_HAS_VERTICAL(face) result, cached after first check
    bool _checked;
};

#endif  // __LV_FNT_MAN_VERT_H_INCLUDED__
