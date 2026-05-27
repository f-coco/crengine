// =============================================================================
// Vertical-mode formatter layout code (vertical-rl / vertical-lr).
//
// Fork origin: this file was created in commit f8b0bbe1 ("vertical-rl
// Option C Phase 2b").  Its functions are vertical-writing-mode siblings
// of their `*Horizontal` counterparts in lvtextfm.cpp (which lived in
// a separate lvtextfm_layout_h.cpp from commit f8b0bbe1 through Phase
// C Step 2a, then folded back into lvtextfm.cpp in Steps 2b/2d).  They were initially ported
// (= cloned and adapted) from the same upstream lvtextfm.cpp originals,
// then specialised for column flow, kinsoku (line-breaking rules), and
// the Y=X coordinate swap.
//
// When reconciling an upstream change to lvtextfm.cpp that lands in a
// region covered by one of the *Horizontal functions, consider whether
// the analogous *Vertical function here needs the same fix.
// =============================================================================

#define MIN_WORD_LEN_TO_HYPHENATE 4
#define MAX_WORD_SIZE 64

// Forward declarations (defined in lvtextfm.cpp)
void addLineHorizontal( LVFormatter* fmt, int start, int end, int x, src_text_fragment_t * para, bool first, bool last, bool preFormattedOnly, bool isLastPara, bool hasInlineBoxes );

// =============================================================================
// Vertical-mode diagnostic globals (Phase C Step 1 + 2a relocation).
//
// All ltext_vert_* counters live here so lvtextfm.cpp stays closer to
// upstream.  Their extern declarations are in lvtextfm_fork.h.
// Increment sites are:
//   - lvtextfm.cpp measureText (ruby_adv_diff)
//   - lvtextfm.cpp LFormattedText::Draw (bleed, fmt_draws, fmt_calls,
//     fmt_vert_calls, word_iters, ib_layout_gap, char_overlap)
// Reset/getter functions are called from cre.cpp via extern linkage.
// =============================================================================

// Vertical-ruby: render_w − advance accumulated diff (was in lvtextfm.cpp).
int ltext_vert_ruby_adv_diff_total = 0;
int ltext_vert_ruby_adv_diff_max   = 0;
void ltext_reset_vert_ruby_adv_diff() {
    ltext_vert_ruby_adv_diff_total = 0;
    ltext_vert_ruby_adv_diff_max   = 0;
}
void ltext_get_vert_ruby_adv_diff(int *total_out, int *max_out) {
    *total_out = ltext_vert_ruby_adv_diff_total;
    *max_out   = ltext_vert_ruby_adv_diff_max;
}

// Vertical-rl bleed counters: accessible from cre.cpp via extern.
// Reset via ltext_reset_vert_bleed(); read via ltext_get_vert_bleed().
int ltext_vert_bleed_count = 0;
int ltext_vert_bleed_max_px = 0;

// Diagnostic: count CJK draw calls reached by the vertical formatter path.
// Compared with lfnt_vert_lifetime_draws this isolates whether records are
// lost between formatter and font (mismatch) or before the formatter (=0).
int ltext_vert_fmt_draws = 0;
// Counts every LFormattedText::Draw entry, regardless of writing-mode, and
// every word-loop iteration.  Comparing fmt_draws / fmt_calls / word_iters
// tells us whether inner ruby cells reach LFormattedText::Draw with vertical
// writing-mode or not.
int ltext_fmt_calls   = 0;
int ltext_fmt_vert_calls = 0;
int ltext_word_iters  = 0;

// Diagnostic: layout/draw position mismatch for vertical inline boxes.
// Fires when ib_word_x (from layout) > vert_min_next_x (draw tracking),
// meaning the layout placed the box further than the draw tracker expected —
// a gap is visible above the inline box.  With correct vert_layout_min_x
// tracking (e.g. after the CJK-only font_size fix for spaces) this is 0.
int ltext_vert_ib_layout_gap_total = 0;
int ltext_vert_ib_layout_gap_max   = 0;

void ltext_reset_vert_ib_layout_gap() {
    ltext_vert_ib_layout_gap_total = 0;
    ltext_vert_ib_layout_gap_max   = 0;
}
void ltext_get_vert_ib_layout_gap(int *total_out, int *max_out) {
    *total_out = ltext_vert_ib_layout_gap_total;
    *max_out   = ltext_vert_ib_layout_gap_max;
}

// Vertical-rl plain-character overlap counters.
// Fires when a CJK/plain character's y0 is less than the previous character's
// y0 + effective_width (= its slot end), meaning two characters overlap in the
// column (height) direction.  This is the "文字が被る" / character-overlap bug.
// Reset via ltext_reset_vert_char_overlap(); read via ltext_get_vert_char_overlap().
int ltext_vert_char_overlap_count = 0;
int ltext_vert_char_overlap_max_px = 0;

void ltext_reset_vert_char_overlap() {
    ltext_vert_char_overlap_count = 0;
    ltext_vert_char_overlap_max_px = 0;
}

void ltext_get_vert_char_overlap(int *count_out, int *max_px_out) {
    *count_out  = ltext_vert_char_overlap_count;
    *max_px_out = ltext_vert_char_overlap_max_px;
}

void ltext_reset_vert_bleed() {
    ltext_vert_bleed_count = 0;
    ltext_vert_bleed_max_px = 0;
}

void ltext_get_vert_bleed(int *count_out, int *max_px_out) {
    *count_out  = ltext_vert_bleed_count;
    *max_px_out = ltext_vert_bleed_max_px;
}

int ltext_get_vert_fmt_draws() {
    return ltext_vert_fmt_draws;
}

void ltext_get_fmt_counts(int *calls_out, int *vert_calls_out, int *word_iters_out) {
    if (calls_out)      *calls_out      = ltext_fmt_calls;
    if (vert_calls_out) *vert_calls_out = ltext_fmt_vert_calls;
    if (word_iters_out) *word_iters_out = ltext_word_iters;
}

