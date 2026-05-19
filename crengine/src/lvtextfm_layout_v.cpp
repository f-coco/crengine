
#define MIN_WORD_LEN_TO_HYPHENATE 4
#define MAX_WORD_SIZE 64

// Forward declarations (defined in lvtextfm_layout_h.cpp)
void addLineHorizontal( LVFormatter* fmt, int start, int end, int x, src_text_fragment_t * para, bool first, bool last, bool preFormattedOnly, bool isLastPara, bool hasInlineBoxes );

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
                        int ibox_adv = fmt->m_srcs[i]->o.width; // set by measureText()
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
                // For inline boxes (ruby groups), o.width > avg_char_advance: inline_box_extra
                // carries the excess so char_count_adv correctly reflects total column depth.
                int char_count_adv = (i - pos + 1) * avg_char_advance + avg_char_advance / 2 + inline_box_extra;
                if ( y + fmt->m_advance[i]-w0 >= maxHeight + spaceReduceWidth
                        || y + char_count_adv > maxHeight + spaceReduceWidth ) {
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
                    int dw = getMaxCondensedSpaceTruncationHorizontal(fmt,i);
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
            if ( lastMandatoryWrap<0 && lastNormalWrap<fmt->m_length-1 && unusedPercent > fmt->m_pbuffer->unused_space_threshold_percent ) {
                #ifdef DEBUG_HYPH_EXTRA_LOOPS
                    int debug_loop_num = 0;
                #endif
                int wordpos_min = lastNormalWrap > pos ? lastNormalWrap : pos;
                while ( wordpos > wordpos_min ) {
                    if ( fmt->m_srcs[wordpos]->flags & LTEXT_SRC_IS_OBJECT ) {
                        wordpos--;
                        continue;
                    }
                    #ifdef DEBUG_HYPH_EXTRA_LOOPS
                        debug_loop_num++;
                        if (debug_loop_num > 1)
                            printf("  hyphen extra loop %d\n", debug_loop_num);
                    #endif
                    int wstart, wend;
                    bool has_rtl;
                    lStr_findWordBounds( fmt->m_text, fmt->m_length, wordpos, wstart, wend, has_rtl );
                    if ( wend <= lastNormalWrap ) {
                        break;
                    }
                    int len = wend - wstart;
                    if ( len < MIN_WORD_LEN_TO_HYPHENATE || has_rtl ) {
                        wordpos = wstart - 1;
                        continue;
                    }
                    if ( len > MAX_WORD_SIZE )
                        len = MAX_WORD_SIZE;
                    lUInt8 * flags = (lUInt8*) (fmt->m_flags + wstart);
                    static lUInt16 widths[MAX_WORD_SIZE];
                    int wordStart_w = wstart>0 ? fmt->m_advance[wstart-1] : 0;
                    for ( int i=0; i<len; i++ ) {
                        widths[i] = fmt->m_advance[wstart+i] - wordStart_w;
                    }
                    int max_width = maxHeight + spaceReduceWidth - (y + (wordStart_w - w0));
                    int _hyphen_width = 0;
                    for ( int i=wstart; i<wend; i++ ) {
                        if ( !(fmt->m_srcs[i]->flags & LTEXT_SRC_IS_OBJECT) ) {
                            _hyphen_width = ((LVFont*)fmt->m_srcs[i]->t.font)->getHyphenWidth();
                            break;
                        }
                    }
                    if ( fmt->m_srcs[wordpos]->lang_cfg->getHyphMethod()->hyphenate(fmt->m_text+wstart, len, widths, flags, _hyphen_width, max_width, 2) ) {
                        for ( int i=0; i<len; i++ ) {
                            if ( fmt->m_flags[wstart+i] & LCHAR_ALLOW_HYPH_WRAP_AFTER ) {
                                if ( widths[i] + _hyphen_width > max_width ) {
                                    TR("hyphen found, but max height reached at char %d", i);
                                    fmt->m_flags[wstart+i] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                                }
                                else if ( wstart + i > pos+1 ) {
                                    if ( lastHyphWrap >= 0 ) {
                                        fmt->m_flags[lastHyphWrap] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                                    }
                                    lastHyphWrap = wstart + i;
                                }
                                else if ( wstart + i >= pos ) {
                                    fmt->m_flags[wstart+i] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                                }
                            }
                        }
                        if ( lastHyphWrap >= 0 ) {
                            break;
                        }
                    }
                    TR("no hyphen found - max_height=%d", max_width);
                    wordpos = wstart - 1;
                }
            }

            // Decide best position to end this line
            int wrapPos = lastHyphWrap;
            if ( lastMandatoryWrap>=0 )
                wrapPos = lastMandatoryWrap;
            else {
                if ( wrapPos < lastNormalWrap )
                    wrapPos = lastNormalWrap;
                if ( wrapPos < 0 )
                    wrapPos = i-1;
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
            // 行頭禁則 wrap-back (追い込み): 閉じ括弧 (」）】〕 etc.) must not start a column.
            // If the first char of the next column is a closing bracket, pull the break
            // back by one so the bracket follows normal text into the next column.
            if ( lastMandatoryWrap < 0 && endp > pos + 1 && endp < fmt->m_length ) {
                if ( getCJKCharType(fmt->m_text[endp]) == cjkt_closing_bracket ) {
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
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx )
{
    // TODO: Implement proper vertical embedded block handling
    processEmbeddedBlockHorizontal( fmt, idx );
}
