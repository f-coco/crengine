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
// Class list and layout fields ported from LuaTeX-ja src/jfm-ujisv.lua.
// JLREQ_VERT_VERT_MARK has no counterpart in jfm-ujisv — it is a fork-only
// auxiliary class for font-quality compensation, dispatched on
// LFNT_HINT_VERTICAL_MARK rather than the classifier.
enum JLReqVertClass {
    JLREQ_VERT_CJK_BODY = 0,        // jfm-ujisv [0]: default body CJK
                                    //   width = 1.0 em, align = middle
                                    //   (includes ー 〜 ～ which are not
                                    //    explicitly listed in jfm-ujisv)
    JLREQ_VERT_OPEN_BRACKET,        // jfm-ujisv [1]: ‘ “ 〈 《 「 『 【 〔 〖 〘 〝 （ ［ ｛ ｟
                                    //   width = 0.5 em, align = right
    JLREQ_VERT_CLOSE_BRACKET_COMMA, // jfm-ujisv [2]: ’ ” 〉 》 」 』 】 〕 〗 〙 〟 ） ］ ｝ ｠ 、 ，
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_MIDDLE_DOT,          // jfm-ujisv [3]: ・ ： ； ·
                                    //   width = 0.5 em, align = middle
    JLREQ_VERT_PERIOD,              // jfm-ujisv [4]: 。 ．
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_DASH,                // jfm-ujisv [5]: — ― ‥ … 〳 〴 〵
                                    //   width = 1.0 em, align = left
    JLREQ_VERT_EXCLAM_QUEST,        // jfm-ujisv [6]: ？ ！ ‼ ⁇ ⁈ ⁉
                                    //   width = 1.0 em, align = left
    JLREQ_VERT_HALF_KANA,           // jfm-ujisv [7]: U+FF61..U+FF9F (halfwidth)
                                    //   width = 0.5 em, align = left
    JLREQ_VERT_VERT_MARK,           // fork-only compensation — NOT in jfm-ujisv.
                                    //   Dispatched on LFNT_HINT_VERTICAL_MARK,
                                    //   not on classifier output.  See lvfntman.cpp.
    JLREQ_VERT_OTHER                // Latin/numerals/etc — rotation path or default
};

// Returns the JLReq vertical class for `c`.  Vertical marks already detected
// by isJapaneseHorizontalMark()/LFNT_HINT_VERTICAL_MARK return
// JLREQ_VERT_VERT_MARK here too (callers may dispatch on either signal).
JLReqVertClass getJLReqVertClass(lChar32 c);

// Per-class JFM layout (LuaTeX-ja jfm-ujisv.lua port).
//
//   width_halves    1 = half-em (= em/2), 2 = full em.  This is the slot's
//                   advance dimension (Y in vertical-rl).  Half-em classes
//                   are JLReq's "compacted" punctuation/brackets that sit
//                   close to their target glyph.
//   align           Alignment within the slot in the advance direction:
//                   JLREQ_ALIGN_LEFT   = start of slot (top in tate)
//                   JLREQ_ALIGN_MIDDLE = centre
//                   JLREQ_ALIGN_RIGHT  = end of slot (bottom in tate)
//                   For full-em slots whose glyph is also full em, align is
//                   moot (no shift).  For half-em slots, align decides where
//                   the glyph sits within the smaller slot, leaving the
//                   other half as compaction whitespace.
//
// X position (perpendicular to advance, = column axis in vertical-rl) is
// NOT controlled by JFM — that comes from the font's vmtx vBX, applied
// uniformly for all classes.  JFM only governs advance-direction layout.
enum JLReqVertAlign {
    JLREQ_ALIGN_LEFT   = 0,
    JLREQ_ALIGN_MIDDLE = 1,
    JLREQ_ALIGN_RIGHT  = 2,
};

struct JLReqVertLayout {
    lUInt8 width_halves;
    lUInt8 align;
};

JLReqVertLayout getJLReqVertLayout(JLReqVertClass cls);

// JFM slot width (advance dimension) for `c`, in pixels.  For named JFM
// classes returns `em_px * width_halves / 2` (em or em/2).  For
// JLREQ_VERT_OTHER (chars not in jfm-ujisv — e.g. Latin glyphs reaching
// this path via a buggy classifier or future extension), returns
// `natural_advance_px` so the font's own advance is preserved.
int getJLReqVertSlotWidth(lChar32 c, int em_px, int natural_advance_px);

// Convenience: JFM in-slot shift (cwa) in the advance direction for `c`,
// in pixels.  Implements `cwa = align * (fwidth - vadv)` from LuaTeX-ja
// ltj-setwidth.lua capsule_glyph_tate() line 269, with vadv = em (CJK
// natural vertical advance).  Returns a NEGATIVE value for half-em
// classes with align >= MIDDLE, shifting the glyph "backward" in
// advance direction (= up in tate) to overlap with the previous slot.
int getJLReqVertCwa(lChar32 c, int em_px);

// In-slot bitmap Y offset for half-em JFM classes (brackets, punctuation,
// halfwidth kana), in pixels relative to slot top.
//
// IMPORTANT: deviates from LuaTeX-ja jfm-ujisv on two counts:
//
//   1. Anchors the bitmap directly to the SLOT edge, ignoring the font's
//      vBY (which carries horizontal-mode design choices that misplace
//      vertical-mode brackets in Noto / Hiragino +vert variants).
//
//   2. Computes the offset relative to a FULL EM SLOT, not the half-em
//      slot LuaTeX-ja's JFM specifies.  Half-em compaction (advance =
//      em/2) shifts the entire slot up by em/2 per preceding half-em
//      char — visually moving brackets and punctuation higher in the
//      column than their natural per-em position.  We retain em-wide
//      slots so adjacent chars sit where the eye expects, and apply
//      align WITHIN the em slot only to bias the glyph toward its
//      JLReq anchor edge.
//
// Behaviour at em_px:
//   align=LEFT   →  0                  (bitmap top at slot top — used for
//                                       」』〉》、，。．．half-width kana)
//   align=MIDDLE →  (em_px - bmh)/2    (centred in em slot — used for ・)
//   align=RIGHT  →  em_px - bmh        (bitmap bottom at slot bottom —
//                                       used for 「『〈《【〔〘 etc.,
//                                       brings bracket close to next char)
//
// Returns -1 for full-em classes (CJK_BODY, DASH, EXCLAM_QUEST, VERT_MARK,
// OTHER); caller should fall back to its default Y computation
// (bitmap-center or font's vBY).
int getJLReqVertHalfEmYOffset(lChar32 c, int em_px, int bmh_px);

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