// =============================================================================
// alignLineHorizontalVerticalPostPass
//
// Origin: extracted from upstream `LVFormatter::alignLine()` (the
// `if (hasInlineBoxes) { ... }` tail block, ~line 3645 in upstream).  The
// upstream block handled inline-box absolute positioning + MathML baseline
// adjustment.  The fork added a parallel branch for plain (non-inline-box)
// words in vertical writing mode that mirrors Draw's vert_min_next_x state
// (vert_layout_min_x), plus Phase 5 (LuaTeX-ja JFM) inter-class glue and
// xkanjiskip shifts so getRect() returns the same position Draw uses.
//
// Extracted to lvtextfm_vert.cpp during the upstream-merge-friendliness
// pass so lvtextfm.cpp's alignLine stays close to upstream.  Future upstream
// changes to the IB-positioning region must be ported here.
//
// Invocation guard: called whenever (hasInlineBoxes || vertical_mode).
// The Phase 5 plain-word post-pass MUST run on every vertical-mode line,
// not just lines with inline boxes — otherwise lines without ruby would
// have a different word->x layout from lines with ruby, breaking column
// stride consistency (see spec/unit/ruby_position_spec.lua).
// =============================================================================
void alignLineHorizontalVerticalPostPass( LVFormatter* fmt, formatted_line_t * frmline, bool hasInlineBoxes ) {
    #if MATHML_SUPPORT==1
        lUInt16 needed_baseline = frmline->baseline;
        lUInt16 needed_height = frmline->height;
    #endif
    // In vertical mode we also mirror the vert_min_next_x clamping that
    // Draw() applies at render time.  Without this, getRect() returns the
    // pre-clamp layout position, causing sbox.y to sit above the rendered
    // glyph by clamp_delta (≈ 1/5 em when the ruby group nearly touches
    // the preceding character).
    bool is_vert_frmline = (fmt->m_pbuffer->writing_mode == css_wm_vertical_rl ||
                            fmt->m_pbuffer->writing_mode == css_wm_vertical_lr);
    int vert_layout_min_x = 0;  // mirrors vert_min_next_x in Draw()
    // Phase 5 xkanjiskip tracker: needs to mirror vert_prev_was_non_cjk_word
    // in Draw so word->x positions align with where Draw places each glyph.
    // 0 = no previous word yet; +1 = prev was non-CJK; -1 = prev was CJK.
    int vert_layout_prev_class = 0;
    // Phase 5 inter-CJK class tracker (mirror of Draw's vert_prev_cjk_class).
    // Stores the JFM class of the most recent CJK char so the next CJK can
    // look up jfm-ujisv.lua [N].glue[M] inter-class spacing.  -1 = none.
    int vert_layout_prev_cjk_class = -1;
    (void)hasInlineBoxes;  // currently unused; kept for parity with upstream guard
    for ( int i=0; i<frmline->word_count; i++ ) {
        formatted_word_t * wi = &frmline->words[i];
        if ( is_vert_frmline && !(wi->flags & LTEXT_WORD_IS_INLINE_BOX) ) {
            // Plain / space word: advance vert_layout_min_x past it.
            int eff_w = (int)wi->width;
            // Guard: only access t.font when the source fragment is a text
            // fragment (not an image, inline-box pad, or other object type).
            if ( (int)wi->src_text_index < fmt->m_pbuffer->srctextlen ) {
                src_text_fragment_t * si = &fmt->m_pbuffer->srctext[wi->src_text_index];
                if ( !(si->flags & LTEXT_SRC_IS_OBJECT) && si->t.font ) {
                    LVFont * fi = (LVFont *)si->t.font;
                    int font_sz = fi->getSize();
                    // CJK punctuation with VERY narrow HarfBuzz TTB advance (no Phase 3
                    // override applied — i.e. font has no proper vmtx, advance < em/2)
                    // needs a font_size minimum so the glyph doesn't overlap the next char.
                    // BUT Phase 3 (LuaTeX-ja JFM half-em compaction, see
                    // getJLReqVertSlotWidth) intentionally sets word->width = em/2 for
                    // class [1] [2] [3] [4] [7] chars, and Phase 4 cwa positions the
                    // bitmap correctly within that half-em slot.  We must NOT clamp those
                    // up to em — that would re-introduce the same "char at top of em slot
                    // with gap below" behaviour that Phase 4 is supposed to fix.
                    //
                    // Non-CJK words (Latin, space) use the actual advance so that
                    // vert_layout_min_x matches the draw's vert_min_next_x tracking.
                    bool is_cjk = (wi->flags & (LTEXT_WORD_IS_CJK | LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)) != 0;
                    bool is_half_em_jfm = false;
                    if ( is_cjk && si->t.text && wi->t.len > 0 ) {
                        JLReqVertLayout layout = getJLReqVertLayout(
                            getJLReqVertClass(si->t.text[wi->t.start]) );
                        is_half_em_jfm = (layout.width_halves == 1);
                    }
                    eff_w = (is_cjk && !is_half_em_jfm && (int)wi->width < font_sz)
                          ? font_sz : (int)wi->width;
                }
            }
            // Phase 5 xkanjiskip mirror: shift this word's layout position by
            // 0.25em when transitioning CJK↔non-CJK, matching the Draw side.
            // Without this, getRect() for the post-boundary word would return
            // the layout position from m_advance (no xkanjiskip), while Draw
            // renders at +0.25em — causing highlights to be offset.
            bool wi_is_cjk = (wi->flags & (LTEXT_WORD_IS_CJK | LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)) != 0;
            bool boundary_cjk_non_cjk =
                   (vert_layout_prev_class == +1 && wi_is_cjk)    // non-CJK → CJK
                || (vert_layout_prev_class == -1 && !wi_is_cjk);  // CJK → non-CJK
            if (boundary_cjk_non_cjk) {
                int em_for_kern = 20; // fallback
                if ( (int)wi->src_text_index < fmt->m_pbuffer->srctextlen ) {
                    src_text_fragment_t * sik = &fmt->m_pbuffer->srctext[wi->src_text_index];
                    if ( !(sik->flags & LTEXT_SRC_IS_OBJECT) && sik->t.font ) {
                        em_for_kern = ((LVFont *)sik->t.font)->getSize();
                    }
                }
                vert_layout_min_x += em_for_kern / 4;  // 0.25em
            }
            vert_layout_prev_class = wi_is_cjk ? -1 : +1;
            // Phase 5 CJK↔CJK inter-class glue mirror (matches Draw side).
            // Look up jfm-ujisv.lua [N].glue[M] for (prev_cjk_class,curr_cjk_class)
            // and shift vert_layout_min_x BEFORE this word.  Keeps
            // word->width = compacted slot width.
            JLReqVertClass curr_cjk_cls_lay = JLREQ_VERT_OTHER;
            if ( wi_is_cjk && (int)wi->src_text_index < fmt->m_pbuffer->srctextlen ) {
                src_text_fragment_t * sicls = &fmt->m_pbuffer->srctext[wi->src_text_index];
                if ( !(sicls->flags & LTEXT_SRC_IS_OBJECT) && sicls->t.text && wi->t.len > 0 ) {
                    curr_cjk_cls_lay = getJLReqVertClass(sicls->t.text[wi->t.start]);
                }
            }
            if ( wi_is_cjk && vert_layout_prev_cjk_class >= 0 ) {
                int eighths = getJLReqGlueKernEighths(
                        (JLReqVertClass)vert_layout_prev_cjk_class, curr_cjk_cls_lay);
                if ( eighths > 0 ) {
                    int em_for_glue = 20;
                    if ( (int)wi->src_text_index < fmt->m_pbuffer->srctextlen ) {
                        src_text_fragment_t * sig = &fmt->m_pbuffer->srctext[wi->src_text_index];
                        if ( !(sig->flags & LTEXT_SRC_IS_OBJECT) && sig->t.font ) {
                            em_for_glue = ((LVFont *)sig->t.font)->getSize();
                        }
                    }
                    vert_layout_min_x += (em_for_glue * eighths) / 8;
                }
            }
            if ( wi_is_cjk ) {
                vert_layout_prev_cjk_class = (int)curr_cjk_cls_lay;
            } else {
                vert_layout_prev_cjk_class = -1;
            }
            // Mirror Draw()'s vert_min_next_x clamping: if a previous word's
            // effective advance pushed vert_layout_min_x past this word's layout
            // position, update word->x so getRect() returns the rendered position,
            // not the raw layout position. This prevents highlights from appearing
            // above the actual glyph when font CJK advances are slightly < font_size.
            if ( (int)wi->x < vert_layout_min_x )
                wi->x = (lInt16)vert_layout_min_x;
            int next_x = wi->x + eff_w;
            if ( next_x > vert_layout_min_x )
                vert_layout_min_x = next_x;
            continue;
        }
        if ( frmline->words[i].flags & LTEXT_WORD_IS_INLINE_BOX ) {
            formatted_word_t * word = &frmline->words[i];
            src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
            ldomNode * node = (ldomNode *) srcline->object;
            ldomNode * source_node = node->getEffectiveNode();
            if ( source_node != node ) {
                // We ended up positionning the cloneNode for an inlineBox on
                // the ::first-line of the paragraph, which means the original
                // inlineBox has not been positionned and won't be shown.
                // Flag that original node as not-rendered/discarded, so that
                // getRect(), when called on a xpointer to the original source,
                // know it has to climb to its clondeNode and use its rect.
                RenderRectAccessor source_fmt( source_node );
                RENDER_RECT_SET_FLAG(source_fmt, BOX_IS_DISCARDED);
            }
            RenderRectAccessor node_fmt( node );
            bool vert_mode = (fmt->m_pbuffer->writing_mode == css_wm_vertical_rl ||
                              fmt->m_pbuffer->writing_mode == css_wm_vertical_lr);
            if ( RENDER_RECT_HAS_FLAG(node_fmt, BOX_IS_POSITIONNED) ) {
                if ( is_vert_frmline ) {
                    // Still advance vert_layout_min_x even if already positioned.
                    int clamped_x = node_fmt.getX() - frmline->x;
                    int nx = clamped_x + (int)word->width;
                    if ( nx > vert_layout_min_x ) vert_layout_min_x = nx;
                }
                continue;
            }
            RENDER_RECT_SET_FLAG(node_fmt, BOX_IS_POSITIONNED);
            if ( is_vert_frmline ) {
                // Apply the same clamping that Draw() will use at render time
                // so getRect() returns the rendered (not pre-clamp) position.
                int ib_word_x    = word->x;
                int clamped_ib_x = (ib_word_x < vert_layout_min_x)
                                   ? vert_layout_min_x : ib_word_x;
                node_fmt.setX( frmline->x + clamped_ib_x );
                vert_layout_min_x = clamped_ib_x + (int)word->width;
                // Inline box ends the JFM class chain.
                vert_layout_prev_cjk_class = -1;
                vert_layout_prev_class = 0;
            } else {
                node_fmt.setX( frmline->x + word->x );
            }
            // In vertical mode, Y encodes the horizontal offset from the right edge.
            // Baseline alignment (a vertical concept in horizontal text) must not be
            // applied as a horizontal shift; doing so displaces ruby base characters
            // rightward by (baseline - ruby_baseline). Use frmline->y directly.
            bool is_vert = vert_mode;
            // For vertical mode: position the ruby block so the annotation
            // overhangs to the right of the column (per JLReq) for all
            // line-spacing values.
            // col_width is always strut_height (no inflation for ruby).
            //
            // For ruby inline boxes (display:ruby): annotation is at doc_y=0
            // (rightmost on screen), base at doc_y=annotation_h.  The body
            // font em is independent of line-height, so
            //   vert_y_adjust = em - box_h = -annotation_h
            // places the annotation to the right of the column boundary
            // regardless of col_w.  The base char is right-aligned within
            // the column (sbox.right = column right = line_x_col), which
            // keeps the annotation directly adjacent to the base (JLReq).
            //
            // For other small inline boxes: centre within strut.
            int vert_y_adjust = 0;
            if ( is_vert ) {
                int box_h = word->o.height;
                int col_w = fmt->m_pbuffer->strut_height;
                bool is_ruby_box = isRubyInlineBox(node);
                if ( is_ruby_box ) {
                    LVFontRef fnt = node->getFont();
                    int em = !fnt.isNull() ? fnt->getSize() : 0;
                    if ( em > 0 && em < col_w )
                        vert_y_adjust = (col_w + em) / 2 - box_h;  // centred
                    else
                        vert_y_adjust = col_w - box_h; // fallback
                } else if ( col_w > box_h ) {
                    vert_y_adjust = (col_w - box_h) / 2;  // centre small box
                } else if ( box_h > col_w ) {
                    vert_y_adjust = col_w - box_h;  // negative: annotation overhangs right
                }
            }
            node_fmt.setY( (int)frmline->y + (is_vert ? vert_y_adjust : (int)frmline->baseline - (int)word->o.baseline + (int)word->y) );
            node_fmt.push();
            #if MATHML_SUPPORT==1
                ldomNode * unboxedParent = node->getUnboxedParent();
                if ( unboxedParent ) {
                    lUInt16 unboxedParentId = unboxedParent->getNodeId();
                    if ( unboxedParentId >= EL_MATHML_START && unboxedParentId <= EL_MATHML_END ) {
                        ensureMathMLVerticalStretch(node, frmline->y, frmline->baseline, frmline->height,
                                                                        needed_baseline, needed_height);
                    }
                }
            #endif
        }
    }
    #if MATHML_SUPPORT==1
        if ( needed_height > frmline->height ) {
            frmline->height = needed_height;
        }
        if ( needed_baseline > frmline->baseline ) {
            int baseline_shift = needed_baseline - frmline->baseline;
            frmline->baseline = needed_baseline;
            // We need to update all the inlineBoxes absolute positions in the paragraph,
            // as they are all to be positionned relative to the baseline, which has moved.
            for ( int i=0; i<frmline->word_count; i++ ) {
                if ( frmline->words[i].flags & LTEXT_WORD_IS_INLINE_BOX ) {
                    formatted_word_t * word = &frmline->words[i];
                    src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
                    ldomNode * node = (ldomNode *) srcline->object;
                    RenderRectAccessor node_fmt( node );
                    node_fmt.setY( node_fmt.getY() + baseline_shift );
                    node_fmt.push();
                }
            }
        }
    #endif
}

