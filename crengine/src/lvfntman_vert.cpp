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
bool isUniformVerticalIdeograph(lChar32 c)
{
    // Hiragana
    if (c >= 0x3041 && c <= 0x309F) return true;
    // Katakana, excluding ー (0x30FC) which is handled as a vert mark
    if (c >= 0x30A0 && c <= 0x30FB) return true;
    if (c >= 0x30FD && c <= 0x30FF) return true;
    // Bopomofo
    if (c >= 0x3105 && c <= 0x312F) return true;
    // CJK Unified Ideographs Extension A
    if (c >= 0x3400 && c <= 0x4DBF) return true;
    // CJK Unified Ideographs
    if (c >= 0x4E00 && c <= 0x9FFF) return true;
    // Hangul Syllables
    if (c >= 0xAC00 && c <= 0xD7A3) return true;
    // CJK Compatibility Ideographs
    if (c >= 0xF900 && c <= 0xFAFF) return true;
    // CJK Unified Ideographs Extension B and beyond
    if (c >= 0x20000) return true;
    return false;
}

// LuaTeX-ja JFM vertical character class.
//
// Char lists ported verbatim from LuaTeX-ja src/jfm-ujisv.lua
// (commit verified 2026-05-27).  Any deviation from that file should
// be treated as a bug — keep the two in sync.
//
// Note: JLREQ_VERT_VERT_MARK does NOT correspond to a class in jfm-ujisv.
// It is a fork-only auxiliary signal (see LFNT_HINT_VERTICAL_MARK in
// lvfntman.h) used to compensate for fonts whose +vert glyph variants
// have buggy hmtx/vmtx (e.g. Hiragino's vBX=0 on ー).  Per LuaTeX-ja's
// own taxonomy, ー 〜 ～ are class [0] body CJK; ‥ … are class [5]
// DASH; this function returns those classes per the source.  Callers
// who want the fork compensation must dispatch on
// LFNT_HINT_VERTICAL_MARK *before* consulting JLReqVertClass.
JLReqVertClass getJLReqVertClass(lChar32 c)
{
    // --- 開き括弧 (class [1] in jfm-ujisv.lua) ---
    switch (c) {
        case 0x2018: // ‘
        case 0x201C: // “
        case 0x3008: // 〈
        case 0x300A: // 《
        case 0x300C: // 「
        case 0x300E: // 『
        case 0x3010: // 【
        case 0x3014: // 〔
        case 0x3016: // 〖
        case 0x3018: // 〘
        case 0x301D: // 〝
        case 0xFF08: // （
        case 0xFF3B: // ［
        case 0xFF5B: // ｛
        case 0xFF5F: // ｟
            return JLREQ_VERT_OPEN_BRACKET;
        default:
            break;
    }

    // --- 閉じ括弧 + 読点 (class [2]) ---
    switch (c) {
        case 0x2019: // ’
        case 0x201D: // ”
        case 0x3009: // 〉
        case 0x300B: // 》
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x3011: // 】
        case 0x3015: // 〕
        case 0x3017: // 〗
        case 0x3019: // 〙
        case 0x301F: // 〟
        case 0xFF09: // ）
        case 0xFF3D: // ］
        case 0xFF5D: // ｝
        case 0xFF60: // ｠
        case 0x3001: // 、
        case 0xFF0C: // ，
            return JLREQ_VERT_CLOSE_BRACKET_COMMA;
        default:
            break;
    }

    // --- 中点 (class [3]) ---
    switch (c) {
        case 0x30FB: // ・
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0x00B7: // ·
            return JLREQ_VERT_MIDDLE_DOT;
        default:
            break;
    }

    // --- 句点 (class [4]) ---
    if (c == 0x3002 /* 。 */ || c == 0xFF0E /* ． */)
        return JLREQ_VERT_PERIOD;

    // --- 分離禁止文字 (class [5] in jfm-ujisv — em dashes and leaders) ---
    switch (c) {
        case 0x2014: // —
        case 0x2015: // ―
        case 0x2025: // ‥
        case 0x2026: // …
        case 0x3033: // 〳
        case 0x3034: // 〴
        case 0x3035: // 〵
            return JLREQ_VERT_DASH;
        default:
            break;
    }

    // --- 感嘆符・疑問符 (class [6]) ---
    switch (c) {
        case 0xFF01: // ！
        case 0xFF1F: // ？
        case 0x203C: // ‼
        case 0x2047: // ⁇
        case 0x2048: // ⁈
        case 0x2049: // ⁉
            return JLREQ_VERT_EXCLAM_QUEST;
        default:
            break;
    }

    // --- 半角カナ (class [7]) ---
    if (c >= 0xFF61 && c <= 0xFF9F)
        return JLREQ_VERT_HALF_KANA;

    // --- Body CJK (class [0], default) ---
    // jfm-ujisv treats ー (U+30FC), 〜 (U+301C), ～ (U+FF5E) as default body
    // CJK — relying on font's vBX/vBY for positioning.  fork callers may
    // override via LFNT_HINT_VERTICAL_MARK if the font's +vert form is
    // mis-positioned (Hiragino-style).
    if (isUniformVerticalIdeograph(c))
        return JLREQ_VERT_CJK_BODY;

    return JLREQ_VERT_OTHER;
}

