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

// True for characters that should be placed by the central-baseline /
// virtual-body model in vertical-rl/lr layout (JLReq, CSS Writing Modes 3,
// upTeX/LuaTeX-ja JFM): ideographs and syllabic kana/Hangul that occupy a
// uniform 1em virtual body centred on the column axis.  Per-glyph vmtx
// variation in CJK fonts is generated noise, not authorial intent, and
// mainstream typesetting systems normalise it.  Caller bitmap-centres
// these glyphs in the column for visually uniform body text.
//
// Excludes:
//   - CJK punctuation block (U+3001..U+303F): position is by font design
//   - Halfwidth/Fullwidth Forms (U+FF00..U+FFEF): mixed semantics
//   - Vertical marks (ー — — ‥ … etc.): handled via LFNT_HINT_VERTICAL_MARK
bool isUniformVerticalIdeograph(lChar32 c);

// =============================================================================
// JLReq vertical character class — LuaTeX-ja-style JFM (Japanese Font Metric).
//
// LuaTeX-ja groups characters into JFM classes that share the same in-slot
// placement rules (slot width in halves of em, alignment within slot).  This
// makes punctuation/bracket placement font-independent: rather than trusting
// each font's vmtx (which differs vendor-to-vendor for 、。「」), the renderer
// applies JLReq-conformant rules per class.
//
// Class set mirrors jfm-ujisv.lua (canonical vertical JFM, jis-derived).
// Numeric order matches the reference for documentation purposes; do not
// rely on it programmatically.
//
// Reference: github.com/luatexja/luatexja  src/jfm-ujisv.lua
// =============================================================================
enum JLReqVertClass {
    JLREQ_VERT_CJK_BODY = 0,        // 漢字, ひらがな, カタカナ, Hangul, etc.
                                    //   width = 1.0 em, align = middle
    JLREQ_VERT_OPEN_BRACKET,        // 「『（〈《【〔〘 etc.
                                    //   width = 0.5 em, align = right
                                    //   → vertical-rl: glyph at slot bottom
                                    //     (= top of next char's space)
    JLREQ_VERT_CLOSE_BRACKET_COMMA, // 」』）〉》】〕〙、，
                                    //   width = 0.5 em, align = left
                                    //   → vertical-rl: glyph at slot top
                                    //     (right-side hang against previous char)
    JLREQ_VERT_MIDDLE_DOT,          // ・：；
                                    //   width = 0.5 em, align = middle
    JLREQ_VERT_PERIOD,              // 。．
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_DASH,                // ―—  (full-em dashes; ー and ‥ … are vert marks)
                                    //   width = 1.0 em, align = middle
    JLREQ_VERT_EXCLAM_QUEST,        // ！？
                                    //   width = 1.0 em, align = middle
    JLREQ_VERT_HALF_KANA,           // Halfwidth katakana (0xFF61..0xFF9F)
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_VERT_MARK,           // ー — ‥ … 〜 ～ — handled by
                                    //   LFNT_HINT_VERTICAL_MARK (centred in column)
    JLREQ_VERT_OTHER                // Latin/numerals/etc — routed through the
                                    //   rotation path or treated like body CJK
};

// Returns the JLReq vertical class for `c`.  Vertical marks already detected
// by isJapaneseHorizontalMark()/LFNT_HINT_VERTICAL_MARK return
// JLREQ_VERT_VERT_MARK here too (callers may dispatch on either signal).
JLReqVertClass getJLReqVertClass(lChar32 c);

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