// 行頭禁則: characters that JLReq prohibits at the start of a line / column.
// In addition to cjkt_closing_bracket (handled separately), LuaTeX-ja's
// kinsoku_table prohibits: 長音記号 (ー), 漢字繰り返し記号 (々々), 仮名繰り返し記号
// (ヽヾゝゞ), iteration mark (〻), and small kana (ぁぃぅぇぉっゃゅょゎ etc.).
// These are pulled back into the previous column when they would otherwise
// start a new column.
static inline bool isVertLineStartProhibitedExt(lChar32 ch) {
    switch (ch) {
        case 0x30FC: // ー KATAKANA-HIRAGANA PROLONGED SOUND MARK
        case 0x3005: // 々 IDEOGRAPHIC ITERATION MARK
        case 0x303B: // 〻 VERTICAL IDEOGRAPHIC ITERATION MARK
        case 0x309D: // ゝ HIRAGANA ITERATION MARK
        case 0x309E: // ゞ HIRAGANA VOICED ITERATION MARK
        case 0x30FD: // ヽ KATAKANA ITERATION MARK
        case 0x30FE: // ヾ KATAKANA VOICED ITERATION MARK
        case 0x3031: // 〱 VERTICAL KANA REPEAT MARK
        case 0x3032: // 〲 VERTICAL KANA REPEAT WITH VOICED SOUND MARK
        case 0x3033: // 〳 VERTICAL KANA REPEAT MARK UPPER HALF
        case 0x3034: // 〴 VERTICAL KANA REPEAT WITH VOICED SOUND MARK UPPER HALF
        case 0x3035: // 〵 VERTICAL KANA REPEAT MARK LOWER HALF
        // Small hiragana
        case 0x3041: // ぁ
        case 0x3043: // ぃ
        case 0x3045: // ぅ
        case 0x3047: // ぇ
        case 0x3049: // ぉ
        case 0x3063: // っ
        case 0x3083: // ゃ
        case 0x3085: // ゅ
        case 0x3087: // ょ
        case 0x308E: // ゎ
        case 0x3095: // ゕ
        case 0x3096: // ゖ
        // Small katakana
        case 0x30A1: // ァ
        case 0x30A3: // ィ
        case 0x30A5: // ゥ
        case 0x30A7: // ェ
        case 0x30A9: // ォ
        case 0x30C3: // ッ
        case 0x30E3: // ャ
        case 0x30E5: // ュ
        case 0x30E7: // ョ
        case 0x30EE: // ヮ
        case 0x30F5: // ヵ
        case 0x30F6: // ヶ
        // Bopomofo iteration (Chinese)
        case 0x309B: // ゛ (combining voiced sound mark, sometimes line-start prohibited)
        case 0x309C: // ゜ (combining semi-voiced sound mark)
            return true;
        default:
            return false;
    }
}

// Returns true if 'ch' is one of the Japanese horizontal-mark characters
// that must be routed through the CJK +vert path in vertical mode.
// needsVerticalRotation90CW() also returns true for Latin/ASCII, so it cannot
// be used here — we need only the specific marks that are non-CJK by
// lStr_isCJK() but should still be rendered upright via +vert.
static inline bool isJapaneseHorizontalMark(lChar32 c) {
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
            return false;
    }
}

// Returns true if every character in [text, text+len) is a Japanese
// horizontal-mark character (―, —, …, ‥, ー, 〜, ～, －).
//
// Used by LFormattedText::Draw's classification step to route such words
// through the CJK +vert glyph path instead of the Latin-in-vertical
// render+rotate-as-block path.
bool isWordAllVertRotationChars(const lChar32 * text, int len) {
    if (!text || len <= 0)
        return false;
    for (int i = 0; i < len; i++) {
        if (!isJapaneseHorizontalMark(text[i]))
            return false;
    }
    return true;
}

// Returns true if 'ch' is a Japanese/CJK sentence-end character that must not
// start a new column (行頭禁則) and should hang at the bottom of the current
// column (ぶら下がり) instead.
static inline bool isVerticalHangingChar(lChar32 ch) {
    switch (ch) {
        case 0x3001: // 、IDEOGRAPHIC COMMA (読点)
        case 0x3002: // 。IDEOGRAPHIC FULL STOP (句点)
        case 0x30FB: // ・KATAKANA MIDDLE DOT
        case 0xFF01: // ！FULLWIDTH EXCLAMATION MARK
        case 0xFF0C: // ，FULLWIDTH COMMA
        case 0xFF0E: // ．FULLWIDTH FULL STOP
        case 0xFF1F: // ？FULLWIDTH QUESTION MARK
        case 0x2026: // … HORIZONTAL ELLIPSIS
            return true;
        default:
            return false;
    }
}

// Step 2: Wrapper that delegates word placement to addLineHorizontal,
// then patches frmline coordinates for vertical layout.
//
// The horizontal function handles all word creation, CJK spacing,
// bidi, overlap correction, alignment, and justify unchanged.
// We only patch the 3 frmline-level coordinates and the m_line_advance
// direction after it returns.
//
// NOTE: addLineHorizontal calls alignLineHorizontal internally, which does
// space-based justify. For CJK-heavy text (each char is its own word),
// this is a reasonable approximation of vertical letter-spacing justify.
// Proper vertical justify (Step 3) will be implemented after Step 4
// (DrawVertical) provides visible output for verification.
// -----------------------------------------------------------------------------
// addLineVertical
// Vertical-rl sibling of addLineHorizontal (lvtextfm.cpp), originally
// ported from upstream `LVFormatter::addLine` and re-specialised for column
// flow.  Cross-check against addLineHorizontal when upstream changes the
// horizontal one.
// -----------------------------------------------------------------------------
void addLineVertical( LVFormatter* fmt, int start, int end, int x, src_text_fragment_t * para, bool first, bool last, bool preFormattedOnly, bool isLastPara, bool hasInlineBoxes )
{
    // Delegate all word placement to horizontal function.
    // This creates the frmline, words, sets word->x/width/y, calls alignLineHorizontal,
    // and updates m_line_advance and m_pbuffer->height.
    //
    // For vertical text: addLineHorizontal already sets everything correctly.
    // - frmline->y = old m_line_advance = column's accumulated X offset from block start.
    // - frmline->height = frmline->width = col_width (set in addLineHorizontal's vertical fix).
    // - m_line_advance += col_width, so m_pbuffer->height = N * col_width after N columns.
    // - word->x = cumulative glyph Y advance within column (used by Draw() as y-offset).
    // - word->y = baseline valign offset (used by Draw() as x-offset correction).
    // Do NOT undo m_line_advance; that would corrupt m_pbuffer->height used for block height.
    addLineHorizontal( fmt, start, end, x, para, first, last, preFormattedOnly, isLastPara, hasInlineBoxes );
}

    /// Split paragraph into lines (vertical layout - Step 1)