// Per-class layout — mirrors LuaTeX-ja jfm-ujisv.lua's char_data
// (width / align fields).  See enum JLReqVertClass in lvfntman_vert.h.
JLReqVertLayout getJLReqVertLayout(JLReqVertClass cls)
{
    JLReqVertLayout out;
    out.width_halves = 2;            // full em by default
    out.align        = JLREQ_ALIGN_MIDDLE;
    switch (cls) {
        case JLREQ_VERT_CJK_BODY:
            // full em, centred — natural placement for ideographs/kana
            break;
        case JLREQ_VERT_OPEN_BRACKET:
            // 「『（〈《【〔〘 — half-em slot, glyph at slot bottom
            //   (compaction whitespace BEFORE the bracket, JLReq 3.1.10)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_RIGHT;
            break;
        case JLREQ_VERT_CLOSE_BRACKET_COMMA:
            // 」』）〉》】〕〙、，— half-em, glyph at slot top
            //   (compaction whitespace AFTER, JLReq 3.1.10)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_MIDDLE_DOT:
            // ・：； — half-em, centred
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_MIDDLE;
            break;
        case JLREQ_VERT_PERIOD:
            // 。．— half-em, glyph at slot top (JLReq 3.1.5: top-right corner)
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_DASH:
            // — ― ‥ … 〳 〴 〵 — full em, glyph at slot top (= align='left')
            // per jfm-ujisv class [5].
            out.width_halves = 2;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_EXCLAM_QUEST:
            // ？！‼⁇⁈⁉ — full em, glyph at slot top (= align='left')
            // per jfm-ujisv class [6].
            out.width_halves = 2;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_HALF_KANA:
            // 半角カタカナ — half-em slot, glyph at slot top
            out.width_halves = 1;
            out.align        = JLREQ_ALIGN_LEFT;
            break;
        case JLREQ_VERT_VERT_MARK:
            // ー — ‥ … 〜 ～ — full em, centred (vertical bar/dot pattern)
            break;
        case JLREQ_VERT_OTHER:
            // Latin/numerals routed elsewhere; treat as full-em fallback
            break;
    }
    return out;
}

int getJLReqVertSlotWidth(lChar32 c, int em_px, int natural_advance_px)
{
    JLReqVertClass cls = getJLReqVertClass(c);
    if (cls == JLREQ_VERT_OTHER)
        return natural_advance_px;        // not in jfm-ujisv → font's natural
    JLReqVertLayout layout = getJLReqVertLayout(cls);
    return (em_px * layout.width_halves) / 2;
}

int getJLReqVertCwa(lChar32 c, int em_px)
{
    JLReqVertLayout layout = getJLReqVertLayout(getJLReqVertClass(c));
    // cwa = align_num * (fwidth - vadv) per ltj-setwidth.lua:269.
    // align_num: 0 (left), 0.5 (middle), 1 (right) — our enum maps to
    // 0, 1, 2; divide by 2 to recover the fractional value.
    // vadv is the font's vertical advance, which for CJK = em.
    int fwidth = (em_px * layout.width_halves) / 2;
    int vadv   = em_px;
    return (((int)layout.align) * (fwidth - vadv)) / 2;
}

int getJLReqVertHalfEmYOffset(lChar32 c, int em_px, int bmh_px)
{
    JLReqVertLayout layout = getJLReqVertLayout(getJLReqVertClass(c));
    if (layout.width_halves != 1)
        return -1;            // not a half-em class
    // Anchor in FULL EM slot — see header doc for why we deviate from
    // LuaTeX-ja's half-em slot here.
    int slot_h = em_px;
    int empty  = slot_h - bmh_px;
    if (empty < 0)
        empty = 0;
    switch (layout.align) {
        case JLREQ_ALIGN_LEFT:
            return 0;
        case JLREQ_ALIGN_MIDDLE:
            return empty / 2;
        case JLREQ_ALIGN_RIGHT:
        default:
            return empty;
    }
}

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