///
/// This is the incremental Step 1 implementation: line splitting with
/// coordinate swap. Uses page_height as the line extent instead of width.
///
/// Key differences from processParagraphHorizontal():
/// - maxWidth -> maxHeight (using m_pbuffer->page_height)
/// - Float handling skipped (floats are a horizontal concept)
/// - getYWithAvailableWidth / fillAndMoveToY skipped (no vertical float avoidance)
/// - addLineHorizontal -> addLineVertical
///
/// addLineVertical currently delegates to addLineHorizontal, so line creation
/// is still horizontal. Step 2 will implement proper vertical addLine.
///
// -----------------------------------------------------------------------------
// processParagraphVertical
// Vertical-rl sibling of processParagraphHorizontal (lvtextfm.cpp).
// Originally ported from upstream `LVFormatter::processParagraph` and adapted
// for column-wise layout + kinsoku.  Cross-check when the horizontal one moves.
// -----------------------------------------------------------------------------
void processParagraphVertical( LVFormatter* fmt, int start, int end, bool isLastPara )
    {
        TR("processParagraphVertical(%d, %d)", start, end);

        // ensure buffer size is ok for paragraph
        fmt->allocate( start, end );
        // copy paragraph text to buffer
        fmt->copyText( start, end );
        // measure paragraph text (with LFNT_HINT_IS_VERTICAL, uses y_advance)
        fmt->measureText();

        // We keep as 'para' the first source text, as it carries
        // the text alignment to use with all added lines.
        src_text_fragment_t * para = &fmt->m_pbuffer->srctext[start];

        // detect case with inline preformatted text inside block with line feeds
        bool preFormattedOnly = true;
        for ( int i=start; i<end; i++ ) {
            if ( !(fmt->m_pbuffer->srctext[i].flags & LTEXT_FLAG_PREFORMATTED) ) {
                preFormattedOnly = false;
                break;
            }
        }
        if ( preFormattedOnly ) {
            bool lfFound = false;
            for ( int i=0; i<fmt->m_length; i++ ) {
                if ( fmt->m_text[i]=='\n' ) {
                    lfFound = true;
                    break;
                }
            }
            preFormattedOnly = preFormattedOnly && lfFound;
        }

        // For vertical text, the line extent is the page height (block direction).
        // Characters advance vertically (y_advance from HarfBuzz), and lines
        // (columns) are stacked horizontally (right to left for vertical-rl).
        //
        // IMPORTANT: m_advance[] here uses HarfBuzz VERTICAL advances with cluster
        // semantics — cluster members get advance=0, cluster end gets the full
        // cluster advance. This causes line-breaking to fire later than expected,
        // allowing too many characters into a column.
        //
        // In Draw(), word->x accumulates HORIZONTAL glyph widths (one per character,
        // not cluster-level), so the column height in pixels = sum(word->width).
        // We must limit that sum to (page_height - y), not m_advance.
        //
        // Fix: get the font's per-character advance (≈ font size) and use
        // char_count × font_advance for the column-height limit check.
        int maxHeight = fmt->m_pbuffer->page_height;
        // Per-character horizontal advance (≈ font size for CJK), used to limit
        // column height based on actual character count rather than cluster advances.
        int avg_char_advance = 0;
        for (int k = 0; k < fmt->m_length; k++) {
            if (fmt->m_srcs[k] && !(fmt->m_srcs[k]->flags & LTEXT_SRC_IS_OBJECT)) {
                if (fmt->m_srcs[k]->t.font)
                    avg_char_advance = ((LVFont*)fmt->m_srcs[k]->t.font)->getSize();
                break;
            }
        }
        if (avg_char_advance <= 0)
            avg_char_advance = fmt->m_pbuffer->strut_height;

        // split paragraph into lines, export lines
        int pos = 0;

        bool is_css_first_line = fmt->m_srcs[0] ? (fmt->m_srcs[0]->flags & LTEXT_IS_FIRST_LINE_CLONE) : false;

        #if (USE_LIBUNIBREAK!=1)
        int upSkipPos = -1;
        #endif

        while ( pos<fmt->m_length ) { // each loop makes a line (column)
            // y is this line indent (top offset in vertical layout).
            // In vertical-rl, this is the offset from the top of the column.
            int y;
            if (para->flags & LTEXT_LEGACY_RENDERING) {
                y = para->indent > 0 ? (pos == 0 ? para->indent : 0 ) : (pos==0 ? 0 : -para->indent);
            } else {
                y = fmt->m_indent_current;
                if ( !fmt->m_indent_first_line_done ) {
                    fmt->m_indent_first_line_done = true;
                    fmt->m_indent_current = fmt->m_indent_after_first_line;
                }
            }
            int w0 = pos>0 ? fmt->m_advance[pos-1] : 0; // measured cumulative vertical advance at start of this line
            int lastNormalWrap = -1;
            int lastDeprecatedWrap = -1;
            int lastHyphWrap = -1;
            int lastMandatoryWrap = -1;
            int spaceReduceWidth = 0;
            int cjkReduceWidth = 0;
            int firstInlineBoxPos = -1;
            int inline_box_extra = 0; // extra column depth from inline boxes beyond avg_char_advance

            // Float handling skipped in vertical mode (Step 1).
            // TODO (Step 2+): implement vertical float positioning if needed.

            if ( fmt->m_flags[pos] & LCHAR_IS_CLUSTER_TAIL && pos > 0 ) {
                int bpos = pos - 1;
                while ( bpos > 0 && fmt->m_flags[bpos] & LCHAR_IS_CLUSTER_TAIL )
                    bpos--;
                int cluster_width = (fmt->m_advance[bpos] - (bpos > 0 ? fmt->m_advance[bpos-1] : 0));
                spaceReduceWidth -= cluster_width;
            }

            // Find candidates where end of line is possible
            bool seen_non_collapsed_space = false;
            bool seen_first_rendered_char = false;
            bool first_line_sequance_end_reached = false;
            int i;
            for ( i=pos; i<fmt->m_length; i++ ) {
                if ( fmt->m_text[i]=='\n' ) {
                    lastMandatoryWrap = i;
                    break;
                }
                if ( is_css_first_line && !(fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE) ) {
                    first_line_sequance_end_reached = true;
                    lastMandatoryWrap = i;
                    break;
                }
                lUInt16 flags = fmt->m_flags[i];
                if ( flags & LCHAR_IS_OBJECT ) {
                    if ( fmt->m_charindex[i] == FLOAT_CHAR_INDEX ) {
                        // Skip floats in vertical layout (Step 1).
                        // TODO (Step 2+): implement vertical float handling.
                        src_text_fragment_t * src = fmt->m_srcs[i];
                        src->o.objflags |= LTEXT_OBJECT_IS_FLOAT_DONE;
                        if ( i==fmt->m_length-1 ) {
                            lastNormalWrap = i;
                        }
                        continue;
                    }
                    if ( fmt->m_charindex[i] == INLINEBOX_CHAR_INDEX ) {
                        if ( firstInlineBoxPos < 0 )
                            firstInlineBoxPos = i;
                        // A ruby inline box occupies advance = N × avg_char_advance column depth,
                        // not 1 ×.  Track the excess so char_count_adv does not undercount column
                        // usage and push body chars past clip.bottom (same class of bug as P11).
                        // Use letter_spacing as actual vertical depth if set (ruby groups in
                        // vertical mode store render_w there); otherwise fall back to o.width.
                        int ibox_adv = (fmt->m_srcs[i]->letter_spacing > 0)
                                       ? (int)fmt->m_srcs[i]->letter_spacing
                                       : fmt->m_srcs[i]->o.width;
                        if ( ibox_adv > avg_char_advance )
                            inline_box_extra += ibox_adv - avg_char_advance;
                    }
                }
                if (!seen_non_collapsed_space) {
                    if (flags & LCHAR_IS_COLLAPSED_SPACE)
                        continue;
                    seen_non_collapsed_space = true;
                }
                if ( !seen_first_rendered_char ) {
                    seen_first_rendered_char = true;
                    // For vertical: images/inline-boxes that are too tall for page_height
                    // are handled by addLineVertical (Step 2).
                }
                if ( fmt->m_has_cjk && i > pos && fmt->m_kerning_mode != KERNING_MODE_DISABLED ) {
                    // CJK/non-CJK boundary spacing (same as horizontal, per jlreq)
                    if ( (fmt->m_flags[i] & LCHAR_IS_CJK) && !(fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                 !(fmt->m_flags[i-1] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                        lUInt16 props = lGetCharProps(fmt->m_text[i-1]);
                        if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                            LVFont * fnt = (LVFont *)fmt->m_srcs[i]->t.font;
                            spaceReduceWidth -= fnt->getSize() / 4;
                        }

                    }
                    else if ( (fmt->m_flags[i-1] & LCHAR_IS_CJK) && !(fmt->m_flags[i-1] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                 !(fmt->m_flags[i] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                        lUInt16 props = lGetCharProps(fmt->m_text[i]);
                        if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                            LVFont * fnt = (LVFont *)fmt->m_srcs[i-1]->t.font;
                            spaceReduceWidth -= fnt->getSize() / 4;
                        }
                    }
                }

                bool grabbedExceedingSpace = false;
                // Two-part break check:
                // 1. m_advance >= maxHeight: cumulative HarfBuzz TTB advance reached the limit.
                //    Use >= so a char whose start equals maxHeight (y0 = clip.bottom = invisible)
                //    goes to the next column instead.
                // 2. char_count_adv > maxHeight: font_size-based safety that mirrors Draw()'s
                //    effective_width = max(word->width, font_size) spacing.  When TTB advances
                //    are slightly smaller than font_size, the draw positions accumulate faster
                //    than m_advance predicts, placing glyphs past clip.bottom.
                //    IMPORTANT: only apply this safety check for CJK characters.  For Latin
                //    characters in vertical mode, m_advance is computed from HarfBuzz x_advance
                //    (fallback when vmtx is absent), so the actual advance IS m_advance.
                //    Using avg_char_advance (≈ em_size from CJK chars) for Latin chars
                //    (x_advance ≈ em/3) inflates char_count_adv by ~3×, causing premature
                //    column breaks inside Latin words (e.g. "answ|er" in "answer").
                // For inline boxes (ruby groups), o.width > avg_char_advance: inline_box_extra
                // carries the excess so char_count_adv correctly reflects total column depth.
                // For ruby inline boxes, m_advance uses o.width (block-direction), but actual
                // column depth is letter_spacing (render_w).  Add i_extra to condition 1.
                int i_extra = (fmt->m_charindex[i] == INLINEBOX_CHAR_INDEX
                               && fmt->m_srcs[i]->letter_spacing > 0)
                              ? fmt->m_srcs[i]->letter_spacing - fmt->m_srcs[i]->o.width
                              : 0;
                bool is_cjk_char = (fmt->m_flags[i] & LCHAR_IS_CJK) != 0;
                int char_count_adv = (i - pos + 1) * avg_char_advance + avg_char_advance / 2 + inline_box_extra;
                if ( y + fmt->m_advance[i]-w0 + i_extra >= maxHeight + spaceReduceWidth
                        || (is_cjk_char && y + char_count_adv > maxHeight + spaceReduceWidth) ) {
                    // ぶら下がり (行末句読点ぶら下がり): if the overflowing character is a
                    // sentence-end punctuation that must not start a new column (行頭禁則),
                    // include it in the current column and stop here.  The glyph will draw
                    // at the column bottom with its ink in the upper portion of the em-square
                    // (where +vert places 。/、), so it remains fully visible even though the
                    // trailing blank of the em-square may be clipped at clip.bottom.
                    if ( fmt->m_hanging_punctuation && isVerticalHangingChar(fmt->m_text[i]) ) {
                        int prev_adv = char_count_adv - avg_char_advance;
                        if ( y + prev_adv <= maxHeight + spaceReduceWidth ) {
                            lastNormalWrap = i;  // include this char in current column
                            i++;
                            break;
                        }
                    }
                    if ( (flags & LCHAR_IS_SPACE) && (flags & LCHAR_ALLOW_WRAP_AFTER) )
                        grabbedExceedingSpace = true;
                    else if ( flags & LCHAR_IS_CJK && lastNormalWrap < i-1 ) {
                        int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                        bool does_fit = false;
                        if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                            bool can_add_space_before, can_add_space_after;
                            int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, i+1, can_add_space_before, can_add_space_after);
                            if ( wa8 != 8 ) {
                                if ( wa8 < 0 )
                                    wa8 = -wa8;
                                w = w * wa8 / 8;
                                if ( y + fmt->m_advance[i-1]-w0 + w <= maxHeight + spaceReduceWidth ) {
                                    does_fit = true;
                                }
                                if ( !does_fit ) {
                                    bool can_add_space_before, can_add_space_after;
                                    wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, i+2 > fmt->m_length ? fmt->m_length : i+2, can_add_space_before, can_add_space_after);
                                    if ( wa8 != 8 ) {
                                        if ( wa8 < 0 )
                                            wa8 = -wa8;
                                        w = w * wa8 / 8;
                                        if ( y + fmt->m_advance[i-1]-w0 + w <= maxHeight + spaceReduceWidth ) {
                                            does_fit = true;
                                        }
                                    }
                                }
                            }
                        }
                        if ( !does_fit && w <= cjkReduceWidth ) {
                            if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                                bool can_add_space_before, can_add_space_after;
                                int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                                if ( wa8 != 8 ) {
                                    if ( wa8 < 0 )
                                        wa8 = -wa8;
                                    w = w * wa8 / 8;
                                }
                            }
                            spaceReduceWidth += w;
                            cjkReduceWidth -= w;
                            does_fit = true;
                        }
                        if ( !does_fit ) {
                            break;
                        }
                    }
                    else
                        break;
                }
                #if (USE_LIBUNIBREAK==1)
                if (flags & LCHAR_ALLOW_WRAP_AFTER) {
                    if (flags & LCHAR_DEPRECATED_WRAP_AFTER) {
                        lastDeprecatedWrap = i;
                    }
                    else {
                        lastNormalWrap = i;
                    }
                }
                #else
                if ((flags & LCHAR_ALLOW_WRAP_AFTER) || (fmt->m_flags[i] & LCHAR_IS_CJK)) {
                    bool avoidWrap = false;
                    for (int j = i+1; j < fmt->m_length; j++) {
                        if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                            if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX)
                                continue;
                            else
                                break;
                        }
                        if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) {
                            avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_BEFORE;
                            break;
                        }
                    }
                    if (!avoidWrap && i < fmt->m_length-1) {
                        for (int j = i-1; j >= 0; j--) {
                            if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                                if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX)
                                    continue;
                                else
                                    break;
                            }
                            if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) {
                                avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_AFTER;
                                break;
                            }
                        }
                    }
                    if (!avoidWrap)
                        lastNormalWrap = i;
                }
                else if ( flags & LCHAR_DEPRECATED_WRAP_AFTER ) {
                    lastDeprecatedWrap = i;
                }
                #endif
                if ( i==fmt->m_length-1 )
                    lastNormalWrap = i;
                if ( !grabbedExceedingSpace &&
                        fmt->m_pbuffer->min_space_condensing_percent != 100 &&
                        i < fmt->m_length-1 &&
                        ( fmt->m_flags[i] & LCHAR_IS_SPACE ) && !( fmt->m_flags[i] & LCHAR_LOCKED_SPACING ) &&
                        !(fmt->m_flags[i+1] & LCHAR_IS_SPACE) ) {
                    int dw = getMaxCondensedSpaceTruncation(fmt,i);
                    if ( dw>0 )
                        spaceReduceWidth += dw;
                }
                else if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                    bool can_add_space_before, can_add_space_after;
                    int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                    if ( wa8 != 8 ) {
                        if ( wa8 < 0 ) {
                            wa8 = -wa8;
                            int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                            cjkReduceWidth += w - (w * wa8 / 8);
                        }
                        else if ( wa8 > 0 ) {
                            int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                            spaceReduceWidth += w - (w * wa8 / 8);
                        }
                    }
                }
                if (grabbedExceedingSpace)
                    break;
            }

            // Glyph at i exceeds available height, or mandatory break.
            if (i<=pos)
                i = pos + 1;
            int wordpos = i-1;

            #if (USE_LIBUNIBREAK==1)
                if ( lastNormalWrap < 0 && lastDeprecatedWrap > 0 ) {
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif
            int normalWrapWidth = lastNormalWrap > 0 ? y + fmt->m_advance[lastNormalWrap]-w0 : 0;
            int unusedSpace = maxHeight - normalWrapWidth;
            int unusedPercent = maxHeight > 0 ? unusedSpace * 100 / maxHeight : 0;
            #if (USE_LIBUNIBREAK!=1)
                int deprecatedWrapWidth = lastDeprecatedWrap > 0 ? y + fmt->m_advance[lastDeprecatedWrap]-w0 : 0;
                if ( deprecatedWrapWidth > normalWrapWidth && unusedPercent > 3 ) {
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif

            // Hyphenation
            tryHyphenBreak(fmt, pos, wordpos, lastNormalWrap, lastMandatoryWrap,
                           y, w0, maxHeight, spaceReduceWidth, unusedPercent, lastHyphWrap);

            // Decide best position to end this line
            int wrapPos = lastHyphWrap;
            if ( lastMandatoryWrap>=0 )
                wrapPos = lastMandatoryWrap;
            else {
                if ( wrapPos < lastNormalWrap )
                    wrapPos = lastNormalWrap;
                if ( wrapPos < 0 ) {
                    // Emergency break: no wrap opportunity found in the column.
                    // Before breaking mid-word, try to find the start of the
                    // current Latin word so we can push the whole word to the
                    // next column.  libunibreak marks AL×AL (e.g. inside
                    // "answer") as prohibited, leaving no lastNormalWrap within
                    // the word.  If the word starts after pos (there are chars
                    // before it in this column), breaking just before the word
                    // keeps the word intact.  If the word starts at pos
                    // (nothing precedes it), we cannot push it back and fall
                    // through to the per-character emergency break.
                    int word_start = i;
                    while ( word_start > pos ) {
                        lUInt16 prev_flags = fmt->m_flags[word_start - 1];
                        if ( (prev_flags & LCHAR_IS_CJK)
                                || (prev_flags & LCHAR_IS_SPACE)
                                || (prev_flags & LCHAR_IS_OBJECT) )
                            break;
                        word_start--;
                    }
                    if ( word_start > pos )
                        wrapPos = word_start - 1;   // break before the Latin word
                    else
                        wrapPos = i - 1;            // nothing before the word; split mid-word
                }
                #if (USE_LIBUNIBREAK!=1)
                if ( wrapPos <= upSkipPos ) {
                    wrapPos = upSkipPos+1;
                    upSkipPos = -1;
                }
                #endif
            }
            int endp = wrapPos + (lastMandatoryWrap<0 ? 1 : 0);

            // ぶら下げ禁則 (burasage-kinsoku): if the first char of the next column is a
            // trailing punctuation that cannot start a column (行頭禁則: 。、etc.),
            // hang it at the end of the current column rather than pushing it to the next.
            // This is standard Japanese typography: these chars may overflow the column bottom
            // by their compressed width (≤ font_size/2) rather than leaving a gap.
            #if (USE_LIBUNIBREAK==1)
            if ( lastMandatoryWrap < 0 && endp > pos && endp < fmt->m_length ) {
                lChar32 next_ch = fmt->m_text[endp];
                cjk_type_t next_type = getCJKCharType(next_ch);
                if ( next_type == cjkt_full_stop || next_type == cjkt_comma ) {
                    // Estimate how much the hung char occupies: its compressed advance
                    // is at most avg_char_advance/2 (full_stop/comma are ~50% at line end).
                    int hung_w = avg_char_advance / 2;
                    int col_used = y + fmt->m_advance[wrapPos] - w0;
                    // col_used must be STRICTLY less than maxHeight so the hung
                    // char's slot start (y0 = col_used + clip.top) is strictly
                    // below clip.bottom = maxHeight + clip.top.  When col_used
                    // equals maxHeight the glyph would start exactly at clip.bottom
                    // and be clipped to zero pixels — include it in the next column
                    // instead (it will appear at the column top, per normal layout).
                    if ( col_used < maxHeight
                            && col_used + hung_w <= maxHeight + avg_char_advance / 2 ) {
                        wrapPos = endp;
                        endp = endp + 1;
                    }
                }
            }
            // 行頭禁則 wrap-back (追い込み): chars that JLReq prohibits at column start
            // must not start a column.  In addition to closing brackets (」）】〕 etc.),
            // LuaTeX-ja's kinsoku_table also lists 長音 (ー), 繰り返し記号 (々ヽヾゝゞ〻),
            // and small kana (ぁぃぅぇぉっゃゅょゎ etc.).  Pull the break back by one
            // so the prohibited char follows normal text into the previous column.
            if ( lastMandatoryWrap < 0 && endp > pos + 1 && endp < fmt->m_length ) {
                lChar32 endp_ch = fmt->m_text[endp];
                if ( getCJKCharType(endp_ch) == cjkt_closing_bracket
                     || isVertLineStartProhibitedExt(endp_ch) ) {
                    wrapPos--;
                    endp--;
                }
            }
            // 行末禁則: 開き括弧 (「（【〔 etc.) must not end a column.
            // If the last char of the current column is an opening bracket, pull the
            // break back by one so the bracket leads off the next column instead.
            if ( lastMandatoryWrap < 0 && wrapPos > pos ) {
                if ( getCJKCharType(fmt->m_text[wrapPos]) == cjkt_opening_bracket ) {
                    wrapPos--;
                    endp--;
                }
            }
            #endif

            // CJK punctuation handling at line boundaries (same as horizontal, per jlreq)
            #if (USE_LIBUNIBREAK!=1)
            int downSkipCount = 0;
            int upSkipCount = 0;
            if (endp > 1 && isCJKLeftPunctuation(*(fmt->m_text + endp))) {
            } else if (endp > 1 && endp < fmt->m_length - 1 && isCJKLeftPunctuation(*(fmt->m_text + endp - 1))) {
                upSkipPos = endp;
                endp--; wrapPos--;
            } else if (endp > 1 && isCJKPunctuation(*(fmt->m_text + endp))) {
                for (int epos = endp; epos<fmt->m_length; epos++, downSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                }
                for (int epos = endp; epos>=start; epos--, upSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                }
                if (downSkipCount <= upSkipCount && downSkipCount <= 2 && false ) {
                   endp += downSkipCount;
                   wrapPos += downSkipCount;
                } else if (upSkipCount <= 2) {
                   upSkipPos = endp;
                   endp -= upSkipCount;
                   wrapPos -= upSkipCount;
                }
            }
            #endif
            if (endp > fmt->m_length)
                endp = fmt->m_length;

            if ( is_css_first_line ) {
                is_css_first_line = false;
                if ( first_line_sequance_end_reached ) {
                    wrapPos = fmt->m_length-1;
                }
                else {
                    lUInt16 orig_offset = 0;
                    ldomNode * orig_node = (ldomNode *) fmt->m_srcs[wrapPos]->object;
                    if ( orig_node ) {
                        orig_node = orig_node->getCloneNodeSource();
                        orig_offset = fmt->m_charindex[wrapPos];
                    }
                    int i = wrapPos;
                    while (i < fmt->m_length && fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE)
                        i++;
                    int normal_sequence_start = i;
                    bool found = false;
                    if ( orig_node ) {
                        while (i < fmt->m_length) {
                            ldomNode * node = (ldomNode *) fmt->m_srcs[i]->object;
                            if ( node == orig_node ) {
                                if ( fmt->m_charindex[i] == orig_offset ) {
                                    found = true;
                                    break;
                                }
                            }
                            i++;
                        }
                    }
                    if ( !found ) {
                        i = normal_sequence_start + wrapPos;
                    }
                    wrapPos = i;
                    if (wrapPos >= fmt->m_length)
                        wrapPos = fmt->m_length-1;
                }
            }

            bool hasInlineBoxes = firstInlineBoxPos >= 0 && firstInlineBoxPos < endp;
            addLineVertical( fmt, pos, endp, y, para, pos==0, wrapPos>=fmt->m_length-1, preFormattedOnly, isLastPara, hasInlineBoxes);
            pos = wrapPos + 1;

            #if (USE_LIBUNIBREAK==1)
            if ( fmt->m_srcs[wrapPos]->lang_cfg->duplicateRealHyphenOnNextLine() && pos > 0 && pos < fmt->m_length-1 ) {
                if ( fmt->m_text[wrapPos] == '-' || fmt->m_text[wrapPos] == UNICODE_HYPHEN ) {
                    pos--;
                    fmt->m_flags[pos] &= ~LCHAR_ALLOW_WRAP_AFTER;
                }
            }
            #endif
        }
    }

    /// handle embedded block for vertical layout
// -----------------------------------------------------------------------------
// processEmbeddedBlockVertical
// Vertical-rl sibling of processEmbeddedBlockHorizontal (lvtextfm.cpp),
// from upstream `LVFormatter::processEmbeddedBlock`.
// -----------------------------------------------------------------------------
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx )
{
    // TODO: Implement proper vertical embedded block handling
    processEmbeddedBlockHorizontal( fmt, idx );
}

// =============================================================================
// initVerticalDrawSetup
//
// Extracted from LFormattedText::Draw entry.  In vertical-rl/lr mode:
//   - DrawDocument passes (actual_Y, actual_X) as (x, y) due to the Y=X
//     coordinate mapping used throughout the rendering/page-split pipeline.
//     Swap them back to screen coordinates.
//   - line_x is the right edge of the first (rightmost) column.  The
//     pipeline-x (= draw_y = doc_y + y0) encodes the horizontal document
//     offset; clip.right − x maps it to the column's screen-X position so
//     doc_y=0 lands near clip.right (rightmost column) and increasing
//     doc_y shifts columns leftward.
//   - When the buffer's draw_extra_info has a stored vert_column_clip_right,
//     use that as the anchor instead of clip.right (which may be widened
//     by content_overflow_clip for ruby annotation overhang).
//
// Increments ltext_fmt_calls and (in vertical mode) ltext_fmt_vert_calls.
// =============================================================================
int initVerticalDrawSetup(
    formatted_text_fragment_t * pbuffer, LVDrawBuf * buf,
    int & x_inout, int & y_inout, const lvRect & clip)
{
    ltext_fmt_calls++;
    bool is_vertical = css_wm_is_vertical(pbuffer->writing_mode);
    if ( !is_vertical )
        return x_inout;
    ltext_fmt_vert_calls++;
    int tmp = x_inout;
    x_inout = y_inout;
    y_inout = tmp;
    draw_extra_info_t * dei = (draw_extra_info_t*)buf->GetDrawExtraInfo();
    int vert_anchor = (dei && dei->vert_column_clip_right)
        ? dei->vert_column_clip_right : clip.right;
    return vert_anchor - x_inout;
}

// =============================================================================
// computeVertRubyInlineBoxMetrics
//
// Extracted from measureText().  Inspects a ruby inline box's child
// structure (post-boxing: rbox1=erm_table → [rbox2_base, rbox2_annot]) to
// count base / annotation chars, find the annotation font size, and sum
// the base text's actual horizontal advances (needed for Latin-base ruby).
// Returns metrics in one struct so the caller can use them in both the
// renderBlockElement call (render_w) and the post-render advance override.
//
// Post-boxing structure:
//   inlineBox → rbox1(erm_table) → [rbox2_base(T="rbc"), rbox2_annot(T="rtc")]
// Both rbox2 elements have nodeId el_rubyBox, so we distinguish them by
// first child: rbox2_annot first child = el_rt/el_rtc → annotation row.
// =============================================================================
VertRubyInlineBoxMetrics computeVertRubyInlineBoxMetrics(
    formatted_text_fragment_t * pbuffer, ldomNode * node, LVFont * lastFont)
{
    VertRubyInlineBoxMetrics m;
    m.render_w = pbuffer->width;
    m.annot_depth = 0;
    m.base_horiz_advance_pre = 0;
    m.base_char_count_pre = 0;
    m.annot_char_count_pre = 0;
    m.annot_font_size_pre = 0;
    m.advance_per_char_pre = lastFont ? lastFont->getSize()
        : (pbuffer->strut_height > 0 ? pbuffer->strut_height : 1);
    m.vert_inline_box = (pbuffer->writing_mode == css_wm_vertical_rl ||
                         pbuffer->writing_mode == css_wm_vertical_lr);
    m.is_ruby_inline_pre = m.vert_inline_box && isRubyInlineBox(node);

    if ( m.is_ruby_inline_pre && m.advance_per_char_pre > 0 ) {
        ldomNode * rbox1_pre = node->getChildNode(0);
        int cc_pre = rbox1_pre->getChildCount();
        for ( int ci = 0; ci < cc_pre; ci++ ) {
            ldomNode * rbox2 = rbox1_pre->getChildNode(ci);
            if ( !rbox2 || !rbox2->isElement() ) continue;
            lUInt16 cid = rbox2->getNodeId();
            bool is_annot = isRubyAnnotId(cid);
            if ( !is_annot && rbox2->getChildCount() > 0 ) {
                ldomNode * fc = rbox2->getChildNode(0);
                if ( fc && fc->isElement() )
                    is_annot = isRubyAnnotId(fc->getNodeId());
            }
            if ( is_annot ) {
                lString32 t = rbox2->getText();
                for ( int k = 0; k < t.length(); k++ )
                    if ( t[k] > 0x20 ) m.annot_char_count_pre++;
                if ( m.annot_font_size_pre == 0 ) {
                    ldomNode * rt = (cid == el_rt || cid == el_rtc) ? rbox2
                                  : (rbox2->getChildCount() > 0 ? rbox2->getChildNode(0) : NULL);
                    if ( rt ) {
                        LVFontRef f = rt->getFont();
                        if ( !f.isNull() ) m.annot_font_size_pre = f->getSize();
                    }
                }
            } else {
                lString32 t = rbox2->getText();
                LVFontRef base_font = rbox2->getFont();
                bool has_font = !base_font.isNull();
                for ( int k = 0; k < t.length(); k++ ) {
                    lChar32 c = t[k];
                    if ( c > 0x20 ) {
                        m.base_char_count_pre++;
                        if ( has_font )
                            m.base_horiz_advance_pre += base_font->getCharWidth(c);
                    }
                }
            }
        }
        if ( m.base_char_count_pre < 1 ) m.base_char_count_pre = 1;
        if ( m.annot_font_size_pre == 0 && m.advance_per_char_pre > 1 )
            m.annot_font_size_pre = m.advance_per_char_pre / 2;
    } else if ( m.vert_inline_box && m.advance_per_char_pre > 0 ) {
        m.base_char_count_pre = 1;
    }
    m.annot_depth = m.annot_char_count_pre * m.annot_font_size_pre;
    if ( m.is_ruby_inline_pre && m.advance_per_char_pre > 0 ) {
        int base_depth = m.base_char_count_pre * m.advance_per_char_pre;
        m.render_w = m.annot_depth > base_depth ? m.annot_depth : base_depth;
    }
    return m;
}

// =============================================================================
// applyVerticalFrmlineDimensions
//
// Extracted from addLineHorizontal.  In vertical-rl/-lr mode, both
// frmline->height and frmline->width represent the column WIDTH on screen
// (cross-column extent).  The formatter computes them as if horizontal, so
// we override them here.  Single-image lines use the image's physical
// width so page-splitter and clip calculations see the correct horizontal
// span.
//
// Per JLReq, ruby annotations overhang into the inter-column gap rather
// than inflating the column.  The inter-column gap (strut − em) is wide
// enough to accommodate a half-em annotation without overlapping
// adjacent column text.
// =============================================================================
void applyVerticalFrmlineDimensions(LVFormatter * fmt, formatted_line_t * frmline)
{
    if ( !css_wm_is_vertical(fmt->m_writing_mode) )
        return;
    int col_width = fmt->m_pbuffer->strut_height;
    if ( frmline->word_count == 1 && (frmline->words[0].flags & LTEXT_WORD_IS_IMAGE) ) {
        col_width = (int)frmline->words[0].width;
    }
    frmline->height = col_width;
    frmline->width = col_width;
}

// =============================================================================
// applyVerticalNNRubyCenterDistribution
//
// For ruby inner annotation cells in vertical-rl: when the annotation has
// multiple chars and the slot divides evenly into sub-slots that are exactly
// 2× each annotation char width (N:N ruby with 2:1 font ratio), distribute
// chars evenly so each annotation char is centred over its corresponding
// base char (JLReq mono-ruby / 対応ルビ positioning).  Returns true if the
// special distribution was applied; caller falls back to standard centering
// otherwise.
// =============================================================================
bool applyVerticalNNRubyCenterDistribution(
    formatted_line_t * frmline, int slot_width, int extra_width)
{
    if ( (int)frmline->word_count < 2 || extra_width <= 0 || slot_width <= 0 )
        return false;
    int N = (int)frmline->word_count;
    if ( slot_width % N != 0 )
        return false;
    int sub_slot = slot_width / N;
    int char_w = (int)frmline->words[0].width;
    if ( char_w <= 0 )
        return false;
    for ( int wi = 1; wi < N; wi++ ) {
        if ( (int)frmline->words[wi].width != char_w )
            return false;
    }
    // Detect 2:1 font ratio N:N ruby: each sub-slot is exactly twice char_w.
    if ( sub_slot != 2 * char_w )
        return false;
    int next_x = 0;
    for ( int wi = 0; wi < N; wi++ ) {
        int sub_start = (wi * slot_width) / N;
        int sub_end   = ((wi + 1) * slot_width) / N;
        int sub_mid   = (sub_start + sub_end) / 2;
        int w_start   = sub_mid - char_w / 2;
        if ( w_start < next_x ) w_start = next_x;
        frmline->words[wi].x = w_start;
        next_x = w_start + char_w;
    }
    frmline->width = next_x;
    return true;
}

// =============================================================================
// drawBorderVertical
//
// Fork-only helper for vertical-rl mode.  CSS physical left/right borders
// map to thin stripes at the column's right (bdidx=1) or left (bdidx=3)
// screen-X edge, spanning y_start..y_end in screen-Y (= the element's
// inline extent).  Delegates to drawBorder() (in lvtextfm.cpp) for the
// actual draw so color/style/dot/interval resolution stays single-sourced
// — any upstream change to drawBorder() is picked up automatically.
// =============================================================================
static void drawBorderVertical(LVDrawBuf * buf, int line_x, int col_width,
                               int y_start, int y_end,
                               ldomNode * borderNode, int bdidx) {
    int bw = measureBorder(borderNode, bdidx);
    int x0, x1;
    if ( bdidx == 1 ) {          // right border → right edge of column
        x0 = line_x - bw;
        x1 = line_x;
    } else if ( bdidx == 3 ) {   // left border → left edge of column
        x0 = line_x - col_width;
        x1 = line_x - col_width + bw;
    } else {
        assert(0); // vertical helper only handles left/right
        return;
    }
    drawBorder(buf, x0, x1, y_start, y_end - y_start, borderNode, bdidx);
}

// =============================================================================
// drawVerticalPadBorders
//
// Extracted from LFormattedText::Draw's PAD border branch.  In vertical-rl
// mode, x and y are swapped: x+frmline->x+word->x is a screen-Y value
// (inline position), not screen-X.  The border spans the element's inline
// (screen-Y) extent; we scan frmline->words[] for the paired PAD to find
// the other boundary.
// =============================================================================
void drawVerticalPadBorders(
    LVDrawBuf * buf, formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int j,
    int y, int line_x, ldomNode * node,
    bool is_right_pad, bool is_mirrored)
{
    if ( is_right_pad != is_mirrored ) {
        // Right PAD marks element end; scan backward for the left PAD.
        // Use y+frmline->x (not y+x+frmline->x): x = draw_y0 is block-direction
        // (screen-X), adding it to a screen-Y value gives a wrong coordinate.
        int y_end = y + (int)frmline->x + (int)word->x;
        int y_start = y + (int)frmline->x; // fallback: frmline inline start
        for ( int k = j - 1; k >= 0; k-- ) {
            formatted_word_t * pw = &frmline->words[k];
            if ( (pw->flags & LTEXT_WORD_IS_PAD)
                  && (pbuffer->srctext[pw->src_text_index].object == srcline->object) ) {
                y_start = y + (int)frmline->x + (int)pw->x + (int)pw->width;
                break;
            }
        }
        drawBorderVertical(buf, line_x, (int)frmline->height, y_start, y_end, node, 1);
    }
    else {
        // Left PAD marks element start; scan forward for the right PAD.
        int y_start = y + (int)frmline->x + (int)word->x + (int)word->width;
        int y_end = -1;
        for ( int k = j + 1; k < frmline->word_count; k++ ) {
            formatted_word_t * pw = &frmline->words[k];
            if ( (pw->flags & LTEXT_WORD_IS_PAD)
                  && (pbuffer->srctext[pw->src_text_index].object == srcline->object) ) {
                y_end = y + (int)frmline->x + (int)pw->x;
                break;
            }
        }
        if ( y_end >= y_start )
            drawBorderVertical(buf, line_x, (int)frmline->height, y_start, y_end, node, 3);
    }
}

// =============================================================================
// applyVerticalLatinPostDraw
//
// For Latin-in-vertical words: after DrawTextString returns, advance
// vert_min_next_x by word->width.  We deliberately do NOT use the _adv
// returned by DrawTextString: measureText shapes the full line as TTB,
// DrawTextString shapes only the single word as LTR, and the resulting
// per-glyph codepoints can differ (e.g. vert-form substitution), producing
// h_advance values that disagree by ±1 px.  Using word->width keeps DRAW's
// vert_min_next_x in lockstep with LAYOUT's vert_layout_min_x mirror —
// without that lockstep, the highlight rect (positioned by word->x set in
// the post-pass) drifts away from the actual rendered glyph.
// =============================================================================
void applyVerticalLatinPostDraw(
    int _adv, int word_width, VerticalDrawState & state,
    const lvRect & clip, int y)
{
    int latin_adv = word_width;
    if ( latin_adv <= 0 )
        latin_adv = _adv;  // fallback only if word->width is unset
    state.vert_min_next_x += latin_adv;
    if ( state.vert_min_next_x > clip.bottom - y )
        state.vert_min_next_x = clip.bottom - y;
    state.vert_prev_effective_width = latin_adv;
    // Mark "prev was non-CJK" so the next CJK word inserts the Phase 5 /
    // xkanjiskip gap between Latin and CJK.  Reset CJK-class tracker since
    // the chain broke.
    state.vert_prev_was_non_cjk_word = true;
    state.vert_prev_cjk_class = -1;
}

// =============================================================================
// drawVerticalEmphasisMarks
//
// Draws 圏点 / 傍点 (text-emphasis marks, JLReq §3.3.10) in vertical-rl
// mode.  Each char in the word gets one mark in the inter-column space
// to the right of the column (= x_mark = line_x).  CSS text-emphasis-style
// values are mapped to Unicode characters per the CSS3 Text Decoration spec.
// =============================================================================
void drawVerticalEmphasisMarks(
    LVDrawBuf * buf, formatted_line_t * frmline,
    src_text_fragment_t * srcline, formatted_word_t * word,
    LVFont * font, int y, int line_x, const lvRect & clip,
    const VerticalDrawState & state)
{
    if ( !(srcline->flags & LTEXT_HAS_EXTRA) )
        return;
    int em_style = getLTextExtraProperty(srcline, LTEXT_EXTRA_CSS_TEXT_EMPHASIS);
    if ( em_style <= 0 )
        return;
    // Map style enum to Unicode mark character.
    lChar32 mark;
    switch ( em_style ) {
        case css_tes_open_dot:      mark = 0x25CB; break; // ○
        case css_tes_open_circle:   mark = 0x25CB; break; // ○
        case css_tes_filled_sesame: mark = 0xFE45; break; // ﹅
        case css_tes_open_sesame:   mark = 0xFE46; break; // ﹆
        case css_tes_filled_dc:     mark = 0x25C9; break; // ◉
        case css_tes_open_dc:       mark = 0x25CE; break; // ◎
        case css_tes_filled_tri:    mark = 0x25B2; break; // ▲
        case css_tes_open_tri:      mark = 0x25B3; break; // △
        default:                    mark = 0x25CF; break; // ● filled dot/circle
    }
    int em = font->getSize();
    // In vertical-rl, marks go to the right of each char (= "before" dir).
    int x_mark = line_x;
    // Each char occupies ~1 em of column depth.
    int char_count = word->t.len;
    int clamped_x = word->x;
    if ( clamped_x < state.vert_min_next_x - em * char_count )
        clamped_x = state.vert_min_next_x - em * char_count;
    for ( int mc = 0; mc < char_count; mc++ ) {
        int y_mark = y + frmline->x + clamped_x + mc * em;
        if ( y_mark >= clip.top && y_mark + em <= clip.bottom ) {
            font->DrawTextString(buf, x_mark, y_mark, &mark, 1, '?',
                NULL, false, NULL, 0, 0, em, 0, 0, 0);
        }
    }
}

// =============================================================================
// applyVerticalImageDraw
//
// Extracted from LFormattedText::Draw's image-word branch.  Right-aligns
// the image to the column right edge (line_x) and clamps left to 0.
// =============================================================================
void applyVerticalImageDraw(
    formatted_line_t * frmline, formatted_word_t * word,
    int y, int line_x,
    int & x0_out, int & y0_out)
{
    // In vertical-rl after the x/y swap at Draw() entry:
    //   line_x = clip.right − x = right edge of current column group.
    // word->width  = image physical width  = block-direction (screen-X) extent.
    // word->o.height = image physical height = inline-direction (screen-Y) extent.
    // Place the right edge of the image at line_x; clamp left to 0.
    x0_out = line_x - (int)word->width;
    if ( x0_out < 0 ) x0_out = 0;
    y0_out = y + (int)frmline->x + (int)word->x;
}

// =============================================================================
// applyVerticalInlineBoxDraw
//
// Extracted from LFormattedText::Draw's inline-box branch.  Sets the four
// coordinate values needed by the inner DrawDocument call, applies the
// vert_min_next_x clamping, updates per-column state, and increments fork
// diagnostic counters (ib_layout_gap, bleed) when the layout-side placement
// disagreed with the running Draw-side tracker.
//
// Coordinate semantics in vertical-rl: after the x/y swap at Draw() entry,
// `x` represents the column advance (doc_y) and `y` represents the
// screen-Y origin of the block.  For a ruby box at column offset node_y
// from the parent right edge:
//   line_x_child = parent_line_x − node_y = (clip.right − x) − node_y
//   ⇒ y0_inline = x + node_y           (so inner Draw's line_x derives correctly)
//   x0_inline   = y + node_x           (carries the screen-Y position)
//   doc_x_ib    = −node_x              (cancel inline_box.getX() in inner DrawDocument)
//   doc_y_ib    = −node_y              (cancel inline_box.getY())
// =============================================================================
void applyVerticalInlineBoxDraw(
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, int node_x, int node_y,
    int x, int y,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, int & doc_x_ib_out, int & doc_y_ib_out)
{
    // Clamp the inline box start to vert_min_next_x so it never starts
    // before the preceding character's visual end.  ib_word_x (TTB-advance-
    // based) is kept as a lower bound: if it exceeds vert_min_next_x (which
    // can happen when the inline box has its own layout position further
    // into the column), use it.
    int ib_word_x = node_x - frmline->x;
    int clamped_ib_x = ib_word_x < state.vert_min_next_x ? state.vert_min_next_x : ib_word_x;
    int clamp_delta = clamped_ib_x - ib_word_x;  // ≥ 0
    // Diagnostic: ib_word_x > vert_min_next_x means the layout placed this
    // box further than the draw tracker expected — a gap above the box.
    // Should be 0 when vert_layout_min_x correctly mirrors vert_min_next_x.
    if ( ib_word_x > state.vert_min_next_x ) {
        int gap = ib_word_x - state.vert_min_next_x;
        ltext_vert_ib_layout_gap_total += gap;
        if ( gap > ltext_vert_ib_layout_gap_max )
            ltext_vert_ib_layout_gap_max = gap;
    }
    // P14 overlap diagnostic: save the OLD vert_min_next_x (= end of the
    // preceding character) BEFORE updating it.  After update,
    // vert_min_next_x = end of THIS inline box.
    int preceding_end = y + (int)frmline->x + state.vert_min_next_x;
    x0_out = y + node_x + clamp_delta;  // draw_x_rb = y + frmline->x + clamped_ib_x
    // Use actual vertical depth (render_w from letter_spacing) so the next
    // character starts after the ruby group's visual end, preventing
    // "文字が被る" overlap.  Fall back to o.width if letter_spacing was
    // not set (non-ruby inline boxes, horizontal mode).
    int ib_actual_depth = (srcline->letter_spacing > 0)
        ? (int)srcline->letter_spacing
        : (int)word->width;
    state.vert_min_next_x = clamped_ib_x + ib_actual_depth;
    state.vert_prev_plain_y0 = y + (int)frmline->x + clamped_ib_x;
    state.vert_prev_effective_width = ib_actual_depth;
    // Ruby / inline box ends the JFM class chain; the next CJK word
    // starts with no class predecessor.
    state.vert_prev_cjk_class = -1;
    state.vert_prev_was_non_cjk_word = false;
    doc_x_ib_out = 0 - node_x;       // anchor to original node_x
    // y0 = x + node_y places the inner Draw at the correct column offset
    // so the ruby base column lands at clip.right − node_y − annot_width.
    // doc_y_ib = −node_y cancels inline_box.getY()=node_y in DrawDocument
    // so children see doc_y = 0 at this level.
    y0_out = x + node_y;
    doc_y_ib_out = 0 - node_y;
    // draw_x_inner = x0 + doc_x_ib + node_x = y + node_x + clamp_delta
    // (DrawDocument accumulates doc_x += inline_box.getX() = node_x).
    // If draw_x_inner < preceding_end, ruby overlaps the char above.
    int draw_x_inner = x0_out + doc_x_ib_out + node_x;
    if ( draw_x_inner < preceding_end ) {
        ltext_vert_bleed_count++;
        int overlap_px = preceding_end - draw_x_inner;
        if ( overlap_px > ltext_vert_bleed_max_px )
            ltext_vert_bleed_max_px = overlap_px;
    }
}

// =============================================================================
// applyVerticalWordDraw
//
// Extracted from LFormattedText::Draw's per-word loop (~190 lines of
// fork-only code).  Handles all three vertical-mode positioning paths
// (TCY, Latin-rotated, plain CJK) plus the drawFlags setup that selects
// which DrawTextString path the caller will invoke.  Maintains the
// per-frmline VerticalDrawState (vert_min_next_x, vert_prev_*) which is
// also touched by inline-box draw and post-DrawTextString Latin advance
// in the caller.
//
// Vertical positioning is fundamentally different from upstream's
// horizontal `x0 = x + frmline->x + word->x; y0 = line_y + ...`:
//   - vertical-rl uses line_x (column right edge) and frmline->height
//     as the column slot, with x0 = line_x − frmline->height (column left).
//   - y in the column is driven by vert_min_next_x (a running tracker)
//     instead of word->x, because Phase 5 inter-class glue + xkanjiskip
//     shifts are applied between chars without being baked into word->x.
//
// Caller responsibilities (still in lvtextfm.cpp Draw):
//   - drawFlags &= LTEXT_TD_MASK + WORD_FLAGS_TO_FNT_FLAGS before call
//   - check is_vertical && !LTEXT_MATH_TRANSFORM before call
//   - after DrawTextString, update state.vert_min_next_x for Latin words
//     (using word->width — see comment at the call site)
// =============================================================================
void applyVerticalWordDraw(
    formatted_text_fragment_t * pbuffer,
    formatted_line_t * frmline, src_text_fragment_t * srcline,
    formatted_word_t * word, LVFont * font,
    int y, int line_x, const lvRect & clip,
    lUInt32 & drawFlags,
    VerticalDrawState & state,
    int & x0_out, int & y0_out, bool & vert_skip_draw_out,
    bool & word_is_latin_in_vertical_out, bool & word_is_vert_mark_out)
{
    vert_skip_draw_out = false;

    // Classify the word: vertical mark, Latin-in-vertical, TCY, or plain CJK.
    // Japanese horizontal marks (―, —, …, 〜, etc.) are below U+2E80 and would
    // default to the Latin-rotated path; route them through the CJK +vert path.
    bool word_is_vert_mark =
        !(word->flags & LTEXT_WORD_IS_TCY)
        && isWordAllVertRotationChars(srcline->t.text + word->t.start, (int)word->t.len);
    bool word_is_latin_in_vertical =
           !(word->flags & LTEXT_WORD_IS_TCY)
        && !(word->flags & LTEXT_WORD_IS_CJK)
        && !(word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)
        && !(word->flags & LTEXT_WORD_IS_IMAGE)
        && !(word->flags & LTEXT_WORD_IS_INLINE_BOX)
        && !word_is_vert_mark;
    word_is_latin_in_vertical_out = word_is_latin_in_vertical;
    word_is_vert_mark_out         = word_is_vert_mark;

    // Set vertical drawFlags (mutually exclusive after the first if).
    if ( !(word->flags & LTEXT_WORD_IS_TCY) && !word_is_latin_in_vertical )
        drawFlags |= LFNT_HINT_IS_VERTICAL;
    if ( word_is_latin_in_vertical )
        drawFlags |= LFNT_HINT_RENDER_ROTATE_FOR_VERTICAL;
    if ( word_is_vert_mark )
        drawFlags |= LFNT_HINT_VERTICAL_MARK;

    if ( word->flags & LTEXT_WORD_IS_TCY ) {
        // TCY (tate-chu-yoko): draw text horizontally within vertical column.
        // The span occupies 1 em of column depth; text is centred in the column.
        int em = font->getSize();
        int clamped_x = (int)word->x < state.vert_min_next_x ? state.vert_min_next_x : (int)word->x;
        x0_out = line_x - frmline->height + (frmline->height - em) / 2;
        int y_slot_start = y + frmline->x + clamped_x;
        y0_out = y_slot_start + (em - font->getHeight()) / 2;
        state.vert_min_next_x = clamped_x + em;
        state.vert_prev_plain_y0 = y_slot_start;
        if (y_slot_start + em > clip.bottom)
            vert_skip_draw_out = true;
        return;
    }
    if ( word_is_latin_in_vertical ) {
        // Non-CJK word in vertical column: render horizontally then rotate
        // 90° CW as a single block.  vert_min_next_x is updated to the start
        // of the rotated block; the caller updates it past the block end
        // after DrawTextString returns (using word->width).
        int font_h = font->getHeight();
        // Phase 5 xkanjiskip for CJK → non-CJK boundary: insert 0.25em
        // BEFORE the Latin block when the previous word was CJK.
        if ( !state.vert_prev_was_non_cjk_word && state.vert_prev_plain_y0 >= 0 ) {
            state.vert_min_next_x += font->getSize() / 4;  // 0.25em
        }
        x0_out = line_x - frmline->height + (frmline->height - font_h) / 2;
        y0_out = y + frmline->x + state.vert_min_next_x;
        state.vert_prev_plain_y0 = y0_out;
        if ( y0_out >= clip.bottom )
            vert_skip_draw_out = true;
        return;
    }
    // Plain CJK (and vertical-mark) word in vertical column.
    //
    // For vertical-rl: line_x is the column's RIGHT edge.  frmline->height
    // is the column width.  Using line_x - frmline->height places the
    // glyph's LEFT edge at the column's LEFT boundary, so full-width CJK
    // glyphs fit exactly [line_x - col_width, line_x] with no overflow.
    //
    // word->y encodes horizontal-mode baseline alignment (small chars
    // pushed downward in horizontal text).  In vertical-rl this must NOT
    // shift the glyph leftward inside the column.  Plain-text words ignore
    // it; only inline-box words use word->y to select sub-column position.
    x0_out = line_x - frmline->height;
    // Centre plain-text chars on the column axis (JLReq 組版).  Skip when
    // the column is inflated by a ruby inline box (frmline->height > strut)
    // — adding (strut-em)/2 would push the glyph into the annotation zone.
    {
        int em    = font->getSize();
        int strut = pbuffer->strut_height;
        if ( (int)frmline->height <= strut && em < strut )
            x0_out += (strut - em) / 2;
    }
    // Phase 5 xkanjiskip for non-CJK → CJK boundary: insert 0.25em
    // (= 2/8 em) BEFORE this CJK word when the previous word was non-CJK.
    if ( state.vert_prev_was_non_cjk_word ) {
        state.vert_min_next_x += font->getSize() / 4;  // 0.25em
    }
    // Phase 5 CJK ↔ CJK inter-class glue: look up jfm-ujisv.lua [N].glue[M]
    // for (prev_cjk_class, curr_cjk_class) and shift vert_min_next_x BEFORE
    // this glyph.  word->width stays at the compacted slot width so the
    // highlight rect matches the glyph; glue renders as empty space between.
    JLReqVertClass curr_cjk_class = JLREQ_VERT_OTHER;
    if ( srcline->t.text && word->t.len > 0 ) {
        curr_cjk_class = getJLReqVertClass(srcline->t.text[word->t.start]);
    }
    if ( state.vert_prev_cjk_class >= 0 ) {
        int eighths = getJLReqGlueKernEighths(
                (JLReqVertClass)state.vert_prev_cjk_class, curr_cjk_class);
        if ( eighths > 0 ) {
            state.vert_min_next_x += ((int)font->getSize() * eighths) / 8;
        }
    }
    int clamped_x = state.vert_min_next_x;
    y0_out = y + frmline->x + clamped_x;
    // Advance vert_min_next_x.  Skip the font_size clamp for half-em JFM
    // chars: Phase 3 intentionally sets word->width = em/2 for class
    // [1][2][3][4][7], and clamping would re-introduce a half-em gap.
    int font_size = font->getSize();
    bool is_half_em_jfm_d = false;
    if ( srcline->t.text && word->t.len > 0 ) {
        JLReqVertLayout layout = getJLReqVertLayout(
            getJLReqVertClass(srcline->t.text[word->t.start]) );
        is_half_em_jfm_d = (layout.width_halves == 1);
    }
    int effective_width = ((int)word->width > font_size || is_half_em_jfm_d)
        ? (int)word->width : font_size;
    state.vert_min_next_x = clamped_x + effective_width;
    // CJK word processed: reset "prev was non-CJK" flag and remember this
    // CJK char's JFM class for the next char's inter-class glue lookup.
    state.vert_prev_was_non_cjk_word = false;
    state.vert_prev_cjk_class = (int)curr_cjk_class;
    // Cap vert_min_next_x at the column height so compressed punctuation
    // (。、 with TTB < font_size) cannot push the next char past clip.bottom.
    if ( state.vert_min_next_x > clip.bottom - y )
        state.vert_min_next_x = clip.bottom - y;
    // Character overlap detection in column direction (P14 / "文字が被る").
    if ( state.vert_prev_plain_y0 >= 0 && y0_out < clip.bottom ) {
        int slot_end_prev = state.vert_prev_plain_y0 + state.vert_prev_effective_width;
        int overlap_px = slot_end_prev - y0_out;
        if ( overlap_px > 0 ) {
            ltext_vert_char_overlap_count++;
            if ( overlap_px > ltext_vert_char_overlap_max_px )
                ltext_vert_char_overlap_max_px = overlap_px;
        }
    }
    state.vert_prev_plain_y0 = y0_out;
    state.vert_prev_effective_width = effective_width;
    // Skip when slot start is at/past clip.bottom; descent can legitimately
    // extend a few px past clip.bottom for the last char (buf->Draw clips it).
    if ( y0_out >= clip.bottom )
        vert_skip_draw_out = true;
}
