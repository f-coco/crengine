
#define MIN_WORD_LEN_TO_HYPHENATE 4
#define MAX_WORD_SIZE 64

// Vertical-rl bleed counters: accessible from cre.cpp via extern.
// Reset via ltext_reset_vert_bleed(); read via ltext_get_vert_bleed().
int ltext_vert_bleed_count = 0;
int ltext_vert_bleed_max_px = 0;

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


    /// align line: add or reduce widths of spaces to achieve desired text alignment
void alignLineHorizontal( LVFormatter* fmt, formatted_line_t * frmline, int alignment, int rightIndent=0, bool hasInlineBoxes=false ) {
        // Fetch current line x offset and max width
        int x_offset;
        int width = fmt->getAvailableWidthAtY(fmt->m_line_advance, fmt->m_pbuffer->strut_height, x_offset);
        // printf("alignLine %d+%d < %d\n", frmline->x, frmline->width, width);

        // For vertical text, getAvailableWidthAtY returns the column length
        // (page_height-strut) so chars wrap correctly at column boundaries.
        // But for inner blocks (e.g., ruby annotation cells) using that
        // page-sized avail would cause text-align:center/right to over-shift
        // by hundreds of pixels.  Use the actual block width for alignment.
        //
        // render_w is estimated from font->getSize()*N, but HarfBuzz advances
        // can be 1 px/char larger due to font-metric rounding, so
        // frmline->width may slightly exceed m_pbuffer->width.  Use
        // max(m_pbuffer->width, frmline->width) as the effective slot width so
        // that JLReq-correct symmetric overhang is preserved: when annotation
        // is wider than base, the annotation fills its own (larger) slot with
        // zero shift, and the base is centred inside that slot — giving equal
        // overhang on both sides.  Never fall back to page_height.
        bool is_vert = (fmt->m_pbuffer->writing_mode == css_wm_vertical_rl ||
                        fmt->m_pbuffer->writing_mode == css_wm_vertical_lr);
        // Track whether we clamped to an inner ruby cell's declared slot width.
        // When true, CENTER alignment uses round-half-up so that annotation and
        // base cells both land on the same integer pixel center (JLReq §3.3.8).
        bool is_inner_vert_cell = false;
        if ( is_vert && fmt->m_pbuffer->width > 0
                     && fmt->m_pbuffer->width < width ) {
            is_inner_vert_cell = true;
            width = fmt->m_pbuffer->width;
            // If content is slightly wider than the estimated cell (e.g. due to
            // HarfBuzz rounding), use the actual content width as the slot so
            // extra_width == 0 and alignment shift is zero — the annotation
            // fills its slot symmetrically without drifting above the cell top.
            if ( (int)frmline->width > width )
                width = (int)frmline->width;
        }

        // (frmline->x may be different from x_offset when non-zero text-indent)
        int usable_width = width - (frmline->x - x_offset) - rightIndent; // remove both sides indents
        int extra_width = usable_width - frmline->width;
        // Try to correct glyphs overlap at text node boundaries (no need to do this for words inside a
        // same text node, the font kerning and HarfBuzz, while measuring, are responsible for that).
        // This should mostly do italic correction and have some effect at italic/non-italic words
        // boundaries that could overlap each other (ie. italic "f" meeting regular "T" or regular "g"
        // meeting italic "f"), but it can also help when both sides use a regular font (ie. regular "f"
        // with some negative RSB meeting superscript footnote "[3]" where "f[" could overlap).
        // We do this in 2 steps: first, see if any and how much correction is needed. And then
        // only apply the correction if we have enough extra_width for it.
        // (This needs to be done after line splitting and BiDi re-ordering to get words in visual order
        // and check their overlap: so, we couldn't reserve more space for these corrections while doing
        // line splitting; we have to compensate the space added for these corrections by reducing other
        // spaces in the line, which will make some lines having less regular word spacing.)
        // To get a way to see the layout without correction, we won't do any correction when
        // kerning is KERNING_MODE_DISABLED (we will bail out below as soon as we have a font to know
        // the current kerning mode): after all, this is some kind of kerning at text node boundaries,
        // so don't do it when kerning is explicitely off.
        // We use this first step to gather additional info about words and where to grab extra width.
        int additional_extra_width = 0; // additional extra width we could get from the allowed spaces condensing
        int over_extra_width = 0; // even more extra width we could get from even more spaces condensing
        int correction_needed_width = 0;
        for ( int i=0; i<(int)frmline->word_count; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            // We will store some computations in these temporary slots (that are not used anymore)
            word->_top_to_baseline = 0; // correction needed
            word->_baseline_to_bottom = 0; // over extra width we can steal from this word's width-min_width
            int dw = word->width - word->min_width;
            if ( dw > 0 ) {
                additional_extra_width += dw;
                // This was computed according to min_space_condensing_percent (usually 50% to 90%),
                // we can grab 10% or 1px more in case we need it.
                dw = dw / 10;
                if (dw < 1)
                    dw = 1;
                over_extra_width += dw;
                word->_baseline_to_bottom = dw;
            }
            if (i==0) { // No previous word
                continue;
            }
            formatted_word_t * prev_word = &frmline->words[i-1];
            if ( prev_word->src_text_index == word->src_text_index ) { // same text node
                continue;
            }
            if ( prev_word->distinct_glyphs <= 0 || word->distinct_glyphs <= 0 ) {
                // Image, inline box, or cursive word on either side: don't do any correction.
                // todo: we should check for overlap if only one word is cursive and the other not
                // todo: we could check if a text word overlaps over an image (considering alpha in image)
                continue;
            }
            src_text_fragment_t * prev_src = &fmt->m_pbuffer->srctext[prev_word->src_text_index];
            src_text_fragment_t * src = &fmt->m_pbuffer->srctext[word->src_text_index];
            if ( prev_src->flags & LTEXT_FLAG_PREFORMATTED && src->flags & LTEXT_FLAG_PREFORMATTED ) {
                continue; // Don't touch anything if both are pre
            }
            LVFont * prev_font = (LVFont *) prev_src->t.font;
            LVFont * font = (LVFont *) src->t.font;
            if ( fmt->m_kerning_mode == KERNING_MODE_DISABLED ) {
                break; // Don't do any correction at all
            }

            // Get enough buffer height to account for any really tall glyph (possibly overflowing
            // the font height) and combinations of big vertical-align (we could compute a smaller
            // height based on these, but a bit too lazy...)
            int some_height = prev_font->getHeight() + font->getHeight();
            int y_offset = some_height + frmline->baseline;
            int buf_height = y_offset + some_height;

            // We want at least 1px of distance, and more only with very large font sizes.
            // (With some words or contexts, we may feel we would be better with more spacing,
            // and we tried with enforcing 1/8em (thin space) or 1/24em (hair space), but this
            // could feel too large with some other words or contexts. It's safer, and enough
            // to no longer notice the overlap, to go with 1px, and 2px when font size > 80,
            // so actually 1/40em;
            int largest_font_size = font->getSize() > prev_font->getSize() ? font->getSize() : prev_font->getSize();
            int min_distance = largest_font_size / 40;
            if ( min_distance < 1 )
                min_distance = 1;
            // LVHorizontalOverlapMeasurementDrawBuf only computes horizontal distances.
            // We make non-blank pixels spread vertically by 1px too, to somehow ensure
            // glyphs don't touch vertically.
            int vertical_spread = min_distance;

            // Using a min_opacity different from 0 allows avoiding some false positive/uneeded
            // corrections (ie. between an italic word and a regular comma or plural "s").
            // (Should this min_opacity be increased in LVHorizontalOverlapMeasurementDrawBuf when
            // checking vertical spreading, ie. *2 or +64 as we go away from the original y ?)
            int min_opacity = 0x40;

            // For easier visual debugging and tuning (to avoid re-renderings), uncomment and tweak this:
            // if ( font->getHintingMode() == HINTING_MODE_DISABLED ) continue;
            // if ( font->getHintingMode() == HINTING_MODE_BYTECODE_INTERPRETOR ) vertical_spread = 0;

            LVHorizontalOverlapMeasurementDrawBuf overBuf(buf_height, true, vertical_spread, min_opacity);
            // (We keep providing flags&LTEXT_TD_MASK, which will draw any underline and such, but
            // this is not handled by LVHorizontalOverlapMeasurementDrawBuf; it feels we don't need
            // to account for them in the overlap, as underline continuation could overlap and
            // cause excessive corrections.)
            // Draw the word on the left
            lUInt32 drawFlags = (prev_src->flags & LTEXT_TD_MASK)
                              | (WORD_FLAGS_TO_FNT_FLAGS(prev_word->flags))
                              | (prev_word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK ? LFNT_HINT_CJK_ALTERED_WIDTH : 0)
                              | ((prev_word->flags & LTEXT_WORD_IS_CJK && fmt->m_pbuffer->cjk_width_scale_percent != 100) ? LFNT_HINT_CJK_SCALED_WIDTH : 0);
            prev_font->DrawTextString(
                &overBuf,
                prev_word->x,
                y_offset - prev_font->getBaseline() + prev_word->y,
                prev_src->t.text + prev_word->t.start,
                prev_word->t.len,
                '?',
                NULL,
                false,
                prev_src->lang_cfg,
                drawFlags,
                prev_src->letter_spacing + prev_word->added_letter_spacing,
                prev_word->width,
                0,
                ((prev_word->flags & LTEXT_WORD_IS_CJK && fmt->m_pbuffer->cjk_width_scale_percent != 100) ?  fmt->m_pbuffer->cjk_width_scale_percent : -1)
            );
            // Draw the word on the right
            overBuf.DrawingRight();
            drawFlags = (src->flags & LTEXT_TD_MASK)
                      | (WORD_FLAGS_TO_FNT_FLAGS(word->flags))
                      | (word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK ? LFNT_HINT_CJK_ALTERED_WIDTH : 0)
                      | ((word->flags & LTEXT_WORD_IS_CJK && fmt->m_pbuffer->cjk_width_scale_percent != 100) ? LFNT_HINT_CJK_SCALED_WIDTH : 0);
            font->DrawTextString(
                &overBuf,
                word->x,
                y_offset - font->getBaseline() + word->y,
                src->t.text + word->t.start,
                word->t.len,
                '?',
                NULL,
                false,
                src->lang_cfg,
                drawFlags,
                src->letter_spacing + word->added_letter_spacing,
                word->width,
                0,
                ((word->flags & LTEXT_WORD_IS_CJK && fmt->m_pbuffer->cjk_width_scale_percent != 100) ?  fmt->m_pbuffer->cjk_width_scale_percent : -1)
            );
            // Get the distance
            int distance = overBuf.getDistance();
            if ( distance < min_distance ) {
                word->_top_to_baseline = min_distance - distance;
                correction_needed_width += word->_top_to_baseline;
                // printf("  distance: %d (min %d) => +%d\n", distance, min_distance, word->_top_to_baseline);
            }
        }
        if ( correction_needed_width > 0 ) {
            // There are some corrections to do, and we can do all or part of them
            int available_width = extra_width + additional_extra_width;
            // If more are needed, get it from words' min_width
            int over_extra_needed = correction_needed_width - available_width;
            if ( over_extra_needed > 0 && over_extra_width > 0 ) {
                if ( over_extra_needed >= over_extra_width ) {
                    // printf("correction: using full over_extra: %d (needed: %d)\n", over_extra_width, over_extra_needed);
                    available_width += over_extra_width;
                    for ( int i=0; i<(int)frmline->word_count; i++ ) {
                        formatted_word_t * word = &frmline->words[i];
                        word->min_width -= word->_baseline_to_bottom; // use all of what we compute we could use
                    }
                }
                else {
                    available_width = correction_needed_width;
                    // printf("correction: using some over_extra: %d (avail: %d)\n", over_extra_needed, over_extra_width);
                    // Loop, grabbing 1px per word until enough
                    while ( over_extra_needed > 0 ) {
                        for ( int i=0; i<(int)frmline->word_count; i++ ) {
                            formatted_word_t * word = &frmline->words[i];
                            if ( word->_baseline_to_bottom > 0 ) {
                                word->min_width--;
                                word->_baseline_to_bottom--;
                                over_extra_needed--;
                            }
                            if ( over_extra_needed == 0 )
                                break;
                        }
                    }
                }
            }
            // printf("correction: %d (%d = %d+%d)\n", correction_needed_width, available_width, extra_width, additional_extra_width);
            int added_x = 0;
            for ( int i=1; i<(int)frmline->word_count; i++ ) {
                formatted_word_t * word = &frmline->words[i];
                word->x += added_x; // shift all words by what's previously been added
                int shift_x = 0;
                if ( word->_top_to_baseline > 0 ) {
                    if ( available_width > 0 ) {
                        shift_x = word->_top_to_baseline <= available_width ? word->_top_to_baseline : available_width;
                        available_width -= shift_x;
                    }
                }
                if ( shift_x <= 0 ) {
                    continue;
                }
                word->x += shift_x;
                added_x += shift_x;
                formatted_word_t * prev_word = &frmline->words[i-1];
                prev_word->width += shift_x;
                prev_word->min_width += shift_x;
                // To see where correction is done, show some overline on the word (also uncomment it in LFormattedText::Draw())
                // word->flags |= LTEXT_WORD__AVAILABLE_BIT_16__;
            }
            frmline->width += added_x;
            extra_width = usable_width - frmline->width;
        }

        // We might want to prevent this when LangCfg == "de" (in german,
        // letter spacing is used for emphasis)
        if ( fmt->m_pbuffer->max_added_letter_spacing_percent > 0 // only if allowed
                        && alignment == LTEXT_ALIGN_WIDTH    // only when justifying
                        && frmline->word_count > 1           // not if single word (expanded, but not taking the full width is ugly)
                        && 100 * extra_width > fmt->m_pbuffer->unused_space_threshold_percent * usable_width ) {
            // extra_width is more than 5% of usable_width: we would be added too much spacing.
            // But we're allowed to add some letter spacing intoto words to reduce spacing
            // between words.
            // (We do that only when this line is justified - we could do it too when the
            // line is left- or right-aligned, but we do not know here if this is not the
            // last line of a paragraph, left aligned, that would not need to be expanded.)
            // We loop and increase letter spacing, and we stop as soon as we are
            // under the unused_space_threshold_percent (5%). If some iteration
            // brings us below min_extra_width (spaces shrunk too much), we go
            // back to the previous letter_spacing (which may put us back with
            // the unused extra space > 5%, but that is preferable).
            //
            // First, gather some info
            int min_extra_width = 0; // negative value (from the allowed spaces condensing)
            int max_font_size = 0;
            for ( int i=0; i<(int)frmline->word_count; i++ ) {
                formatted_word_t * word = &frmline->words[i];
                // Ignore images, inline boxes, cursive words and CJK words (flexible CJK words can
                // have a min_width, but we can't steal from it as it is used for fine positionning;
                // we will also not apply any added letter spacing to CJK glyphs, as each already
                // got the extra space added - and if using this option with CJK, we'd rather have
                // them get less space added, and western/numbers get the expansion).
                if ( word->distinct_glyphs <= 0 || word->flags & LTEXT_WORD_IS_CJK )
                    continue;
                min_extra_width += word->min_width - word->width;
                src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
                LVFont * font = (LVFont *)srcline->t.font;
                int font_size = font->getSize();
                if ( font_size > max_font_size )
                    max_font_size = font_size;
                // Store this word font size in this temporary slot (that is not used anymore)
                word->_top_to_baseline = font_size;
            }
            int added_spacing = 0;
            int letter_spacing_ratio = 0;
            while ( true ) {
                letter_spacing_ratio++;
                added_spacing = 0;
                bool can_try_larger = false;
                for ( int i=0; i<(int)frmline->word_count; i++ ) {
                    formatted_word_t * word = &frmline->words[i];
                    if ( word->distinct_glyphs <= 0 || word->flags & LTEXT_WORD_IS_CJK )
                        continue;
                    // Store previous value in _baseline_to_bottom (also not used anymore) in case of
                    // excess and the need to use previous value (so we don't have to recompute it)
                    word->_baseline_to_bottom = word->added_letter_spacing;
                    // We apply letter_spacing proportionally to the font size (words
                    // in a smaller font size won't get any in the loop first steps)
                    int word_font_size = word->_top_to_baseline;
                    word->added_letter_spacing = letter_spacing_ratio * word_font_size / max_font_size;
                    int word_max_letter_spacing = word_font_size * fmt->m_pbuffer->max_added_letter_spacing_percent / 100;
                    if ( word->added_letter_spacing > word_max_letter_spacing  )
                        word->added_letter_spacing = word_max_letter_spacing;
                    else
                        can_try_larger = true;
                    added_spacing += word->distinct_glyphs * word->added_letter_spacing;
                }
                int new_extra_width = extra_width - added_spacing;
                if ( new_extra_width < min_extra_width ) { // too much added, not enough for spaces
                    // Get back values from previous step (which was fine)
                    added_spacing = 0;
                    for ( int i=0; i<(int)frmline->word_count; i++ ) {
                        formatted_word_t * word = &frmline->words[i];
                        if ( word->distinct_glyphs <= 0 || word->flags & LTEXT_WORD_IS_CJK )
                            continue;
                        word->added_letter_spacing = word->_baseline_to_bottom;
                        added_spacing += word->distinct_glyphs * word->added_letter_spacing;
                    }
                    break;
                }
                if ( !can_try_larger ) // all allowed max letter_spacing reached
                    break;
                if ( 100 * new_extra_width <= fmt->m_pbuffer->unused_space_threshold_percent * usable_width ) {
                    // < 5%, we're good
                    break;
                }
            }
            if ( added_spacing ) {
                // Fix up words positions and widths
                int shift_x = 0;
                for ( int i=0; i<(int)frmline->word_count; i++ ) {
                    formatted_word_t * word = &frmline->words[i];
                    if ( word->distinct_glyphs > 0 && !(word->flags & LTEXT_WORD_IS_CJK) ) {
                        int added_width = word->distinct_glyphs * word->added_letter_spacing;
                        if ( i == frmline->word_count-1 ) {
                            // For the last word on a justified line, we want to not see
                            // any letter_spacing added after last glyph.
                            // The font will draw it, but we just want to position this
                            // word so it's drawn outside: just remove one letter_spacing.
                            // But not if this last word gets a hyphen, or the hyphen
                            // (not part of the word but added when drawing) would be
                            // shifted to the left.
                            if ( !(word->flags & LTEXT_WORD_CAN_HYPH_BREAK_LINE_AFTER) ) {
                                added_width -= word->added_letter_spacing;
                            }
                        }
                        word->width += added_width;
                        word->min_width += added_width;
                        word->x += shift_x;
                        shift_x += added_width;
                        frmline->width += added_width;
                        extra_width -= added_width;
                    }
                    else {
                        // Images, inline box, cursive words and flexible CJK words still need to be shifted
                        word->x += shift_x;
                    }
                }
            }
        }
        extra_width = usable_width - frmline->width;

        if ( fmt->m_has_cjk && extra_width < 0 && frmline->word_count > 1
                    && frmline->words[frmline->word_count-1].flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK ) {
            // Line too wide (some space reduction is needed) and the last word is a flexible CJK.
            // With our typography rules for Traditional Chinese and Japanese, it can have a min_width
            // different from its width.
            // With Traditional Chinese, we let it have the same reduction as all others flexible words.
            // But with Japanese, in the jlreq specs, the reduction of spacing at end of line comes
            // first in the reduction priorities order, before reduction in the middle of the line.
            // Moreover, a closing bracket, comma or fullstop is to be either fullwidth or halfwidth,
            // with no value in-between allowed.
            // So, ensure all this by making the last word have its width be its min_width.
            formatted_word_t * word = &frmline->words[frmline->word_count-1];
            src_text_fragment_t * src = &fmt->m_pbuffer->srctext[word->src_text_index];
            if ( src->lang_cfg->isJapanese() ) {
                int dw = word->width - word->min_width;
                if (dw > 0) {
                    word->width = word->min_width;
                    extra_width += dw; // this might then get positive, and no reduction might be needed on other words
                }
            }
        }

        if ( extra_width < 0 && !is_vert ) {
            // line is too wide
            // reduce spaces to fit line
            // (skipped for vertical text: the formatter already limits column content
            // via char_count_adv, and space reduction shifts inline-box word->x values
            // without the vert_min_next_x clamping that protects plain characters,
            // causing ruby groups to appear shifted upward on screen)
            int extraSpace = -extra_width;
            int totalSpace = 0;
            int i;
            for ( i=0; i<(int)frmline->word_count; i++ ) {
                int dw = frmline->words[i].width - frmline->words[i].min_width;
                if (dw>0) {
                    totalSpace += dw;
                }
            }
            if ( totalSpace>0 ) {
                int delta = 0;
                for ( i=0; i<(int)frmline->word_count; i++ ) {
                    frmline->words[i].x -= delta;
                    int dw = frmline->words[i].width - frmline->words[i].min_width;
                    if (dw>0 && totalSpace>0) {
                        int n = dw * extraSpace / totalSpace;
                        totalSpace -= dw;
                        extraSpace -= n;
                        delta += n;
                        frmline->width -= n;
                        if ( frmline->words[i].flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK ) {
                            // For CJK glyphs that get their visual width modified,
                            // we need their accurate visual width to be able to
                            // reposition correctly a right aligned opening punctuation
                            // or a centered Traditional Chinese punctuation.
                            frmline->words[i].width -= n;
                            // We don't need min_width anymore: set it to 0 as a flag for
                            // debugging and drawing in color affected flexible CJK glyphs.
                            frmline->words[i].min_width = 0;
                        }
                    }
                }
            }
        }
        else if ( alignment==LTEXT_ALIGN_LEFT ) {
            // no additional alignment necessary
            // Except may be with CJK lines (the last line of a justified paragraph being left aligned)
            if ( fmt->m_has_cjk && ( fmt->m_cjk_prev_line_added_space_div > 0 || fmt->m_cjk_prev_line_added_space_mod > 0 ) ) {
                // We did add spacing to the previous line to ensure text justification (see below)
                if ( frmline->word_count >= 2 && frmline->words[0].flags & LTEXT_WORD_IS_CJK
                                              && frmline->words[1].flags & LTEXT_WORD_IS_CJK ) {
                    // 2 steps: first, check if we don't exceed the available width; if not, apply changes
                    for ( int apply=0; apply<=1; apply++ ) {
                        if ( !apply ) {
                            // Don't do it if addSpaceDiv is larger than 1/4 em (probably some excessive
                            // spacing added because of long/unbreakable western word)
                            src_text_fragment_t * src = &fmt->m_pbuffer->srctext[frmline->words[0].src_text_index];
                            LVFont * font = (LVFont *) src->t.font;
                            if ( fmt->m_cjk_prev_line_added_space_div > font->getSize() * 1/4 ) {
                                break;
                            }
                        }
                        int addSpaceDiv = fmt->m_cjk_prev_line_added_space_div;
                        int addSpaceMod = fmt->m_cjk_prev_line_added_space_mod;
                        int delta = 0;
                        for ( int i=0; i<(int)frmline->word_count; i++ ) {
                            if (apply)
                                frmline->words[i].x += delta;
                            if ( frmline->words[i].flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER ) {
                                delta += addSpaceDiv;
                                if ( addSpaceMod>0 ) {
                                    addSpaceMod--;
                                    delta++;
                                }
                            }
                            if ( !(frmline->words[i].flags & LTEXT_WORD_IS_CJK) ) {
                                // No need to apply the 1px 'rest' to each word once a non-CJK has
                                // broken the alignment: we still apply addSpaceDiv to keep some
                                // regularity on the followup content
                                addSpaceMod = 0;
                            }
                        }
                        if ( !apply ) { // First step: check only
                            if ( delta > extra_width ) {
                                // Can't ensure complete and regular same spacing as previous
                                // justified lines: don't apply any spacing tweak
                                break;
                            }
                        }
                        else {
                            frmline->width += delta;
                        }
                    }
                }
            }
        }
        else if ( alignment==LTEXT_ALIGN_CENTER ) {
            // For ruby inner annotation cells in vertical-rl: when the annotation has
            // multiple chars and the slot divides evenly into sub-slots that are exactly
            // 2× each annotation char width (N:N ruby with 2:1 font ratio), distribute
            // chars evenly so each annotation char is centred over its corresponding base
            // char (JLReq mono-ruby / 対応ルビ positioning).
            //
            // For all other cases (including the base inner cell which fills its slot,
            // or N:M ruby) use round-half-up block centering.
            bool used_even_dist = false;
            if ( is_inner_vert_cell && (int)frmline->word_count >= 2 && extra_width > 0 ) {
                int N = (int)frmline->word_count;
                int slot = fmt->m_pbuffer->width;
                if ( slot > 0 && slot % N == 0 ) {
                    int sub_slot = slot / N;
                    int char_w = (int)frmline->words[0].width;
                    bool equal_widths = (char_w > 0);
                    for ( int wi = 1; wi < N && equal_widths; wi++ )
                        if ( (int)frmline->words[wi].width != char_w ) equal_widths = false;
                    // Detect 2:1 font ratio N:N ruby: each sub-slot is exactly twice char_w.
                    if ( equal_widths && char_w > 0 && sub_slot == 2 * char_w ) {
                        int next_x = 0;
                        for ( int wi = 0; wi < N; wi++ ) {
                            int sub_start = (wi * slot) / N;
                            int sub_end   = ((wi + 1) * slot) / N;
                            int sub_mid   = (sub_start + sub_end) / 2;
                            int w_start   = sub_mid - char_w / 2;
                            if ( w_start < next_x ) w_start = next_x;
                            frmline->words[wi].x = w_start;
                            next_x = w_start + char_w;
                        }
                        frmline->width = next_x;
                        used_even_dist = true;
                    }
                }
            }
            if ( !used_even_dist ) {
                // Standard round-half-up block centering (also correct for base inner cells).
                // For vertical ruby inner cells with annotation longer than base (extra_width < 0):
                // centering would give a negative shift (annotation starts above the cell top),
                // causing the first annotation char to bleed into the character preceding the ruby
                // group.  Clamp to 0 so the annotation starts at the cell top and overflows only
                // downward (past the last base char) — less visually disruptive than upward bleed.
                int center_shift = is_inner_vert_cell ? (extra_width + 1) / 2 : extra_width / 2;
                if ( is_inner_vert_cell && center_shift < 0 )
                    center_shift = 0;
                frmline->x += center_shift;
            }
        }
        else if ( alignment==LTEXT_ALIGN_RIGHT ) {
            frmline->x += extra_width;
        }
        else {
            // LTEXT_ALIGN_WIDTH
            if ( fmt->m_has_cjk ) {
                // Reset these if we end up not needing to add space (see below)
                fmt->m_cjk_prev_line_added_space_div = 0;
                fmt->m_cjk_prev_line_added_space_mod = 0;
            }
            if ( extra_width > 0
                    && fmt->m_writing_mode != css_wm_vertical_rl
                    && fmt->m_writing_mode != css_wm_vertical_lr ) {
                // distribute additional space (skip for vertical text: within-column
                // character spacing should not be expanded; columns are already broken
                // at page_height, so extra_width would push chars past clip.bottom)
                int extraSpace = extra_width;
                int addSpacePoints = 0;
                int i;
                for ( i=0; i<(int)frmline->word_count-1; i++ ) {
                    if ( frmline->words[i].flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER )
                        addSpacePoints++;
                }
                if ( addSpacePoints>0 ) {
                    int addSpaceDiv = extraSpace / addSpacePoints;
                    int addSpaceMod = extraSpace % addSpacePoints;
                    if ( fmt->m_has_cjk ) {
                        // We are adding spacing to justify the text. Remember the spacing we are
                        // adding to this line in case the next line is the last. The last line
                        // would not be justified and wouldn't get any added spacing, which would
                        // make it look more condensed that the justified line above it. So, we'll
                        // add to this last line the same spacing added to the above line so CJK
                        // chars looks vertically aligned.
                        // We do this only if the justified line (and also above with the last left
                        // aligned line) starts with 2 CJK chars (otherwise, the alignment is already
                        // broken and no fix will help).
                        if ( frmline->word_count >= 2 && frmline->words[0].flags & LTEXT_WORD_IS_CJK
                                                      && frmline->words[1].flags & LTEXT_WORD_IS_CJK ) {
                            fmt->m_cjk_prev_line_added_space_div = addSpaceDiv;
                            fmt->m_cjk_prev_line_added_space_mod = addSpaceMod;
                        }
                    }
                    int delta = 0;
                    for ( i=0; i<(int)frmline->word_count; i++ ) {
                        frmline->words[i].x += delta;
                        if ( frmline->words[i].flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER ) {
                            delta += addSpaceDiv;
                            if ( addSpaceMod>0 ) {
                                addSpaceMod--;
                                delta++;
                            }
                        }
                    }
                    frmline->width += extraSpace;
                }
            }
        }
        if ( hasInlineBoxes ) {
            #if MATHML_SUPPORT==1
                lUInt16 needed_baseline = frmline->baseline;
                lUInt16 needed_height = frmline->height;
            #endif
            // Now that we have the final x of each word, we can update
            // the RenderRectAccessor x/y of each word that is a inlineBox
            // (needed to correctly draw highlighted text in them).
            //
            // In vertical mode we also mirror the vert_min_next_x clamping that
            // Draw() applies at render time.  Without this, getRect() returns the
            // pre-clamp layout position, causing sbox.y to sit above the rendered
            // glyph by clamp_delta (≈ 1/5 em when the ruby group nearly touches
            // the preceding character).
            bool is_vert_frmline = (fmt->m_pbuffer->writing_mode == css_wm_vertical_rl ||
                                    fmt->m_pbuffer->writing_mode == css_wm_vertical_lr);
            int vert_layout_min_x = 0;  // mirrors vert_min_next_x in Draw()
            for ( int i=0; i<frmline->word_count; i++ ) {
                formatted_word_t * wi = &frmline->words[i];
                if ( is_vert_frmline && !(wi->flags & LTEXT_WORD_IS_INLINE_BOX) ) {
                    // Plain / space word: advance vert_layout_min_x past it.
                    src_text_fragment_t * si = &fmt->m_pbuffer->srctext[wi->src_text_index];
                    if ( si->t.font ) {
                        LVFont * fi = (LVFont *)si->t.font;
                        int font_sz = fi->getSize();
                        int eff_w   = ((int)wi->width > font_sz) ? (int)wi->width : font_sz;
                        int next_x  = wi->x + eff_w;
                        if ( next_x > vert_layout_min_x )
                            vert_layout_min_x = next_x;
                    }
                    continue;
                }
                if ( frmline->words[i].flags & LTEXT_WORD_IS_INLINE_BOX ) {
                    formatted_word_t * word = &frmline->words[i];
                    src_text_fragment_t * srcline = &fmt->m_pbuffer->srctext[word->src_text_index];
                    ldomNode * node = (ldomNode *) srcline->object;
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
                        // Detect ruby inline box: el_inlineBox wrapping a ruby table,
                        // whose parent <ruby> element has display:ruby.
                        // (Same check as lvtextfm.cpp measureText ruby detection.)
                        bool is_ruby_box = node
                            && node->getParentNode()
                            && node->getParentNode()->getStyle()->display == css_d_ruby
                            && node->getChildCount() > 0
                            && node->getChildNode(0)->getRendMethod() == erm_table;
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
                    node_fmt.setY( frmline->y + (is_vert ? vert_y_adjust : frmline->baseline - word->o.baseline + word->y) );
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
    }

    // Forward declaration (defined later in this file)
    int getMaxCondensedSpaceTruncation( LVFormatter* fmt, int pos );

    /// split line into words, add space for width alignment
void addLineHorizontal( LVFormatter* fmt, int start, int end, int x, src_text_fragment_t * para, bool first, bool last, bool preFormattedOnly, bool isLastPara, bool hasInlineBoxes )
    {
        // No need to do some x-alignment work if light formatting, when we
        // are only interested in computing block height and positioning
        // floats: 'is_reusable' will be unset, and any attempt at reusing
        // this formatting for drawing will cause a non-light re-formatting.
        // Except when there are inlineBoxes in the text: we need to correctly
        // position them to have their x/y saved in their RenderRectAccessor
        // (so getRect() can work accurately before the page is drawn).
        bool light_formatting = fmt->m_pbuffer->light_formatting && !hasInlineBoxes;

        // In one specific case, we don't want to have light formatting, and we need to go
        // thru alignLine() with all the lines of the paragraph: if the paragraph contains
        // CJK chars and inline boxes, and is justified and is not a single line.
        // Because of our specific handling of the alignment of CJK chars on the last line
        // with those of the previous line, we need that previous-to-last line to be non-light
        // formatted so we get accurate fmt->m_cjk_prev_line_added_space_div/_mod to position
        // any inline box in the last line (as their positions are saved in the cache).
        // As we don't and can't know here if the current line is the previous to last,
        // we go at non-light-formatting all lines...
        if ( light_formatting && fmt->m_has_cjk && fmt->m_has_inline_boxes && !(first && last)
                              && ((para->flags & LTEXT_FLAG_NEWLINE) == LTEXT_ALIGN_WIDTH) ) {
            light_formatting = false;
        }

        // todo: we can avoid some more work below when light_formatting (and
        // possibly the BiDi re-ordering we need for ordering footnotes, as
        // if we don't re-order, we'll always have them in the logical order,
        // and we can just append them in lvrend.cpp instead of checking
        // where to insert them if RTL - but we'd still have to do that
        // if some inlinebox prevent doing light formatting :(.)

        // int maxWidth = fmt->getCurrentLineWidth(); // if needed for debug printf() below

        // Provided x is the line indent: as we're making words in the visual
        // order here, it will be line start x for LTR paragraphs; but for RTL
        // ones, we'll handle it as some reserved space on the right.
        int rightIndent = 0;
        if ( fmt->m_para_dir_is_rtl ) {
            rightIndent = x;
            // maxWidth -= x; // put x/first char indent on the right: reduce width
            x = fmt->getCurrentLineX(); // use shift induced by left floats
        }
        else {
            x += fmt->getCurrentLineX(); // add shift induced by left floats
        }
        // Get overflows, needed to position first and last words
        int usable_left_overflow;
        int usable_right_overflow;
        fmt->getCurrentLineUsableOverflows(usable_left_overflow, usable_right_overflow);
        bool is_vertical_mode = (fmt->m_pbuffer->writing_mode == css_wm_vertical_rl ||
                                 fmt->m_pbuffer->writing_mode == css_wm_vertical_lr);

        // Find out text alignment to ensure for this line
        int align = para->flags & LTEXT_FLAG_NEWLINE;

        // Note that with Firefox, text-align-last applies to the first line when
        // it is also the last (so, it is used for a single line paragraph).
        // Also, when "text-align-last: justify", Firefox does justify the last
        // (or single) line.
        // We support private keywords to not behave like that for standalone lines.
        bool if_not_first = para->flags & LTEXT_LAST_LINE_IF_NOT_FIRST;
        if ( last && ( if_not_first ? !first : true ) ) { // Last line of paragraph (it is also first when standalone)
            // https://drafts.csswg.org/css-text-3/#text-align-last-property
            //  "If 'auto' is specified, content on the affected line is aligned
            //  per text-align-all unless text-align-all is set to justify,
            //  in which case it is start-aligned. All other values are
            //  interpreted as described for text-align. "
            int last_align = (para->flags >> LTEXT_LAST_LINE_ALIGN_SHIFT) & LTEXT_FLAG_NEWLINE;
            if ( last_align ) {
                // specified (or inherited) to something other than 'auto': use it
                align = last_align;
            }
            else { // text-align-last: auto (inherited default)
                // Keep using value from text-align, except when it is set to 'justify'
                if ( align == LTEXT_ALIGN_WIDTH ) {
                    // Justification is in use, and this line is the last
                    // (or a single line): align it to the left (or to the
                    // right if FriBiDi detected this paragraph is RTL)
                    align = fmt->m_para_dir_is_rtl ? LTEXT_ALIGN_RIGHT : LTEXT_ALIGN_LEFT;
                }
            }
        }

        // Override it for PRE lines (or in case align has not been set)
        if ( preFormattedOnly || !align )
            align = fmt->m_para_dir_is_rtl ? LTEXT_ALIGN_RIGHT : LTEXT_ALIGN_LEFT;

        TR("addLine(%d, %d) y=%d  align=%d", start, end, fmt->m_line_advance, align);

        // Note: parameter needReduceSpace and variable splitBySpaces (which
        // was always true) have been removed, as we always split by space:
        // even if we end up not changing spaces' widths, we need to make
        // individual words (as they may use different fonts, and for many
        // other reasons).

        // If BiDi detected, re-order line from logical order into visual order
        bool trustDirection = false;
        bool lineIsBidi = false;
        #if (USE_FRIBIDI==1)
        trustDirection = true;
        bool restore_last_width = false;
        int last_width_to_restore;
        if (fmt->m_has_bidi) {
            // We don't want to mess too much with the follow up code, so we
            // do the following, which might be expensive for full RTL documents:
            // we just reorder all chars, flags, width and references to
            // the original nodes, according to how fribidi decides the visual
            // order of chars should be.
            // We can mess with the m_* arrays (the range that spans the current
            // line) as they won't be used anymore after this function.
            // Except for the width of the last char (that we may modify
            // while zeroing the widths of collapsed spaces) that will be
            // used as the starting width of next line. We'll restore it
            // when done with this line.
            last_width_to_restore = fmt->m_advance[end-1];
            restore_last_width = true;

            // From fribidi documentation:
            // fribidi_reorder_line() reorders the characters in a line of text
            // from logical to final visual order. Note:
            // - the embedding levels may change a bit
            // - the bidi types and embedding levels are not reordered
            // - last parameter is a map of string indices which is reordered to
            //   reflect where each glyph ends up
            //
            // For re-ordering, we need some temporary buffers.
            // We use static buffers, and don't bother with dynamic buffers
            // in case we would overflow the static buffers.
            // (4096, if some glyphs spans 4 composing unicode codepoints, would
            // make 1000 glyphs, which with a small font of width 4px, would
            // allow them to be displayed on a 4000px screen.
            // Increase that if not enough.)
            #define MAX_LINE_SIZE 4096
            if ( end-start > MAX_LINE_SIZE ) {
                // Show a warning and truncate to avoid a segfault.
                printf("CRE WARNING: bidi processing line overflow (%d > %d)\n", end-start, MAX_LINE_SIZE);
                end = start + MAX_LINE_SIZE;
            }
            static lChar32 bidi_tmp_text[MAX_LINE_SIZE];
            static lUInt16 bidi_tmp_flags[MAX_LINE_SIZE];
            static src_text_fragment_t * bidi_tmp_srcs[MAX_LINE_SIZE];
            static lUInt16 bidi_tmp_charindex[MAX_LINE_SIZE];
            static int     bidi_tmp_widths[MAX_LINE_SIZE];
            // Map of string indices which is reordered to reflect where each
            // glyph ends up. Note that fribidi will access it starting
            // from 0 (and not from 'start'): this would need us to allocate
            // it the size of the full fmt->m_text (instead of MAX_LINE_SIZE)!
            // But we can trick that by providing a fake start address,
            // shifted by 'start' (which is ugly and could cause a segfault
            // if some other part than [start:end] would be accessed, but
            // we know fribid doesn't - by contract as it shouldn't reorder
            // any other part except between start:end).
            static FriBidiStrIndex bidi_indices_map[MAX_LINE_SIZE];
            for (int i=start; i<end; i++) {
                bidi_indices_map[i-start] = i;
            }
            FriBidiStrIndex * _virtual_bidi_indices_map = bidi_indices_map - start;

            FriBidiFlags bidi_flags = 0;
            // We're not using bidi_flags=FRIBIDI_FLAG_REORDER_NSM (which is mostly
            // needed for code drawing the resulting reordered result) as it would
            // mess with our indices map, and the final result would be messy.
            // (Looks like even Freetype drawing does not need the BIDI rule
            // L3 (combining-marks-must-come-after-base-char) as it draws finely
            // RTL when we draw the combining marks before base char.)
            int max_level = fribidi_reorder_line(bidi_flags, fmt->m_bidi_ctypes, end-start, start,
                                fmt->m_para_bidi_type, fmt->m_bidi_levels, NULL, _virtual_bidi_indices_map);
            if (max_level > 1) {
                lineIsBidi = true;
                // bidi_tmp_* will contain things in the visual order, from which
                // we will make words (exactly as if it had been LTR that way)
                for (int i=start; i<end; i++) {
                    int bidx = i - start;
                    int j = bidi_indices_map[bidx]; // original indice in fmt->m_text, fmt->m_flags, fmt->m_bidi_levels...
                    bidi_tmp_text[bidx] = fmt->m_text[j];
                    bidi_tmp_srcs[bidx] = fmt->m_srcs[j];
                    bidi_tmp_charindex[bidx] = fmt->m_charindex[j];
                    // Add a flag if this char is part of a RTL segment
                    if ( FRIBIDI_LEVEL_IS_RTL( fmt->m_bidi_levels[j] ) )
                        fmt->m_flags[j] |= LCHAR_IS_RTL;
                    else
                        fmt->m_flags[j] &= ~LCHAR_IS_RTL;
                    bidi_tmp_flags[bidx] = fmt->m_flags[j];
                    // bidi_tmp_widths will contains each individual char width, that we
                    // compute from the accumulated width. We'll make it a new
                    // accumulated width in next loop
                    bidi_tmp_widths[bidx] = fmt->m_advance[j] - (j > 0 ? fmt->m_advance[j-1] : 0);
                    // todo: we should probably also need to update/move the
                    // LCHAR_IS_CLUSTER_TAIL flag... haven't really checked
                    // (might be easier or harder due to the fact that we
                    // don't use FRIBIDI_FLAG_REORDER_NSM?)
                }

                // It looks like fribidi is quite good enough at taking
                // care of collapsed spaces! No real extra space seen
                // when testing, except at start and end.
                // Anyway, we handle collapsed spaces and their widths
                // as we would expect them to be with LTR text just out
                // of fmt->copyText().

                // Starting with prev_was_space=true like in fmt->copyText() may kill some
                // legitimate spaces at start of line (with some specific BiDi LTR test
                // cases, compared to Firefox that keeps some spaces).
                // But starting with prev_was_space=false keeps a space at left
                // on the first line of pure Hebrew/Arabic document paragraphs, killing
                // text justification.
                // I think I've read that the BiDi would by itself push any irrelevant spaces
                // at the end of the visual reordering, and any space still at the (visual)
                // start is relevant, and should not collapsed to nothing.
                // Given that, it feels we can go with this:
                bool prev_was_space = fmt->m_para_dir_is_rtl;
                // - in a RTL paragraph, irrelevant spaces will be on the left, and we will
                //   be collpasing/dropping them.
                // - in a LTR paragraph, only BiDi-releavant spaces should be on the left,
                //   and we will keep them.
                // If this leads to bad results, we could go with prev_was_space=true, as in
                // our context where we may favor text justification, it is preferable to avoid
                // leading and trailing spaces, rather than getting spurious ones (even if the
                // BiDi algo thinks there should be kept).

                int prev_non_collapsed_space = -1;
                int w = start > 0 ? fmt->m_advance[start-1] : 0;
                for (int i=start; i<end; i++) {
                    int bidx = i - start;
                    fmt->m_text[i] = bidi_tmp_text[bidx];
                    fmt->m_flags[i] = bidi_tmp_flags[bidx];
                    fmt->m_srcs[i] = bidi_tmp_srcs[bidx];
                    fmt->m_charindex[i] = bidi_tmp_charindex[bidx];
                    // Handle consecutive spaces at start and in the text
                    if ( (fmt->m_srcs[i]->flags & LTEXT_FLAG_PREFORMATTED) ) {
                        prev_was_space = false;
                        prev_non_collapsed_space = -1;
                        fmt->m_flags[i] &= ~LCHAR_IS_COLLAPSED_SPACE;
                    }
                    else {
                        if ( fmt->m_text[i] == ' ' ) {
                            if (prev_was_space) {
                                fmt->m_flags[i] |= LCHAR_IS_COLLAPSED_SPACE;
                                fmt->m_flags[i] &= ~LCHAR_IS_SPACE;
                                // Put this (now collapsed, but possibly previously non-collapsed)
                                // space width on the preceeding now non-collapsed space
                                int w_orig = bidi_tmp_widths[bidx];
                                bidi_tmp_widths[bidx] = 0;
                                if ( prev_non_collapsed_space >= 0 ) {
                                    fmt->m_advance[prev_non_collapsed_space] += w_orig;
                                    w += w_orig;
                                }
                            }
                            else {
                                fmt->m_flags[i] &= ~LCHAR_IS_COLLAPSED_SPACE;
                                fmt->m_flags[i] |= LCHAR_IS_SPACE;
                                prev_was_space = true;
                                prev_non_collapsed_space = i;
                            }
                        }
                        else if ( !(fmt->m_flags[i] & LCHAR_IS_TO_IGNORE) ) {
                            // (Don't update any space related state when meeting an ignorable)
                            prev_was_space = false;
                            prev_non_collapsed_space = -1;
                        }
                    }
                    w += bidi_tmp_widths[bidx];
                    fmt->m_advance[i] = w;
                    // printf("%x:f%x,w%d ", fmt->m_text[i], fmt->m_flags[i], fmt->m_advance[i]);
                }
                // Also flag as collapsed the trailing spaces on the reordered line
                // (but not if the paragraph is RTL as these are not at the visual
                // end, so not trailing, and may be relevant).
                if ( !fmt->m_para_dir_is_rtl && prev_non_collapsed_space >= 0) {
                    int prev_width = prev_non_collapsed_space > 0 ? fmt->m_advance[prev_non_collapsed_space-1] :0 ;
                    for (int i=prev_non_collapsed_space; i<end; i++) {
                        if ( !(fmt->m_flags[i] & LCHAR_IS_TO_IGNORE) )
                            fmt->m_flags[i] |= LCHAR_IS_COLLAPSED_SPACE;
                        fmt->m_advance[i] = prev_width;
                    }
                }

            }
            // Note: we reordered fmt->m_text and others, which are used from now on only
            // to properly split words. When drawing the text, these are no more used,
            // and the string is taken directly from the copy of the text node string
            // stored as src_text_fragment_t->t.text, so FreeType and HarfBuzz will
            // get the text in logical order (as HarfBuzz expects it).
            // Also, when parens/brackets are involved in RTL text, only HarfBuzz
            // will correctly mirror them. When not using Harfbuzz, we'll mirror
            // mirrorable chars below when a word is RTL.
        }
        #endif

        // Note: not certain why or how useful this lastnonspace (used below) is.
        int lastnonspace = 0;
        for ( int k=end-1; k>=start; k-- ) {
            // Also not certain if we should skip floats or LCHAR_IS_OBJECT
            if ( !(fmt->m_flags[k] & LCHAR_IS_SPACE) ) {
                lastnonspace = k;
                break;
            }
        }
        // Handle some edge case here (can't find another place where we could
        // handle it): when this line ends with a CJK char and then a space.
        // Usually, this trailing space is made part of the last word (and this
        // last word gets its width reduced so this space is like it was not there).
        // When making CJK words, we make a word on each CJK char, not knowing
        // yet what comes after, and we would end with a space-only word, that
        // would get a width of 0, but would prevent any CJK flexible width at
        // end-of-line tuning for that previous CJK char/word, as it is not at
        // end of line because of this space char...
        if ( lastnonspace < end-1 && lastnonspace >= start && (fmt->m_flags[lastnonspace] & LCHAR_IS_CJK) ) {
            end = lastnonspace+1; // ignore last space(s)
        }

        // Create/add a new line to buffer
        formatted_line_t * frmline =  lvtextAddFormattedLine( fmt->m_pbuffer );
        frmline->y = fmt->m_line_advance;
        frmline->x = x;
        // This new line starts with a minimal height and baseline, as set from the
        // paragraph parent node (by lvrend.cpp renderFinalBlock()). These may get
        // increased if some inline elements need more, but not decreased.
        frmline->height = fmt->m_pbuffer->strut_height;
        frmline->baseline = fmt->m_pbuffer->strut_baseline;
        if (fmt->m_has_ongoing_float)
            // Avoid page split when some float that started on a previous line
            // still spans this line
            frmline->flags |= LTEXT_LINE_SPLIT_AVOID_BEFORE;
        if ( lineIsBidi ) {
            // Flag that line, so createXPointer() and getRect() know it's not
            // a regular one and can't assume words and text nodes are linear.
            frmline->flags |= LTEXT_LINE_IS_BIDI;
        }
        if ( fmt->m_para_dir_is_rtl ) {
            frmline->flags |= LTEXT_LINE_PARA_IS_RTL;
            // Might be useful (we may have a bidi line in a LTR paragraph).
            // (Used for ordering in-page footnote links)
        }

        if ( preFormattedOnly && (start == end) ) {
            // Specific for preformatted text when consecutive \n\n:
            // start == end, and we have no source text to point to,
            // but we should draw en empty line (we can't just simply
            // increase fmt->m_line_advance and fmt->m_pbuffer->height, we need to have
            // a frmline as Draw() loops thru these lines - a frmline
            // with no word will do).
            src_text_fragment_t * srcline = fmt->m_srcs[start];
            if (srcline->interval > 0) { // should always be the case
                if (srcline->interval > frmline->height) // keep strut_height if greater
                    frmline->height = srcline->interval;
            }
            else { // fall back to line-height: normal
                LVFont * font = (LVFont*)srcline->t.font;
                frmline->height = font->getHeight();
            }
            fmt->m_line_advance += frmline->height;
            fmt->m_pbuffer->height = fmt->m_line_advance;
            fmt->checkOngoingFloat();
            fmt->positionDelayedFloats();
            #if (USE_FRIBIDI==1)
            if ( restore_last_width ) // bidi: restore last width to not mess with next line
                fmt->m_advance[end-1] = last_width_to_restore;
            #endif
            return;
        }

        src_text_fragment_t * lastSrc = fmt->m_srcs[start];
        // We can just skip FLOATs in addLine(), as they were taken
        // care of in processParagraph() to just reduce the available width
        // So skip floats at start:
        while (lastSrc && (lastSrc->flags & LTEXT_SRC_IS_OBJECT) && (lastSrc->o.objflags & LTEXT_OBJECT_IS_FLOAT) ) {
            start++;
            lastSrc = fmt->m_srcs[start];
        }
        if (!lastSrc) { // nothing but floats
            if (isLastPara) {
                // If this is a standalone or the last "paragraph" (floats standalone, or
                // alone after the last <br/>), make the already added line zero-height.
                frmline->height = 0;
            }
            fmt->m_line_advance += frmline->height;
            fmt->m_pbuffer->height = fmt->m_line_advance;
            fmt->checkOngoingFloat();
            fmt->positionDelayedFloats();
            #if (USE_FRIBIDI==1)
            if ( restore_last_width ) // bidi: restore last width to not mess with next line
                fmt->m_advance[end-1] = last_width_to_restore;
            #endif
            return;
        }
        // Ignore space at start of line (this rarely happens, as line
        // splitting discards the space on which a split is made - but it
        // can happen in other rare wrap cases like lastDeprecatedWrap
        // or if a wrap happened to be allowed before a no-break-space).
        // Do it only for the 2nd++ lines of a paragraph, as a leading
        // no-break-space may be used to add some indentation.
        if ( !first && (fmt->m_flags[start] & LCHAR_IS_SPACE) && !(lastSrc->flags & LTEXT_FLAG_PREFORMATTED) ) {
            start++;
            lastSrc = fmt->m_srcs[start];
        }

        // Some words vertical-align positioning might need to be fixed
        // only once the whole line has been laid out
        bool delayed_valign_computation = false;

        // Make out words, making a new one when some properties change
        int wstart = start;
        bool firstWord = true;
        bool lastWord = false;
        bool lastIsSpace = false;
        bool isSpace = false;
        bool space = false;
        // Bidi
        bool lastIsRTL = false;
        bool isRTL = false;
        bool bidiLogicalIndicesShift = false;
        // Unicode script change
        bool scriptChanged = false;
        #if (USE_HARFBUZZ==1)
            lUInt32 prevScript = HB_SCRIPT_COMMON;
            hb_unicode_funcs_t* _hb_unicode_funcs = hb_unicode_funcs_get_default();
        #endif
        // Ignorables
        bool isToIgnore = false;
        // Used when LTEXT_FIT_GLYPHS and preceeding or following word is an image or inline box
        int prev_word_overflow = 0;
        bool prev_word_is_object = false;
        for ( int i=start; i<=end; i++ ) { // loop thru each char
            src_text_fragment_t * newSrc = i<end ? fmt->m_srcs[i] : NULL;
            if ( i<end ) {
                isSpace = (fmt->m_flags[i] & LCHAR_IS_SPACE)!=0; // current char is a space
                space = lastIsSpace && !isSpace && i<=lastnonspace;
                // /\ previous char was a space, current char is not a space
                //     Note: last check was initially "&& i<lastnonspace", but with
                //     this, a line containing "thing inside a " (ending with a
                //     1-char word) would be considered only 2 words ("thing" and
                //     "inside a") and, when justify'ing text, space would not be
                //     distributed between "inside" and "a"...
                //     Not really sure what's the purpose of this last test...
                #if (USE_HARFBUZZ==1)
                    // To be done only when we met multiple scripts in a same paragraph
                    // while measuring (which we checked only when using Harfbuzz kerning)
                    if ( fmt->m_has_multiple_scripts && !(fmt->m_flags[i] & LCHAR_IS_OBJECT) ) {
                        hb_script_t script = hb_unicode_script(_hb_unicode_funcs, fmt->m_text[i]);
                        if ( script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_UNKNOWN ) {
                            if ( prevScript != HB_SCRIPT_COMMON && script != prevScript ) {
                                scriptChanged = true;
                            }
                            prevScript = script;
                        }
                    }
                #endif
                isToIgnore = fmt->m_flags[i] & LCHAR_IS_TO_IGNORE;
                isRTL = fmt->m_flags[i] & LCHAR_IS_RTL;
                bidiLogicalIndicesShift = false;
                if ( lineIsBidi && isRTL == lastIsRTL && i > 0) {
                    // The bidi algo may have reordered logical chars, and
                    // put side by side same-direction chars that where
                    // not consecutive in the original logical text.
                    // We need to make a new word when we see these
                    // reordered indices shifting by more than +/- 1,
                    // as when drawing the words, we'll use the source
                    // text nodes' logical text.
                    if ( isRTL ) { // indices should be decreasing by 1
                        if ( fmt->m_charindex[i] != fmt->m_charindex[i-1] - 1 )
                            bidiLogicalIndicesShift = true;
                    }
                    else { // LTR: indices should be increasing by 1
                        if ( fmt->m_charindex[i] != fmt->m_charindex[i-1] + 1 )
                            bidiLogicalIndicesShift = true;
                    }
                    // (fmt->m_charindex[i-1] might be bad when i-1 is from
                    // another text node, or an object - but no need
                    // for checking that as it will have triggered
                    // another condition for making a word.)
                }
            }
            else {
                lastWord = true;
            }

            // This loop goes thru each char, and create a new word when it meets:
            // - a non-space char that follows a space (this non-space char will be
            //   the first char of next word).
            // - a char from a different text node (eg: "<span>first</span>next")
            // - a CJK char (whether or not preceded by a space): each becomes a word
            // - the end of text, which makes the last word
            //
            // It so grabs all spaces (0 or 1 with our XML parser) following
            // the current real word, and includes it in the word. So a word
            // includes its following space if any, but should not start with
            // a space. The trailing space is needed for the word processing
            // code below to properly set flags and guess the amount of spaces
            // that can be increased or reduced for proper alignment.
            // Also, these words being then stacked to each other to build the
            // line, the ending space should be kept to be drawn and seen
            // between each word (some words may not be separated by space when
            // from different text nodes or CJK).
            // Note: a "word" in our current context is just a unit of text that
            // should be rendered together, and can be moved on the x-axis for
            // alignment purpose (the 2 french words "qu'autrefois" make a
            // single "word" here, the single word "quelconque", if hyphenated
            // as "quel-conque" will make one "word" on this line and another
            // "word" on the next line.
            //
            // In a sequence of collapsing spaces, only the first was kept as
            // a LCHAR_IS_SPACE. The following ones were flagged as
            // LCHAR_IS_COLLAPSED_SPACE, and thus are not LCHAR_IS_SPACE.
            // With the algorithm described just above, these collapsed spaces
            // can then only be at the start of a word.
            // Their calculated width has been made to 0, but the drawing code
            // (LFormattedText::Draw() below) will use the original srctext text
            // to draw the string: we can't override this original text (it is
            // made read-only with the use of 'const') to replace the space with
            // a zero-width char (which may not be zero-width in a monospace font).
            // So, we need to adjust each word start index to get rid of the
            // collapsed spaces.
            //
            // Note: if we were to make a space between 2 CJY chars a collapsed
            // space, we would have it at the end of each word, which may
            // be fine without additional work needed (not verified):
            // having a zero-width, it would not change the width of the
            // CJKchar/word, and would not affect the next CJKchar/word position.
            // It would be drawn as a space, but the next CJKchar would override
            // it when it is drawn next.

            if ( i>wstart && (   newSrc!=lastSrc
                              || space
                              || lastWord
                              || isRTL != lastIsRTL
                              || bidiLogicalIndicesShift
                              || scriptChanged
                              || isToIgnore
                              || (fmt->m_flags[wstart] & LCHAR_IS_CJK) // a CJK char makes its own word
                              || (fmt->m_flags[i] & LCHAR_IS_CJK) // a CJK char ends previous word
                             ) ) {
                // New HTML source node, space met just before, last word, or CJK char:
                // create and add new word with chars from wstart to i-1

                #if (USE_HARFBUZZ==1)
                    if ( fmt->m_has_multiple_scripts ) {
                        // Reset as next segment can start with any script
                        prevScript = HB_SCRIPT_COMMON;
                        scriptChanged = false;
                    }
                #endif

                // Remove any collapsed space at start of word: they
                // may have a zero width and not influence positioning,
                // but they will be drawn as a space by Draw(). We need
                // to increment the start index into the src_text_fragment_t
                // for Draw() to start rendering the text from this position.
                // Also skip floating nodes and chars flagged as to be ignored.
                while (wstart < i) {
                    if ( !(fmt->m_flags[wstart] & LCHAR_IS_COLLAPSED_SPACE) &&
                         !(fmt->m_flags[wstart] & LCHAR_IS_TO_IGNORE) &&
                            !(fmt->m_srcs[wstart]->flags & LTEXT_SRC_IS_OBJECT && fmt->m_srcs[wstart]->o.objflags & LTEXT_OBJECT_IS_FLOAT) )
                        break;
                    // printf("_"); // to see when we remove one, before the TR() below
                    wstart++;
                }
                if (wstart == i) { // word is only collapsed spaces or ignorable chars
                    // No need to create it.
                    // Except if it is the last word, and we have not yet added any:
                    // we need a word for the line to have a height (frmline->height)
                    // so that the following line is one line below the empty line we
                    // made (eg, when <br/><br/>)
                    // However, we don't do that if it would be the last empty line in
                    // the last paragraph (paragraphs here are just sections of the final
                    // block cut by <BR>): most browsers don't display the line break
                    // implied by the BR when we have: "<div>some text<br/> </div>more text"
                    // or "<div>some text<br/> <span> </span> </div>more text".
                    if (lastWord && firstWord) {
                        if (!isLastPara) {
                            wstart--; // make a single word with a single collapsed space
                            if (fmt->m_flags[wstart] & LCHAR_IS_TO_IGNORE) {
                                // In this (edgy) case, we would be rendering this char we
                                // want to ignore.
                                // This is a bit hacky, but no other solution: just
                                // replace that ignorable char with a space in the
                                // src text
                                *((lChar32 *) (fmt->m_srcs[wstart]->t.text + fmt->m_srcs[wstart]->t.offset + fmt->m_charindex[wstart])) = U' ';
                            }
                            else if (fmt->m_srcs[wstart]->flags & LTEXT_SRC_IS_OBJECT && fmt->m_srcs[wstart]->o.objflags & LTEXT_OBJECT_IS_FLOAT) {
                                // But not if what's on this line is a float (the code below don't expect floats)
                                // Keep the empty line with the strut height.
                                continue;
                            }
                        }
                        else { // Last or single para with no word
                            // A line has already been added: just make
                            // it zero height.
                            frmline->height = 0;
                            frmline->baseline = 0;
                            continue;
                            // We'll then just exit the loop as we are lastWord
                        }
                    }
                    else {
                        // no word made, get ready for next loop
                        lastSrc = newSrc;
                        lastIsSpace = isSpace;
                        lastIsRTL = isRTL;
                        continue;
                    }
                }

                // Create/add a new word to this frmline
                formatted_word_t * word = lvtextAddFormattedWord(frmline);
                src_text_fragment_t * srcline = fmt->m_srcs[wstart]; // should be identical to lastSrc
                word->src_text_index = srcline->index;

                // This LTEXT_VALIGN_ flag is now only of use with objects (images)
                int vertical_align_flag = srcline->flags & LTEXT_VALIGN_MASK;
                // These will be used later to adjust the main line baseline and height:
                int top_to_baseline = 0;   // distance from this word top to its own baseline (formerly named 'b')
                int baseline_to_bottom =0; // descender below baseline for this word (formerly named 'h')
                    // (initialized to 0 to avoid avoid "used but uninitialized" warning, but will be set when needed)
                // For each word, we'll have to check and adjust line height and baseline,
                // except when LTEXT_VALIGN_TOP and LTEXT_VALIGN_BOTTOM where it has to
                // be delayed until the full line is laid out. Until that, we store some
                // info into word->_top_to_baseline and word->_baseline_to_bottom.
                bool adjust_line_box = true;
                // We will make sure elements with "-cr-hint: strut-confined"
                // do not change the strut baseline and height
                bool strut_confined = (srcline->flags & LTEXT_STRUT_CONFINED) && fmt->m_allow_strut_confining;

                if ( srcline->flags & LTEXT_SRC_IS_OBJECT ) {
                    // object: image or inline-block box (floats have been skipped above)

                    // This is set or used only when LTEXT_FIT_GLYPHS
                    if ( prev_word_overflow ) {
                        frmline->width += prev_word_overflow;
                        frmline->words[frmline->word_count-2].width += prev_word_overflow;
                        frmline->words[frmline->word_count-2].min_width += prev_word_overflow;
                        prev_word_overflow = 0;
                    }
                    prev_word_is_object = true; // to be used when processing next word

                    word->distinct_glyphs = 0;
                    word->x = frmline->width;
                    word->width = srcline->o.width;
                    word->min_width = word->width;
                    word->o.height = srcline->o.height;
                    if ( srcline->o.objflags & LTEXT_OBJECT_IS_INLINE_BOX ) { // inline-block
                        word->flags = LTEXT_WORD_IS_INLINE_BOX;
                        // For inline-block boxes, the baseline may not be the bottom; it has
                        // been computed in fmt->measureText().
                        word->o.baseline = srcline->o.baseline;
                        top_to_baseline = word->o.baseline;
                        baseline_to_bottom = word->o.height - word->o.baseline;
                        // We can't really ensure strut_confined with inline-block boxes,
                        // or we could miss content (it would be overwritten by next lines)
                        if ( fmt->m_pbuffer->inlineboxes_links ) {
                            // The buffer has some inline boxes with footnote links.
                            // If this inline box has some, let lvrend.cpp know, so it can
                            // fetch them when adding this line to the page split context
                            lString32Collection * links;
                            lUInt32 key = ((ldomNode *) srcline->object)->getDataIndex();
                            if ( fmt->m_pbuffer->inlineboxes_links->get(key, links) ) {
                                word->flags |= LTEXT_WORD_IS_LINK_START;
                                        // we re-use this flag already used by lvrend.cpp
                            }
                        }
                    }
                    else if ( srcline->o.objflags & LTEXT_OBJECT_IS_IMAGE ) {
                        word->flags = LTEXT_WORD_IS_IMAGE;
                        // The image dimensions have already been resized to fit
                        // into fmt->m_pbuffer->width (and strut confining if requested.
                        // Note: it can happen when there is some text-indent than
                        // the image width exceeds the available width: it might be
                        // shown overflowing or overrideing other content.
                        word->width = srcline->o.width;
                        word->o.height = srcline->o.height;
                        // todo: adjust fmt->m_max_img_height with this image valign_dy/vertical_align_flag
                        // Per specs, the baseline is the bottom of the image
                        top_to_baseline = word->o.height;
                        baseline_to_bottom = 0;
                        // Flag word if that image is at the start of a link (for in-page footnotes)
                        if ( srcline->flags & LTEXT_IS_LINK ) {
                            word->flags |= LTEXT_WORD_IS_LINK_START;
                        }
                    }
                    else if ( srcline->o.objflags & LTEXT_OBJECT_IS_PAD ) {
                        word->flags = LTEXT_WORD_IS_PAD;
                        word->width = srcline->o.width;         // margin + padding + border (the full width taken)
                        word->o.height = srcline->o.height;     // padding + border (background-color extends into this)
                        word->o.baseline = srcline->o.baseline; // border thickness (for drawing it)
                        if ( fmt->m_flags[wstart] & LCHAR_IS_RTL ) {
                            // Depending on context, BiDi made this pad appears as RTL: a left pad will have
                            // to be drawn as a right pad, with padding|border|margin in this order
                            // (and conversely for a right pad)
                            word->flags |= LTEXT_WORD_DIRECTION_IS_RTL;
                        }
                    }
                    else {
                        // Should not happen
                        crFatalError(130, "Unexpected object type for word");
                    }

                    // srcline->valign_dy sets the baseline, except in a few specific cases
                    // word->y has to be set to where the baseline should be
                    // For vertical-align: top or bottom, delay computation as we need to
                    // know the final frmline height and baseline, which might change
                    // with upcoming words.
                    if ( word->flags & LTEXT_WORD_IS_PAD ) {
                        // We don't care about y/height/baseline
                        word->y = srcline->valign_dy;
                        adjust_line_box = false;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_TOP ) {
                        // was (before we delayed computation):
                        // word->y = top_to_baseline - frmline->baseline;
                        adjust_line_box = false;
                        delayed_valign_computation = true;
                        word->flags |= LTEXT_WORD_VALIGN_TOP;
                        if ( strut_confined )
                            word->flags |= LTEXT_WORD_STRUT_CONFINED;
                        word->_top_to_baseline = top_to_baseline;
                        word->_baseline_to_bottom = baseline_to_bottom;
                        word->y = top_to_baseline;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_BOTTOM ) {
                        // was (before we delayed computation):
                        // word->y = frmline->height - frmline->baseline;
                        adjust_line_box = false;
                        delayed_valign_computation = true;
                        word->flags |= LTEXT_WORD_VALIGN_BOTTOM;
                        if ( strut_confined )
                            word->flags |= LTEXT_WORD_STRUT_CONFINED;
                        word->_top_to_baseline = top_to_baseline;
                        word->_baseline_to_bottom = baseline_to_bottom;
                        word->y = - baseline_to_bottom;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_TEXT_TOP ) {
                        // srcline->valign_dy has been set to where top of image or box should be
                        word->y = srcline->valign_dy + top_to_baseline;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_TEXT_BOTTOM ) {
                        // srcline->valign_dy has been set to where bottom of image or box should be
                        word->y = srcline->valign_dy - baseline_to_bottom;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_MIDDLE ) {
                        // srcline->valign_dy has been set to where the middle of image or box should be
                        word->y = srcline->valign_dy - (top_to_baseline + baseline_to_bottom)/2 + top_to_baseline;
                    }
                    else { // otherwise, align baseline according to valign_dy (computed in lvrend.cpp)
                        word->y = srcline->valign_dy;
                    }

                    // Inline image or inline-block: ensure any "page-break-before/after: avoid"
                    // specified on them (the specs say those apply to "block-level elements
                    // in the normal flow of the root element. User agents may also apply it
                    // to other elements like table-row elements", so it's mostly assumed that
                    // they won't apply on inline elements and we'll never meet them - but as
                    // it doesn't say we should not, let's ensure them if provided - and
                    // only "avoid" as it may have some purpose to stick a full-width image
                    // or inline-block to the previous or next line).
                    ldomNode * node = (ldomNode *) srcline->object;
                    if ( node && srcline->o.objflags & LTEXT_OBJECT_IS_INLINE_BOX ) {
                        // We have not propagated page_break styles from the original
                        // inline-block to its inlineBox wrapper
                        node = node->getChildNode(0);
                    }
                    if ( node ) {
                        css_style_ref_t style = node->getStyle();
                        if ( style->page_break_before == css_pb_avoid )
                            frmline->flags |= LTEXT_LINE_SPLIT_AVOID_BEFORE;
                        if ( style->page_break_after == css_pb_avoid )
                            frmline->flags |= LTEXT_LINE_SPLIT_AVOID_AFTER;
                    }
                }
                else {
                    // word
                    // wstart points to the previous first non-space char
                    // i points to a non-space char that will be in next word
                    // i-1 may be a space, or not (when different html tag/text nodes stuck to each other)
                    word->flags = 0;

                    // Handle vertical positioning of this word
                    LVFont * font = (LVFont*)srcline->t.font;
                    int vertical_align_flag = srcline->flags & LTEXT_VALIGN_MASK;
                    int line_height = srcline->interval;
                    int fh = font->getHeight();
                    if ( strut_confined && line_height > fmt->m_pbuffer->strut_height ) {
                        // If we'll be confining text inside the strut, get rid of any
                        // excessive line-height for the following computations).
                        // But we should keep it at least fh so drawn text doesn't
                        // overflow the box we'll try to confine into the strut.
                        line_height = fh > fmt->m_pbuffer->strut_height ? fh : fmt->m_pbuffer->strut_height;
                    }
                    // As we do only +/- arithmetic, the following values being negative should be fine.
                    // Accounts for line-height (adds what most documentation calls half-leading to top
                    // and to bottom  - note that "leading" is a typography term referring to "lead" the
                    // metal, and not to lead/leader/head/header - so the half use for bottom should not
                    // be called half-tailing :):
                    int half_leading = (line_height - fh) / 2;
                    int half_leading_bottom = line_height - fh - half_leading;
                    top_to_baseline = font->getBaseline() + half_leading;
                    baseline_to_bottom = line_height - top_to_baseline;
                    // For vertical-align: top or bottom, delay computation as we need to
                    // know the final frmline height and baseline, which might change
                    // with upcoming words.
                    if ( vertical_align_flag == LTEXT_VALIGN_TOP ) {
                        // was (before we delayed computation):
                        // word->y = font->getBaseline() - frmline->baseline + half_leading;
                        adjust_line_box = false;
                        delayed_valign_computation = true;
                        word->flags |= LTEXT_WORD_VALIGN_TOP;
                        if ( strut_confined )
                            word->flags |= LTEXT_WORD_STRUT_CONFINED;
                        word->_top_to_baseline = top_to_baseline;
                        word->_baseline_to_bottom = baseline_to_bottom;
                        word->y = font->getBaseline() + half_leading;
                    }
                    else if ( vertical_align_flag == LTEXT_VALIGN_BOTTOM ) {
                        // was (before we delayed computation):
                        // word->y = frmline->height - fh + font->getBaseline() - frmline->baseline - half_leading;
                        adjust_line_box = false;
                        delayed_valign_computation = true;
                        word->flags |= LTEXT_WORD_VALIGN_BOTTOM;
                        if ( strut_confined )
                            word->flags |= LTEXT_WORD_STRUT_CONFINED;
                        word->_top_to_baseline = top_to_baseline;
                        word->_baseline_to_bottom = baseline_to_bottom;
                        word->y = - fh + font->getBaseline() - half_leading_bottom;
                    }
                    else {
                        // For others, vertical-align computation is done in lvrend.cpp renderFinalBlock()
                        word->y = srcline->valign_dy;
                    }
                    // printf("baseline_to_bottom=%d top_to_baseline=%d word->y=%d txt=|%s|\n", baseline_to_bottom,
                    //   top_to_baseline, word->y, UnicodeToLocal(lString32(srcline->t.text, srcline->t.len)).c_str());

                    // Set word start and end (start+len-1) indices in the source text node
                    if ( !fmt->m_has_bidi ) {
                        // No bidi, everything is linear
                        word->t.start = fmt->m_charindex[wstart] + srcline->t.offset;
                        word->t.len = i - wstart;
                    }
                    else if ( fmt->m_flags[wstart] & LCHAR_IS_RTL ) {
                        // Bidi and first char RTL.
                        // As we split on bidi level change, the full word is RTL.
                        // As we split on src text fragment, we are sure all chars
                        // are in the same text node.
                        // charindex may have been reordered, and may not be sync'ed with wstart/i-1,
                        // but it is linearly decreasing between i-1 and wstart
                        word->t.start = fmt->m_charindex[i-1] + srcline->t.offset;
                        word->t.len = fmt->m_charindex[wstart] - fmt->m_charindex[i-1] + 1;
                        word->flags |= LTEXT_WORD_DIRECTION_IS_RTL; // Draw glyphs in reverse order
                        #if (USE_FRIBIDI==1)
                        // If not using Harfbuzz, procede to mirror parens & al (don't
                        // do that if Harfbuzz is used, as it does that by itself, and
                        // would mirror back our mirrored chars!)
                        if ( fmt->m_kerning_mode != KERNING_MODE_HARFBUZZ ) {
                            lChar32 * str = (lChar32*)(srcline->t.text + word->t.start);
                            FriBidiChar mirror;
                            for (int i=0; i < word->t.len; i++) {
                                if ( fribidi_get_mirror_char( (FriBidiChar)(str[i]), &mirror) )
                                    str[i] = (lChar32)mirror;
                            }
                        }
                        #endif
                    }
                    else {
                        // Bidi and first char LTR. Same comments as above, except for last one:
                        // it is linearly increasing between wstart and i-1
                        word->t.start = fmt->m_charindex[wstart] + srcline->t.offset;
                        word->t.len = fmt->m_charindex[i-1] + 1 - fmt->m_charindex[wstart];
                    }

                    // Flag word that are the start of a link (for in-page footnotes)
                    if ( word->t.start==srcline->t.offset && srcline->flags & LTEXT_IS_LINK ) {
                        word->flags |= LTEXT_WORD_IS_LINK_START;
                        // todo: we might miss some links if the source text starts with a space
                    }

                    // Below this are stuff that could be skipped if light_formatting
                    // (We need bidi and the above adjustment only to get correctly ordered
                    // in-page footnotes links.)

                    // For Harfbuzz, which may shape differently words at start or end of paragraph.
                    // todo: this is probably wrong if some multi bidi levels re-ordering has been done
                    if ( first ) { // first line of paragraph
                        if ( fmt->m_para_dir_is_rtl ? lastWord : firstWord )
                            word->flags |= LTEXT_WORD_BEGINS_PARAGRAPH;
                    }
                    if ( last ) { // last line of paragraph
                        if ( fmt->m_para_dir_is_rtl ? firstWord : lastWord )
                            word->flags |= LTEXT_WORD_ENDS_PARAGRAPH;
                    }
                    if ( trustDirection)
                        word->flags |= LTEXT_WORD_DIRECTION_KNOWN;

                    // We need to compute how many glyphs can have letter_spacing added, that
                    // might be done in alignLine() (or not). We have to do it now even if
                    // not used, as we won't have that information anymore in alignLine().
                    word->added_letter_spacing = 0;
                    word->distinct_glyphs = word->t.len; // start with all chars are distinct glyphs
                    bool seen_non_space = false;
                    int tailing_spaces = 0;
                    for ( int j=i-1; j >= wstart; j-- ) {
                        if ( fmt->m_flags[j] & LCHAR_LOCKED_SPACING ) {
                            // A single char flagged with this makes the whole word non tweakable
                            word->distinct_glyphs = 0;
                            tailing_spaces = 0; // prevent tailing spaces correction
                            break;
                        }
                        if ( !seen_non_space && (fmt->m_flags[j] & LCHAR_IS_SPACE) ) {
                            // We'd rather not include the space that ends most words.
                            word->distinct_glyphs--;
                            // But some words can be made of a single space, that we'd rather
                            // not ignore when adjusting spacing.
                            tailing_spaces++;
                            continue;
                        }
                        seen_non_space = true;
                        if ( fmt->m_flags[j] & (LCHAR_IS_CLUSTER_TAIL|LCHAR_IS_COLLAPSED_SPACE|LCHAR_IS_TO_IGNORE) ) {
                            word->distinct_glyphs--;
                        }
                    }
                    if ( !seen_non_space && tailing_spaces ) {
                        word->distinct_glyphs += tailing_spaces;
                    }

                    if ( i - wstart == 1 && (fmt->m_flags[wstart] & LCHAR_IS_CJK) ) {
                        word->flags |= LTEXT_WORD_IS_CJK;
                    }

                    // If we're asked to fit glyphs (avoid glyphs from overflowing line edges and
                    // on neighbour text nodes), we might need to tweak words x and width
                    bool fit_glyphs = srcline->flags & LTEXT_FIT_GLYPHS;

                    if ( fit_glyphs && !firstWord && prev_word_is_object ) {
                        int lsb = font->getLeftSideBearing(fmt->m_text[wstart]);
                        if ( lsb < 0 ) {
                            // Prev word was an image or inline box: avoid first glyph
                            // from overflowing in it by shifting this new word start
                            // on the right
                            frmline->width += -lsb;
                        }
                    }

                    if ( firstWord && (align == LTEXT_ALIGN_LEFT || align == LTEXT_ALIGN_WIDTH) ) {
                        // Adjust line start x if needed
                        // No need to do it when line is centered or right aligned (doing so
                        // might increase the line width and change space widths for no reason).
                        // We currently have no chance to get an added hyphen for hyphenation
                        // at start of line, as we handle only hyphenation with LTR text.
                        // It feels we have to do it even for the first line with text-indent,
                        // as some page might have multiple consecutive single lines that can
                        // benefit from hanging so the margin looks clean too.
                        int lsb = font->getLeftSideBearing(fmt->m_text[wstart]);
                        int left_overflow = lsb < 0 ? -lsb : 0;
                        if ( fit_glyphs ) {
                            // We don't want any part of the glyph to overflow in the left margin.
                            // We correct only overflows - keeping underflows (so, not having
                            // the glyph blackbox really fit the edge) respects the natural
                            // alignment.
                            // We also prevent hanging punctuation as it de facto overflows.
                            // (We used to correct it only for italic fonts, where "J" or "f"
                            // can have have huge negative overflow for their part below baseline
                            // and so leak on the left. On the left, we were also correcting
                            // underflows, so fitting italic glyphs to the left edge - but we
                            // don't anymore as it doesn't really feel needed.)
                            frmline->x += left_overflow; // so that the glyph's overflow is at original frmline->x
                            // printf("%c lsb=%d\n", fmt->m_text[wstart], font->getLeftSideBearing(fmt->m_text[wstart]));
                        }
                        else {
                            // We prevent hanging punctuation on the common opening quotation marks
                            // or dashes that we flagged with LCHAR_LOCKED_SPACING (most of these
                            // are characters that can hang) - and on fully-pre lines and when
                            // the font is monospace.
                            // Note that some CJK fonts might have full-width glyphs for some of our
                            // common hanging chars, but not for others, and this might look bad with
                            // them, and different whether it is used as the main font or as a fallback.
                            // (Noto Sans CJK SC has full-width glyphs for single or double quotation
                            // marks (‘ ’ “ ”), but not for all our other hanging chars.)
                            // Reducing CJK half-blank full-width glyphs's width should be handled
                            // more generically elsewhere.
                            // We try to avoid hanging these with some heuristic below.
                            bool allow_hanging = fmt->m_hanging_punctuation &&
                                                 !preFormattedOnly &&
                                                 !is_vertical_mode &&
                                                 !(fmt->m_flags[wstart] & LCHAR_LOCKED_SPACING) &&
                                                 font->getFontFamily() != css_ff_monospace;
                            int shift_x = 0;
                            if ( allow_hanging ) {
                                bool check_font;
                                int percent = srcline->lang_cfg->getHangingPercent(false, fmt->m_para_dir_is_rtl, check_font, fmt->m_text, wstart, end-wstart-1);
                                if ( percent && check_font && left_overflow > 0 ) {
                                    // Some fonts might already have enough negative
                                    // left side bearing for some chars, that would
                                    // make them naturally hang on the left.
                                    percent = 0;
                                }
                                if ( percent ) {
                                    int first_char_width = fmt->m_advance[wstart] - (wstart>0 ? fmt->m_advance[wstart-1] : 0);
                                    shift_x = first_char_width * percent / 100;
                                    if ( shift_x == 0 ) // Force at least 1px if division rounded it to 0
                                        shift_x = 1;
                                    // Cancel it if this char looks like it might be full-width
                                    // (0.9 * font size, in case HarfBuzz has reduced the advance)
                                    // and it has a lot of positive left side bearing (left half
                                    // of the glyph blank) - see above.
                                    if ( first_char_width > 0.9 * font->getSize() && lsb > 0.4 * first_char_width ) {
                                        shift_x = 0;
                                    }
                                }
                            }
                            if ( shift_x - lsb > usable_left_overflow ) {
                                shift_x = usable_left_overflow + lsb;
                            }
                            frmline->x -= shift_x;
                        }
                    }

                    // Word x position on line: for now, we just stack words after each other.
                    // They will be adjusted if needed in alignLine()
                    word->x = frmline->width;

                    // Set and adjust word natural width (and min_width which might be used in alignLine())
                    word->width = fmt->m_advance[i>0 ? i-1 : 0] - (wstart>0 ? fmt->m_advance[wstart-1] : 0);
                    word->min_width = word->width;
                    TR("addLine - word(%d, %d) x=%d (%d..%d)[%d] |%s|", wstart, i, frmline->width, wstart>0 ? fmt->m_advance[wstart-1] : 0, fmt->m_advance[i-1], word->width, LCSTR(lString32(fmt->m_text+wstart, i-wstart)));
                    // TCY (tate-chu-yoko): in vertical mode, each TCY span occupies exactly 1em
                    if ( (srcline->flags & LTEXT_IS_TCY)
                         && (css_wm_is_vertical(fmt->m_writing_mode)) ) {
                        int em = font->getSize();
                        word->width = em;
                        word->min_width = em;
                        word->flags |= LTEXT_WORD_IS_TCY;
                    }
                    if ( fmt->m_flags[wstart] & LCHAR_IS_CLUSTER_TAIL ) {
                        // The start of this word is part of a ligature that started
                        // in a previous word: some hyphenation wrap happened on
                        // this ligature, which will not be rendered as such.
                        // We are the second part of the hyphenated word, and our first
                        // char(s) have a width of 0 (for being part of the ligature):
                        // we need to re-measure this half of the original word.
                        int new_width;
                        if ( fmt->measureWord(word, new_width) ) {
                            word->width = new_width;
                            word->min_width = word->width;
                        }
                    }
                    if ( fmt->m_flags[i-1] & LCHAR_ALLOW_HYPH_WRAP_AFTER ) {
                        if ( fmt->m_flags[i] & LCHAR_IS_CLUSTER_TAIL ) {
                            // The end of this word is part of a ligature that, because
                            // of hyphenation, has been splitted onto next word.
                            // We are the first part of the hyphenated word, and
                            // our last char(s) have been assigned the width of the
                            // ligature glyph, which will not be rendered as such:
                            // we need to re-measure this half of the original word.
                            int new_width;
                            if ( fmt->measureWord(word, new_width) ) {
                                word->width = new_width;
                            }
                        }
                        word->width += font->getHyphenWidth();
                        word->min_width = word->width;
                        word->flags |= LTEXT_WORD_CAN_HYPH_BREAK_LINE_AFTER;
                    }

                    bool preformatted = srcline->flags & LTEXT_FLAG_PREFORMATTED;
                    if ( fmt->m_flags[i-1] & LCHAR_IS_SPACE ) {
                        // Current word ends with a space.
                        // Each word ending with a space (except in some conditions) can
                        // have its width reduced by a fraction of this space width or
                        // increased if needed (for text justification), so actually
                        // making that space larger or smaller.
                        // Note: checking if the first word of first line is one of the
                        // common opening quotation marks or dashes is done in fmt->measureText(),
                        // to have it work also with BiDi/RTL text (checking that here
                        // would be too late, as reordering has been done).
                        if ( !(fmt->m_flags[i-1] & LCHAR_LOCKED_SPACING) ) {
                            word->flags |= LTEXT_WORD_CAN_ADD_SPACE_AFTER;
                            int dw = getMaxCondensedSpaceTruncation(fmt,i-1);
                            if (dw>0) {
                                word->min_width = word->width - dw;
                            }
                        }
                        if ( lastWord && !preformatted ) {
                            // If last word of line, remove any trailing space
                            // from word's width (but not with preformatted, in
                            // case of text-align:right where we don't want to
                            // lose any trailing space)
                            word->width = fmt->m_advance[i>1 ? i-2 : 0] - (wstart>0 ? fmt->m_advance[wstart-1] : 0);
                            word->min_width = word->width;
                        }
                    }
                    else if ( !firstWord && fmt->m_flags[wstart] & LCHAR_IS_SPACE ) {
                        // Current word starts with a space (looks like this should not happen):
                        // we can increase the space between previous word and this one if needed
                        //if ( word->t.len<2 || fmt->m_text[i-1]!=UNICODE_NO_BREAK_SPACE || fmt->m_text[i-2]!=UNICODE_NO_BREAK_SPACE)
                        //if ( fmt->m_text[wstart]==UNICODE_NO_BREAK_SPACE && fmt->m_text[wstart+1]==UNICODE_NO_BREAK_SPACE)
                        //    CRLog::trace("Double nbsp text[-1]=%04x", fmt->m_text[wstart-1]);
                        //else
                        frmline->words[frmline->word_count-2].flags |= LTEXT_WORD_CAN_ADD_SPACE_AFTER;
                    }
                    else if ( word->flags & LTEXT_WORD_IS_CJK ) {
                        // We usually can add space before and after CJK chars if needed for text justification,
                        // and we may reduce the widths of some CJK chars (those flagged as "flexible").
                        // See comments at top of fmt->getFlexibleCJKWidthAdjustment() for more info.
                        // These are the defaults for non-flexible CJK chars:
                        int wa8 = 8; // Stay fullwidth (8 x 1/8em)
                        bool can_add_space_before = true;
                        bool can_add_space_after = true;
                        // But if flexible, these depend on the context
                        if ( (fmt->m_flags[wstart] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) ) {
                            // We provide this line start and end as the start and end
                            wa8 = fmt->getFlexibleCJKWidthAdjustment(wstart, start, end, can_add_space_before, can_add_space_after);
                        }
                        // Apply them
                        if ( can_add_space_after ) {
                            word->flags |= LTEXT_WORD_CAN_ADD_SPACE_AFTER;
                        }
                        if ( !firstWord ) {
                            if ( can_add_space_before ) {
                                // If previous word was a CJK, it got correctly LTEXT_WORD_CAN_ADD_SPACE_AFTER
                                // or not. If not set, it got explicitely can_add_space_after=false, and we don't
                                // want to change it. So don't do anything if is is CJK.
                                if ( !(frmline->words[frmline->word_count-2].flags & LTEXT_WORD_IS_CJK) ) {
                                    // Previous word may be digits or latin text that did not get _CAN_ADD_SPACE_AFTER
                                    // if these was no space - but a followup CJK should allow it.
                                    // But this previous word may also be a non-CJK opening punctuation (ie. U+201C
                                    // with Japanese non-made fullwidth and so not considered CJK) that we don't want
                                    // to spread from its following CJK.
                                    // It feels we can trust ALLOW_WRAP_AFTER being not set to assume it is
                                    // an opening punctuation or similar and that no space should be added.
                                    if ( fmt->m_flags[wstart-1] & LCHAR_ALLOW_WRAP_AFTER ) {
                                        frmline->words[frmline->word_count-2].flags |= LTEXT_WORD_CAN_ADD_SPACE_AFTER;
                                    }
                                }
                            }
                            else { // cancel any previously set
                                frmline->words[frmline->word_count-2].flags &= ~LTEXT_WORD_CAN_ADD_SPACE_AFTER;
                            }
                        }
                        if ( wa8 != 8 ) {
                            // We floor the adjusted width, as we ceil'ed the width we can steal from it (so that if
                            // the width is an odd number of pixels, we can fit 2 halfwidth'ed chars instead of none).
                            if ( wa8 > 0 ) { // should be forced to be the adjusted width
                                word->min_width = word->width * wa8 / 8;
                                word->width = word->min_width;
                                word->flags |= LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK;
                            }
                            else if ( wa8 < 0 ) { // can be reduced down to this adjusted width, only if needed
                                wa8 = -wa8;
                                word->min_width = word->width * wa8 / 8;
                                word->flags |= LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK;
                            }
                            // Note: the clreq/jlreq specs mention that punctuation can or should be made halfwidth,
                            // or stay fullwidth, and no in-between. So, no reason to use min_space_condensing_percent
                            // to allow tuning by how much CJK punctuation can be reduced and limit it to 75% or 90%.
                        }
                    }

                    if ( fmt->m_has_cjk && !firstWord && fmt->m_kerning_mode != KERNING_MODE_DISABLED ) {
                        // At the boundary between a CJK segment and a segment of non-CJK chars, we want to
                        // add a bit of spacing, 1/4em as advised by clreq and jlreq.
                        // We explicitely don't do this if any boundary is some punctuation (CJK or not), as
                        // a CJK punctuation might bring itself some spacing, and a non-CJK punctuation can
                        // itself serves as spacing.
                        // Note: in jlreq, the priority for decreasing even more this 1/4em for line adjustment
                        // comes very late, so we don't really need to make it adjustable, and we can just
                        // ensure this space by shifting word->x (otherwise, we would need instead to add
                        // a dummy word flagged as WORD_IS_PAD with a width and a min_width=0).
                        // As done in processParagraph() (see there for details), but now in visual order.
                        if ( (fmt->m_flags[wstart] & LCHAR_IS_CJK) && !(fmt->m_flags[wstart] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                     !(fmt->m_flags[wstart-1] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                            // Current char is a non-flexible CJK (so, not a CJK punctuation).
                            // Previous char is not a CJK, object nor space.
                            lUInt16 props = lGetCharProps(fmt->m_text[wstart-1]);
                            if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                                // Previous char is not a punctuation and not some other kind of space.
                                // Add 1/4 of the CJK char's font size
                                int spacing = font->getSize() / 4;
                                word->x += spacing;
                                frmline->width += spacing;
                            }

                        }
                        else if ( (fmt->m_flags[wstart-1] & LCHAR_IS_CJK) && !(fmt->m_flags[wstart-1] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                     !(fmt->m_flags[wstart] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                            // Previous char is a non-flexible CJK (so, not a CJK punctuation).
                            // Current char is not a CJK, object nor space.
                            lUInt16 props = lGetCharProps(fmt->m_text[wstart]);
                            if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                                // Current char is not a punctuation and not some other kind of space.
                                // Add 1/4 of the CJK char's font size
                                int spacing = ((LVFont *)fmt->m_srcs[wstart-1]->t.font)->getSize() / 4;
                                word->x += spacing;
                                frmline->width += spacing;
                            }
                        }
                    }

                    if ( lastWord && (align == LTEXT_ALIGN_RIGHT || align == LTEXT_ALIGN_WIDTH) ) {
                        // Adjust line end if needed.
                        // If we need to adjust last word's last char, we need to put the delta
                        // in this word->width, which will make it into frmline->width.
                        // By reducing the last word width and so frmline->width, we'll have
                        // its drawing (with its real width) overflow the line width. We'll
                        // store this overflow in frmline->width_overflow so we can include
                        // it in text highlighting.

                        // Find the real last drawn glyph
                        int lastnonspace = i-1;
                        for ( int k=i-1; k>=wstart; k-- ) {
                            if ( !(fmt->m_flags[k] & LCHAR_IS_SPACE) ) {
                                lastnonspace = k;
                                break;
                            }
                        }
                        bool ends_with_hyphen = fmt->m_flags[lastnonspace] & LCHAR_ALLOW_HYPH_WRAP_AFTER;
                        int rsb = 0; // don't bother with hyphen rsb, which can't overflow
                        int right_overflow = 0;
                        if ( !ends_with_hyphen ) {
                            rsb = font->getRightSideBearing(fmt->m_text[lastnonspace]);
                            if ( rsb < 0 )
                                right_overflow = -rsb;
                        }
                        if ( fit_glyphs ) {
                            // We don't want any part of the glyph to overflow in the right margin.
                            // (We used to correct it only for italic fonts, where "J" or "f"
                            // can have have huge negative overflow for their part above baseline
                            // and so leak on the right. We were previously also correcting only
                            // overflows and not underflows.)
                            word->width += right_overflow;
                        }
                        else {
                            // We prevent hanging punctuation in a few cases (see above)
                            bool allow_hanging = fmt->m_hanging_punctuation &&
                                                 !preFormattedOnly &&
                                                 !is_vertical_mode &&
                                                 font->getFontFamily() != css_ff_monospace;
                            int shift_w = 0;
                            if ( allow_hanging ) {
                                if ( ends_with_hyphen ) {
                                    int percent = srcline->lang_cfg->getHyphenHangingPercent();
                                    if ( percent ) {
                                        shift_w = font->getHyphenWidth() * percent / 100;
                                        if ( shift_w == 0 ) // Force at least 1px if division rounded it to 0
                                            shift_w = 1;
                                    }
                                    // Note: some part of text in bold or in a bigger font size inside
                                    // a paragraph may stand out more than the regular text, and this
                                    // is quite noticable with the hyphen.
                                    // We might want to limit or force hyphen hanging to what it should
                                    // be with the main paragraph font, but that might not work well in
                                    // some situations.
                                    // See https://github.com/koreader/crengine/pull/355#issuecomment-656760791
                                }
                                else {
                                    bool check_font;
                                    int percent = srcline->lang_cfg->getHangingPercent(true, fmt->m_para_dir_is_rtl, check_font, fmt->m_text, lastnonspace, end-lastnonspace-1);
                                    if ( percent && check_font && right_overflow > 0 ) {
                                        // Some fonts might already have enough negative
                                        // right side bearing for some chars, that would
                                        // make them naturally hang on the right.
                                        percent = 0;
                                    }
                                    if ( percent ) {
                                        int last_char_width = fmt->m_advance[lastnonspace] - (lastnonspace>0 ? fmt->m_advance[lastnonspace-1] : 0);
                                        shift_w = last_char_width * percent / 100;
                                        if ( shift_w == 0 ) // Force at least 1px if division rounded it to 0
                                            shift_w = 1;
                                        // Cancel it if this char looks like it might be full-width
                                        // (0.9 * font size, in case HarfBuzz has reduced the advance)
                                        // and it has a lot of positive right side bearing (right half
                                        // of the glyph blank) - see comment above in 'firstWord' handling.
                                        if ( last_char_width > 0.9 * font->getSize() && rsb > 0.4 * last_char_width ) {
                                            shift_w = 0;
                                        }
                                    }
                                }
                            }
                            if ( shift_w - rsb > usable_right_overflow ) {
                                shift_w = usable_right_overflow + rsb;
                            }
                            word->width -= shift_w;
                            // This last word will overflow over frmline->width: remember it,
                            // so we can include it in the drawing of native text selection.
                            frmline->width_overflow = shift_w;
                        }
                    }

                    // This is set or used only when LTEXT_FIT_GLYPHS
                    prev_word_is_object = false;
                    prev_word_overflow = 0;
                    if ( fit_glyphs && !lastWord ) {
                        int rsb = font->getRightSideBearing(fmt->m_text[i-1]);
                        if ( rsb < 0 ) {
                            // This may be added to shit word width if next
                            // word is an image or an inline box
                            prev_word_overflow = -rsb;
                        }
                    }

                    /* Hanging punctuation (with CJK specifics) old code:
                     *
                    bool visualAlignmentEnabled = fmt->m_hanging_punctuation && (align != LTEXT_ALIGN_CENTER);
                    if ( visualAlignmentEnabled && lastWord ) { // if floating punctuation enabled
                        int endp = i-1;
                        int lastc = fmt->m_text[endp];
                        int wAlign = font->getVisualAligmentWidth();
                        word->width += wAlign/2;
                        while ( (fmt->m_flags[endp] & LCHAR_IS_SPACE) && endp>0 ) { // || lastc=='\r' || lastc=='\n'
                            word->width -= fmt->m_advance[endp] - fmt->m_advance[endp-1];
                            endp--;
                            lastc = fmt->m_text[endp];
                        }
                        // We reduce the word width from the hanging char width, so it's naturally pushed
                        // outside in the margin by the alignLine code
                        if ( word->flags & LTEXT_WORD_CAN_HYPH_BREAK_LINE_AFTER ) {
                            word->width -= font->getHyphenWidth(); // TODO: strange fix - need some other solution
                        }
                        else if ( lastc=='.' || lastc==',' || lastc=='!' || lastc==':' || lastc==';' || lastc=='?') {
                            FONT_GUARD
                            int w = font->getCharWidth(lastc);
                            TR("floating: %c w=%d", lastc, w);
                            if (frmline->width + w + wAlign + x >= maxWidth)
                                word->width -= w; //fix russian "?" at line end
                        }
                        else if ( lastc==0x2019 || lastc==0x201d ||   // ’ ” right quotation marks
                                  lastc==0x3001 || lastc==0x3002 ||   // 、 。 ideographic comma and full stop
                                  lastc==0x300d || lastc==0x300f ||   // 」 』 ideographic right bracket
                                  lastc==0xff01 || lastc==0xff0c ||   // ！ ， fullwidth ! and ,
                                  lastc==0xff1a || lastc==0xff1b ) {  // ： ； fullwidth : and ;
                            FONT_GUARD
                            int w = font->getCharWidth(lastc);
                            if (frmline->width + w + wAlign + x >= maxWidth)
                                word->width -= w;
                            else if (w!=0) {
                                // (This looks like some awkward way of detecting if the line
                                // is made out of solely same-fixed-width CJK ideographs,
                                // which will fail if there's enough variable-width western
                                // chars to fail the rounded division vs nb of char comparison.)
                                if (end - start == int((maxWidth - wAlign) / w))
                                    word->width -= w; // Chinese floating punctuation
                                else if (x/w >= 1 && (end-start==int(maxWidth-wAlign-x)/w)-1)
                                    word->width -= w; // first line with text-indent
                            }
                        }
                        if (frmline->width!=0 && last && align!=LTEXT_ALIGN_CENTER) {
                            // (Chinese) add spaces between words in last line or single line
                            // (so they get visually aligned on a grid with the char on the
                            // previous justified lines)
                            FONT_GUARD
                            int properwordcount = maxWidth/font->getSize() - 2;
                            int extraSpace = maxWidth - properwordcount*font->getSize() - wAlign;
                            int exccess = (frmline->width + x + word->width + extraSpace) - maxWidth;
                            if ( exccess>0 && exccess<maxWidth ) { // prevent the line exceeds screen boundary
                                extraSpace -= exccess;
                            }
                            if ( extraSpace>0 ) {
                                int addSpacePoints = 0;
                                int a;
                                int points=0;
                                for ( a=0; a<(int)frmline->word_count-1; a++ ) {
                                    if ( frmline->words[a].flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER )
                                        points++;
                                }
                                addSpacePoints = properwordcount - (frmline->word_count - 1 - points);
                                if (addSpacePoints > 0) {
                                    int addSpaceDiv = extraSpace / addSpacePoints;
                                    int addSpaceMod = extraSpace % addSpacePoints;
                                    int delta = 0;
                                    for (a = 0; a < (int) frmline->word_count; a++) {
                                        frmline->words[a].x +=  delta;
                                        {
                                            delta += addSpaceDiv;
                                            if (addSpaceMod > 0) {
                                                addSpaceMod--;
                                                delta++;
                                            }
                                        }
                                    }
                                }
                            }
                            word->width+=extraSpace;
                        }
                        if ( first && font->getSize()!=0 && (maxWidth/font->getSize()-2)!=0 ) {
                            // proportionally enlarge text-indent when visualAlignment or
                            // floating punctuation is enabled
                            FONT_GUARD
                            int cnt = ((x-wAlign/2)%font->getSize()==0) ? (x-wAlign/2)/font->getSize() : 0;
                                // ugly way to caculate text-indent value, I can not get text-indent from here
                            int p = cnt*(cnt+1)/2;
                            int asd = (2*font->getSize()-font->getCharWidth(lastc)) / (maxWidth/font->getSize()-2);
                            int width = p*asd + cnt; //same math as delta above
                            if (width>0)
                                frmline->x+=width;
                        }
                        word->min_width = word->width;
                    } // done if floating punctuation enabled
                    * End of old code for handling hanging punctuation
                    */

                    // printf("addLine - word(%d, %d) x=%d (%d..%d)[%d>%d %x] |%s|\n", wstart, i,
                    //      frmline->width, wstart>0 ? fmt->m_advance[wstart-1] : 0, fmt->m_advance[i-1], word->width,
                    //      word->min_width, word->flags, LCSTR(lString32(fmt->m_text+wstart, i-wstart)));
                }

                // Word added: adjust frmline height and baseline to account for this word
                if ( adjust_line_box ) {
                    // Adjust full line box height and baseline if needed:
                    // frmline->height is the current line height
                    // frmline->baseline is the distance from line top to the main baseline of the line
                    // top_to_baseline (normally positive number) is the distance from this word top to its own baseline.
                    // baseline_to_bottom (normally positive number) is the descender below baseline for this word
                    // word->y is the distance from this word baseline to the line main baseline
                    //   it is positive when word is subscript, negative when word is superscript
                    //
                    // negative word->y means it's superscript, so the line's baseline might need to go
                    // down (increase) to make room for the superscript
                    int needed_baseline = top_to_baseline - word->y;
                    if ( needed_baseline > frmline->baseline ) {
                        // shift the line baseline and height by the amount needed at top
                        int shift_down = needed_baseline - frmline->baseline;
                        // if (frmline->baseline) printf("pushed down +%d\n", shift_down);
                        // if (frmline->baseline && srcline->object)
                        //     printf("%s\n", UnicodeToLocal(ldomXPointer((ldomNode*)srcline->object, 0).toString()).c_str());
                        if ( !strut_confined ) {
                            // move line away from the strut baseline
                            frmline->baseline += shift_down;
                            frmline->height += shift_down;
                        }
                        else { // except if "-cr-hint: strut-confined":
                            // Keep the strut, move the word down
                            word->y += shift_down;
                        }
                    }
                    // positive word->y means it's subscript, so the line's baseline does not need to be
                    // changed, but more room below might be needed to display the subscript: increase
                    // line height so next line is pushed down and dont overwrite the subscript
                    int needed_height = frmline->baseline + baseline_to_bottom + word->y;
                    if ( needed_height > frmline->height ) {
                        // printf("extended down +%d\n", needed_height-frmline->height);
                        if ( !strut_confined ) {
                            frmline->height = needed_height;
                        }
                        else { // except if "-cr-hint: strut-confined":
                            // We'd rather move the word up, but it shouldn't go
                            // above the top of the line, so it's not drawn over
                            // previous line text. If it's taller than line height,
                            // it's ok to have it overflow bottom: some part of
                            // it might be overwritten by next line, which we'd
                            // rather have fully readable.
                            word->y -= needed_height - frmline->height;
                            int top_dy = top_to_baseline - word->y - frmline->baseline;
                            if ( top_dy > 0 )
                                word->y += top_dy;
                        }
                    }
                }

                frmline->width += word->width;
                firstWord = false;

                lastSrc = newSrc;
                wstart = i;
            }
            lastIsSpace = isSpace;
            lastIsRTL = isRTL;
        }
        // All words added

        if ( delayed_valign_computation ) {
            // Delayed computation and line box adjustment when we have some words
            // (or images, or inline-boxes) with vertical-align: top or bottom.
            // First, see if we need to adjust frmline->baseline and frmline->height,
            // similarly as done above if adjust_line_box:
            for ( int i=0; i<frmline->word_count; i++ ) {
                if ( frmline->words[i].flags & (LTEXT_WORD_VALIGN_TOP|LTEXT_WORD_VALIGN_BOTTOM) ) {
                    formatted_word_t * word = &frmline->words[i];
                    if ( word->flags & LTEXT_WORD_STRUT_CONFINED )
                        continue; // don't have such words affect current line height & baseline
                    // Update incomplete word->y with current frmline baseline & height,
                    // just as it would have been done if not delayed
                    int cur_word_y;
                    if ( word->flags & LTEXT_WORD_VALIGN_TOP )
                        cur_word_y = word->y - frmline->baseline;
                    else if ( word->flags & LTEXT_WORD_VALIGN_BOTTOM )
                        cur_word_y = word->y + frmline->height - frmline->baseline;
                    else // should not happen
                        cur_word_y = word->y;
                    int needed_baseline = word->_top_to_baseline - cur_word_y;
                    if ( needed_baseline > frmline->baseline ) {
                        // shift the line baseline and height by the amount needed at top
                        int shift_down = needed_baseline - frmline->baseline;
                        frmline->baseline += shift_down;
                        frmline->height += shift_down;
                    }
                    int needed_height = frmline->baseline + word->_baseline_to_bottom + cur_word_y;
                    if ( needed_height > frmline->height ) {
                        frmline->height = needed_height;
                    }
                }
            }
            // Then, get the final word->y (baseline) that aligns the word to top or bottom of frmline
            for ( int i=0; i<frmline->word_count; i++ ) {
                if ( frmline->words[i].flags & (LTEXT_WORD_VALIGN_TOP|LTEXT_WORD_VALIGN_BOTTOM) ) {
                    formatted_word_t * word = &frmline->words[i];
                    if ( word->flags & LTEXT_WORD_VALIGN_TOP ) {
                        word->y = word->y - frmline->baseline;
                    }
                    else if ( word->flags & LTEXT_WORD_VALIGN_BOTTOM ) {
                        word->y = word->y + frmline->height - frmline->baseline;
                    }
                    if ( word->flags & LTEXT_WORD_STRUT_CONFINED ) {
                        // If this word is taller than final line height,
                        // we'd rather have it overflows bottom.
                        int top_dy = word->_top_to_baseline - word->y - frmline->baseline;
                        if ( top_dy > 0 )
                            word->y += top_dy; // move it down
                    }
                }
            }
        }

        if ( !light_formatting ) {
            // Fix up words position and width to ensure requested alignment and indent
            alignLineHorizontal( fmt, frmline, align, rightIndent, hasInlineBoxes );
        }

        // Vertical text: fix up line dimensions
        // The formatter computes width/height as if horizontal. For vertical text:
        // - frmline->height = column WIDTH on screen (cross-column extent)
        // - frmline->width  = column WIDTH on screen (used by visibility checks)
        // Symmetry with horizontal mode: in horizontal mode, lines grow taller to
        // accommodate inline boxes that exceed strut_height (e.g. ruby with annotation
        // makes the line 36px tall instead of 30px).  Per JLReq, the same should apply
        // in vertical mode: columns containing ruby grow wider so the base character
        // stays aligned with body-text characters and the annotation fits inside the
        // column rather than overflowing into adjacent columns.
        //
        // We can't use frmline->height directly because it may have been grown by
        // baseline-adjustment logic (lines 1996-2010) to a value LARGER than the
        // tallest inline box (e.g. baseline-shift for box.baseline != strut.baseline
        // gives frmline->height = strut + shift + box descender, which can exceed h_box).
        // Using such an inflated value would make col_width larger than h_box and the
        // ruby base character (positioned at the box's left edge) would appear shifted
        // right relative to plain characters (which sit at column_left).  Iterate
        // through the words to find the actual max inline-box height instead.
        if ( css_wm_is_vertical(fmt->m_writing_mode) ) {
            // Column width is always strut_height regardless of inline box (ruby) height.
            // Per JLReq, ruby annotations overhang into the inter-column gap rather than
            // inflating the column.  The inter-column gap (strut - em) is wide enough to
            // accommodate a half-em annotation without overlapping adjacent column text.
            int col_width = fmt->m_pbuffer->strut_height;
            frmline->height = col_width;
            frmline->width = col_width;
        }

        // Get ready for next line
        fmt->m_line_advance += frmline->height;
        fmt->m_pbuffer->height = fmt->m_line_advance;
        fmt->checkOngoingFloat();
        fmt->positionDelayedFloats();
        #if (USE_FRIBIDI==1)
        if ( restore_last_width ) // bidi: restore last width to not mess with next line
            fmt->m_advance[end-1] = last_width_to_restore;
        #endif
    }

int getMaxCondensedSpaceTruncation(LVFormatter* fmt, int pos) {
        if (pos<0 || pos>=fmt->m_length || !(fmt->m_flags[pos] & LCHAR_IS_SPACE))
            return 0;
        if (fmt->m_pbuffer->min_space_condensing_percent==100)
            return 0;
        int w = (fmt->m_advance[pos] - (pos > 0 ? fmt->m_advance[pos-1] : 0));
        int dw = w * (100 - fmt->m_pbuffer->min_space_condensing_percent) / 100;
        if ( dw>0 ) {
            // typographic rule: don't use spaces narrower than 1/4 of font size
            /* 20191126: disabled, to allow experimenting with lower %
            LVFont * fnt = (LVFont *)fmt->m_srcs[pos]->t.font;
            int fntBasedSpaceWidthDiv2 = fnt->getSize() * 3 / 4;
            if ( dw>fntBasedSpaceWidthDiv2 )
                dw = fntBasedSpaceWidthDiv2;
            */
            return dw;
        }
        return 0;
    }

    #if (USE_LIBUNIBREAK!=1)
    bool isCJKPunctuation(lChar32 c) {
        return ( c >= 0x3000 && c <= 0x303F ) || // CJK Symbols and Punctuation
               ( c >= 0x2000 && c <= 0x206F &&   // General Punctuation, except these:
                    c!=0x2018 && c!=0x201a && c!=0x201b &&    // ‘ ‚ ‛  left quotation marks
                    c!=0x201c && c!=0x201e && c!=0x201f &&    // “ „ ‟  left double quotation marks
                    c!=0x2035 && c!=0x2036 && c!=0x2037 &&    // ‵ ‶ ‷ reversed single/double/triple primes
                    c!=0x2039 && c!=0x2045 && c!=0x204c  ) || // ‹ ⁅ ⁌ left angle quot mark, bracket, bullet
               ( c >= 0xFF01 && c <= 0xFFEE ) || // Halfwidth and Fullwidth Forms (obviously wrong)
               ( c == 0x00b7 ); // · middle dot
    }

    bool isCJKLeftPunctuation(lChar32 c) {
        return c==0x2018 || c==0x201c || // ‘ “ left single and double quotation marks
               c==0x3008 || c==0x300a || c==0x300c || c==0x300e || c==0x3010 || // 〈 《 「 『 【 CJK left brackets
               c==0xff08; // （ fullwidth left parenthesis
    }
    #endif

    bool isLeftPunctuation(lChar32 c) {
        // Opening quotation marks and dashes that we don't want a followup space to
        // have its width changed
        // (We don't use CH_PROP_PUNCT_OPEN as we consider a few more non-punctuation chars.)
        return ( c >= 0x2010 && c <= 0x2027 ) || // Hyphens, dashes, quotation marks, bullets...
               ( c >= 0x2032 && c <= 0x205E ) || // Primes, bullets...
               ( c >= 0x002A && c <= 0x002F ) || // Ascii * + , - . /
                 c == 0x00AB || c == 0x00BB   || // Quotation marks (including right pointing, for german text)
                 c == 0x0022 || c == 0x0027 || c == 0x0023; // Ascii " ' #

    }

// Shared hyphenation-break search used by both processParagraphHorizontal and
// processParagraphVertical.  lineStart is the line's leading offset: x (accumulated
// left indent) for horizontal, y (column top indent) for vertical.  maxExtent is
// maxWidth or maxHeight.  wordpos is updated in-place (decremented during search).
static void tryHyphenBreak(
    LVFormatter* fmt, int pos, int& wordpos,
    int lastNormalWrap, int lastMandatoryWrap,
    int lineStart, int w0,
    int maxExtent, int spaceReduceWidth,
    int unusedPercent, int& lastHyphWrap)
{
    if ( lastMandatoryWrap >= 0 || lastNormalWrap >= fmt->m_length-1
            || unusedPercent <= fmt->m_pbuffer->unused_space_threshold_percent )
        return;
    // #define DEBUG_HYPH_EXTRA_LOOPS // Uncomment for debugging loops
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
        if ( !(fmt->m_srcs[wordpos]->flags & LTEXT_HYPHENATE) || (fmt->m_srcs[wordpos]->flags & LTEXT_FLAG_NOWRAP) ) {
            wordpos = wordpos - MIN_WORD_LEN_TO_HYPHENATE;
            continue;
        }
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
        if ( wstart >= wordpos ) {
            wordpos = wordpos - MIN_WORD_LEN_TO_HYPHENATE;
            continue;
        }
        if ( len > MAX_WORD_SIZE )
            len = MAX_WORD_SIZE;
        lUInt8 * flags = (lUInt8*) (fmt->m_flags + wstart);
        static lUInt16 widths[MAX_WORD_SIZE];
        int wordStart_w = wstart > 0 ? fmt->m_advance[wstart-1] : 0;
        for ( int i = 0; i < len; i++ )
            widths[i] = fmt->m_advance[wstart+i] - wordStart_w;
        int max_extent = maxExtent + spaceReduceWidth - (lineStart + (wordStart_w - w0));
        int _hyphen_width = 0;
        for ( int i = wstart; i < wend; i++ ) {
            if ( !(fmt->m_srcs[i]->flags & LTEXT_SRC_IS_OBJECT) ) {
                _hyphen_width = ((LVFont*)fmt->m_srcs[i]->t.font)->getHyphenWidth();
                break;
            }
        }
        if ( fmt->m_srcs[wordpos]->lang_cfg->getHyphMethod()->hyphenate(
                fmt->m_text+wstart, len, widths, flags, _hyphen_width, max_extent, 2) ) {
            for ( int i = 0; i < len; i++ ) {
                if ( fmt->m_flags[wstart+i] & LCHAR_ALLOW_HYPH_WRAP_AFTER ) {
                    if ( widths[i] + _hyphen_width > max_extent ) {
                        TR("hyphen found, but max_extent reached at char %d", i);
                        fmt->m_flags[wstart+i] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                    }
                    else if ( wstart + i > pos+1 ) {
                        if ( lastHyphWrap >= 0 )
                            fmt->m_flags[lastHyphWrap] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                        lastHyphWrap = wstart + i;
                    }
                    else if ( wstart + i >= pos ) {
                        fmt->m_flags[wstart+i] &= ~LCHAR_ALLOW_HYPH_WRAP_AFTER;
                    }
                }
            }
            if ( lastHyphWrap >= 0 )
                break;
        }
        TR("no hyphen found - max_extent=%d", max_extent);
        wordpos = wstart - 1;
    }
}

    /// Split paragraph into lines
void processParagraphHorizontal( LVFormatter* fmt, int start, int end, bool isLastPara )
    {
        TR("processParagraph(%d, %d)", start, end);

        // ensure buffer size is ok for paragraph
        fmt->allocate( start, end );
        // copy paragraph text to buffer
        fmt->copyText( start, end );
        // measure paragraph text
        fmt->measureText();

        // We keep as 'para' the first source text, as it carries
        // the text alignment to use with all added lines.
        src_text_fragment_t * para = &fmt->m_pbuffer->srctext[start];

        // detect case with inline preformatted text inside block with line feeds -- override align=left for this case
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

        // Not per-specs, but when floats reduce the available width, skip y until
        // we have the width to draw at least a few chars on a line.
        // We use N x strut_height because it's one easily acccessible font metric here.
        int minWidth = 3 * fmt->m_pbuffer->strut_height;

        // split paragraph into lines, export lines
        int pos = 0;

        bool is_css_first_line = fmt->m_srcs[0] ? (fmt->m_srcs[0]->flags & LTEXT_IS_FIRST_LINE_CLONE) : false;

        #if (USE_LIBUNIBREAK!=1)
        int upSkipPos = -1;
        #endif

        // Note: we no longer adjust here x and width to account for first or
        // last italic glyphs side bearings or hanging punctuation, as here,
        // we're still just walking the text in logical order, which might
        // be re-ordered when BiDi.
        // We'll handle that in AddLine() where we'll make words in visual
        // order; the small shifts we might have on the final width vs the
        // width measured here will hopefully be compensated on the space chars.

        while ( pos<fmt->m_length ) { // each loop makes a line
            // x is this line indent. We use it like a x coordinates below, but
            // we'll use it on the right in addLine() if para is RTL.
            int x;
            if (para->flags & LTEXT_LEGACY_RENDERING) {
                x = para->indent > 0 ? (pos == 0 ? para->indent : 0 ) : (pos==0 ? 0 : -para->indent);
            } else {
                x = fmt->m_indent_current;
                if ( !fmt->m_indent_first_line_done ) {
                    fmt->m_indent_first_line_done = true;
                    fmt->m_indent_current = fmt->m_indent_after_first_line;
                }
            }
            int w0 = pos>0 ? fmt->m_advance[pos-1] : 0; // measured cumulative width at start of this line
            int lastNormalWrap = -1;
            int lastDeprecatedWrap = -1; // Different usage whether USE_LIBUNIBREAK or not (see below)
            int lastHyphWrap = -1;
            int lastMandatoryWrap = -1;
            int spaceReduceWidth = 0; // max total line width which can be reduced by narrowing of spaces
            int cjkReduceWidth = 0; // max total line width which can be reduced by narrowing CJK punctuations
            int firstInlineBoxPos = -1;

            int maxWidth = fmt->getCurrentLineWidth();
            if (maxWidth <= minWidth) {
                // Find y with available minWidth
                int unused_x;
                // We need to provide a height to find some width available over
                // this height, but we don't know yet the height of text (that
                // may have some vertical-align or use a bigger font) or images
                // that will end up on this line (line height is handled later,
                // by AddLine()), we can only ask for the only height we know
                // about: fmt->m_pbuffer->strut_height...
                // todo: find a way to be sure or react to that
                int new_y = fmt->getYWithAvailableWidth(fmt->m_line_advance, minWidth, fmt->m_pbuffer->strut_height, unused_x);
                fmt->fillAndMoveToY( new_y );
                maxWidth = fmt->getCurrentLineWidth();
            }

            if ( fmt->m_flags[pos] & LCHAR_IS_CLUSTER_TAIL && pos > 0 ) {
                // This line starts with a cluster tail, probably because hyphenation was
                // allowed inside this cluster. The first char(s) would get a width of 0,
                // which may allow more text to be brought into this line: later, in AddLine(),
                // we may have to handle the excess of text by reducing all spaces' widths
                // and possibly making them all 0 or negative if needed.
                // So, account for the whole cluster width into this line (by considering it as a
                // negative spaceReduceWidth): it might be too much and we could do with a fraction
                // of it (but which value?), but better too much spacing than not enough.
                int bpos = pos - 1;
                while ( bpos > 0 && fmt->m_flags[bpos] & LCHAR_IS_CLUSTER_TAIL )
                    bpos--;
                int cluster_width = (fmt->m_advance[bpos] - (bpos > 0 ? fmt->m_advance[bpos-1] : 0));
                spaceReduceWidth -= cluster_width;
            }

            // Find candidates where end of line is possible
            bool seen_non_collapsed_space = false;
            bool seen_first_rendered_char = false;
            bool first_line_sequance_end_reached = false; // ::first-line cloned text end reached
            int i;
            for ( i=pos; i<fmt->m_length; i++ ) {
                if ( fmt->m_text[i]=='\n' ) { // might happen in <pre>formatted only (?)
                    lastMandatoryWrap = i;
                    break;
                }
                if ( is_css_first_line && !(fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE) ) {
                    // We reached the non-first-line sequence: the first-line sequence
                    // did fit all on the first line, don't go at appending the same
                    // text from the non-first line sequence
                    first_line_sequance_end_reached = true;
                    // For the followup wrap position resolution, pretend we met a \n
                    // after where first line sequence ended
                    lastMandatoryWrap = i;
                    break;
                }
                lUInt16 flags = fmt->m_flags[i];
                if ( flags & LCHAR_IS_OBJECT ) {
                    if ( fmt->m_charindex[i] == FLOAT_CHAR_INDEX ) { // float
                        src_text_fragment_t * src = fmt->m_srcs[i];
                        // Not sure if we can be called again on the same LVFormatter
                        // object, but the whole code allows for re-formatting and
                        // they should give the same result.
                        // So, use a flag to not re-add already processed floats.
                        if ( !(src->o.objflags & LTEXT_OBJECT_IS_FLOAT_DONE) ) {
                            int currentWidth = x + fmt->m_advance[i]-w0 - spaceReduceWidth;
                            fmt->addFloat( src, currentWidth );
                            src->o.objflags |= LTEXT_OBJECT_IS_FLOAT_DONE;
                            maxWidth = fmt->getCurrentLineWidth();
                        }
                        // We don't set lastNormalWrap when collapsed spaces,
                        // so let's not for floats either.
                        // But we need to when the float is the last source (as
                        // done below, otherwise we would not update wrapPos and
                        // we'd get another ghost line, and this real last line
                        // might be wrongly justified).
                        if ( i==fmt->m_length-1 ) {
                            lastNormalWrap = i;
                        }
                        continue;
                    }
                    if ( fmt->m_charindex[i] == INLINEBOX_CHAR_INDEX && firstInlineBoxPos < 0 ) {
                        firstInlineBoxPos = i;
                    }
                }
                // We would not need to bother with LCHAR_IS_COLLAPSED_SPACE, as they have zero
                // width and so can be grabbed here. They carry LCHAR_ALLOW_WRAP_AFTER just like
                // a space, so they will set lastNormalWrap.
                // But we don't want any collapsed space at start to make a new line if the
                // following text is a long word that doesn't fit in the available width (which
                // can happen in a small table cell). So, ignore them at start of line:
                if (!seen_non_collapsed_space) {
                    if (flags & LCHAR_IS_COLLAPSED_SPACE)
                        continue;
                    seen_non_collapsed_space = true;
                }
                if ( !seen_first_rendered_char ) {
                    seen_first_rendered_char = true;
                    // First real non ignoreable char (collapsed spaces skipped):
                    // it might be a wide image or inlineBox. Check that we have
                    // enough current width to have it on this line, otherwise,
                    // move down until we find a y where it would fit (but only
                    // if we're sure we'll find some)
                    int needed_width = x + fmt->m_advance[i]-w0;
                    if ( needed_width > maxWidth && needed_width <= fmt->m_pbuffer->width ) {
                        // Find y with available needed_width
                        int unused_x;
                        // todo: provide the height of the image or inline-box
                        int new_y = fmt->getYWithAvailableWidth(fmt->m_line_advance, needed_width, fmt->m_pbuffer->strut_height, unused_x);
                        fmt->fillAndMoveToY( new_y );
                        maxWidth = fmt->getCurrentLineWidth();
                    }
                }
                if ( fmt->m_has_cjk && i > pos && fmt->m_kerning_mode != KERNING_MODE_DISABLED ) {
                    // At the boundary between a CJK segment and a segment of non-CJK chars, we want to
                    // add a bit of spacing, 1/4em as advised by clreq and jlreq.
                    // https://www.w3.org/TR/jlreq/#handling_of_western_text_in_japanese_text_using_proportional_western_fonts
                    // If a char on either side is a space or a punctuation (CJK or not), we don't do it (to
                    // avoid excessive spacing when it is already provided by the CJK punctuation, and around
                    // non-CJK punctuation as we're not sure what comes after/before and on which side the
                    // spacing should be added, and it might itself serves as spacing).
                    // We're doing this now in the stream of char in logical order. We'll be doing it again
                    // in addLine() with chars possibly visually re-ordered.
                    if ( (fmt->m_flags[i] & LCHAR_IS_CJK) && !(fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                 !(fmt->m_flags[i-1] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                        // Current char is a non-flexible CJK (so, not a CJK punctuation).
                        // Previous char is not a CJK, object nor space.
                        lUInt16 props = lGetCharProps(fmt->m_text[i-1]);
                        if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                            // Previous char is not a punctuation and not some other kind of space.
                            // Assume we'll have to add 1/4 of the CJK char's font size
                            LVFont * fnt = (LVFont *)fmt->m_srcs[i]->t.font;
                            spaceReduceWidth -= fnt->getSize() / 4;
                        }

                    }
                    else if ( (fmt->m_flags[i-1] & LCHAR_IS_CJK) && !(fmt->m_flags[i-1] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) &&
                                 !(fmt->m_flags[i] & (LCHAR_IS_CJK|LCHAR_IS_OBJECT|LCHAR_IS_SPACE)) ) {
                        // Previous char is a non-flexible CJK (so, not a CJK punctuation).
                        // Current char is not a CJK, object nor space.
                        lUInt16 props = lGetCharProps(fmt->m_text[i]);
                        if ( !CH_PROP_IS_PUNCT(props) && !(props & CH_PROP_SPACE) ) {
                            // Current char is not a punctuation and not some other kind of space.
                            LVFont * fnt = (LVFont *)fmt->m_srcs[i-1]->t.font;
                            spaceReduceWidth -= fnt->getSize() / 4;
                        }
                    }
                }

                bool grabbedExceedingSpace = false;
                if ( x + fmt->m_advance[i]-w0 > maxWidth + spaceReduceWidth ) {
                    // It's possible the char at i is a space whose width exceeds maxWidth,
                    // but it should be a candidate for lastNormalWrap (otherwise, the
                    // previous word will be hyphenated and we will get spaces widen for
                    // text justification)
                    if ( (flags & LCHAR_IS_SPACE) && (flags & LCHAR_ALLOW_WRAP_AFTER) ) // don't break yet
                        grabbedExceedingSpace = true;
                    else if ( flags & LCHAR_IS_CJK && lastNormalWrap < i-1 ) {
                        // This CJK char doesn't fit, previous char did fit but a wrap is not allowed between
                        // them: wrapping before previous char would cause a hole at end of line of at least
                        // one CJK glyph (which would be counteracted, if the line is to be justified, by
                        // spreading out all glyphs on the line).
                        // If we have seen some flexible CJK punctuations, we can steal some width from
                        // them to possibly make both chars fit on the line.
                        // It is also possible this char is itself flexible and would fit if reduced.
                        int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                        bool does_fit = false;
                        if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                            // Check if this flexible char can/should be reduced if at end of line
                            bool can_add_space_before, can_add_space_after; // not used here
                            // We provide end=i+1 ('end' is exclusive) to see how this char does when at end of line
                            int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, i+1, can_add_space_before, can_add_space_after);
                            if ( wa8 != 8 ) {
                                // This char can/should be smaller (ie. halfwidth) if at end of line:
                                // see if it would then fit if made that smaller.
                                if ( wa8 < 0 )
                                    wa8 = -wa8;
                                w = w * wa8 / 8; // floor'ed, to get more chance to fit
                                if ( x + fmt->m_advance[i-1]-w0 + w <= maxWidth + spaceReduceWidth ) {
                                    does_fit = true;
                                }
                                // If it doesn't fit when just itself smaller, we'll do the check
                                // just below with its reduced width.
                            }
                        }
                        if ( !does_fit && w <= cjkReduceWidth ) {
                            // It would fit if we steal space from previous "can be smaller" chars, as they
                            // provide enough stealable space.
                            // Transfer the required width from stolen from cjkReduceWidth into spaceReduceWidth,
                            // so that we now fit and can go on (current char may still not have LCHAR_ALLOW_WRAP_AFTER,
                            // and we may end up grabbing more of the upcoming chars, or just end up using
                            // the previous lastNormalWrap if we don't meet any that allow a wrap).
                            if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                                // It's possible for a flexible char to get a different width whether at
                                // end of line or in the middle, and possibly a larger one when in the
                                // middle (ie. fullstop in Japanese). We need here to get the width it
                                // would have later when followed, and account this width in the transfer.
                                bool can_add_space_before, can_add_space_after; // not used here
                                // We now provide end=fmt->m_length to state we're not at end of line
                                int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                                if ( wa8 != 8 ) {
                                    if ( wa8 < 0 )
                                        wa8 = -wa8;
                                    w = w * wa8 / 8; // floor'ed
                                }
                            }
                            spaceReduceWidth += w;
                            cjkReduceWidth -= w;
                            does_fit = true;
                        }
                        if ( !does_fit ) {
                            break;
                        }
                        // Note: we steal from cjkReduceWidth only here when trying to make another CJK char
                        // fit on the line. We could try to also steal from them when adding non-CJK chars,
                        // which would make western longer words, and would probably fail finding a wrap
                        // anyway (and would feel a bit agressive if a wrap is found thanks to them).
                        // This means that some small words like small numbers (ie. "12"), that could have
                        // fitted if we grabbed some pixels from cjkReduceWidth, will be pushed to next
                        // line and the previous line will use expansion for justification.
                    }
                    else
                        break;
                }
                #if (USE_LIBUNIBREAK==1)
                // Note: with libunibreak, we can't assume anymore that LCHAR_ALLOW_WRAP_AFTER is synonym to IS_SPACE.
                if (flags & LCHAR_ALLOW_WRAP_AFTER) {
                    if (flags & LCHAR_DEPRECATED_WRAP_AFTER) {
                        // Allowed by libunibreak, but prevented by "white-space: nowrap" on
                        // this text node parent. Store this opportunity as lastDeprecatedWrap,
                        // that we will use only if no lastNormalWrap found.
                        lastDeprecatedWrap = i;
                    }
                    else {
                        lastNormalWrap = i;
                    }
                }
                #else
                // A space or a CJK ideograph make a normal allowed wrap
                // Note: upstream has added in:
                //   https://github.com/buggins/coolreader/commit/e2a1cf3306b6b083467d77d99dad751dc3aa07d9
                // to the next if:
                //  || lGetCharProps(fmt->m_text[i]) == 0
                // but this does not look right, as any other unicode char would allow wrap.
                if ((flags & LCHAR_ALLOW_WRAP_AFTER) || (fmt->m_flags[i] & LCHAR_IS_CJK)) {
                    // Need to check if previous and next non-space char request a wrap on
                    // this space (or CJK char) to be avoided
                    bool avoidWrap = false;
                    // Look first at following char(s)
                    for (int j = i+1; j < fmt->m_length; j++) {
                        if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                            if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX) // skip floats
                                continue;
                            else // allow wrap between space/CJK and image or inline-box
                                break;
                        }
                        if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) { // not another (collapsible) space
                            avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_BEFORE;
                            break;
                        }
                    }
                    if (!avoidWrap && i < fmt->m_length-1) { // Look at preceding char(s)
                        // (but not if it is the last char, where a wrap is fine
                        // even if it ends after a CH_PROP_AVOID_WRAP_AFTER char)
                        for (int j = i-1; j >= 0; j--) {
                            if ( fmt->m_flags[j] & LCHAR_IS_OBJECT ) {
                                if (fmt->m_charindex[j] == FLOAT_CHAR_INDEX) // skip floats
                                    continue;
                                else // allow wrap after a space following an image or inline-box
                                    break;
                            }
                            if ( !(fmt->m_flags[j] & LCHAR_ALLOW_WRAP_AFTER) ) { // not another (collapsible) space
                                avoidWrap = lGetCharProps(fmt->m_text[j]) & CH_PROP_AVOID_WRAP_AFTER;
                                break;
                            }
                        }
                    }
                    if (!avoidWrap)
                        lastNormalWrap = i;
                    // We could use lastDeprecatedWrap, but it then get too much real chances to be used:
                    // else lastDeprecatedWrap = i;
                    // Note that a wrap can happen AFTER a '-' (that has CH_PROP_AVOID_WRAP_AFTER)
                    // when lastDeprecatedWrap is prefered below.
                }
                else if ( flags & LCHAR_DEPRECATED_WRAP_AFTER ) {
                    // Different meaning than when USE_LIBUNIBREAK: it is set
                    // by lastFont->fmt->measureText() on some hyphens.
                    // (To keep this legacy behaviour and not complexify things, we don't
                    // ensure "white-space: nowrap" when not using libunibreak.)
                    lastDeprecatedWrap = i; // Hyphens make a less priority wrap
                }
                #endif // not USE_LIBUNIBREAK==1
                if ( i==fmt->m_length-1 ) // Last char always provides a normal wrap
                    lastNormalWrap = i;
                if ( !grabbedExceedingSpace &&
                        fmt->m_pbuffer->min_space_condensing_percent != 100 &&
                        i < fmt->m_length-1 &&
                        ( fmt->m_flags[i] & LCHAR_IS_SPACE ) && !( fmt->m_flags[i] & LCHAR_LOCKED_SPACING ) &&
                        !(fmt->m_flags[i+1] & LCHAR_IS_SPACE) ) {
                    // Each space not followed by a space is candidate for space condensing
                    int dw = getMaxCondensedSpaceTruncation(fmt,i);
                    if ( dw>0 )
                        spaceReduceWidth += dw;
                }
                else if ( fmt->m_flags[i] & LCHAR_IS_FLEXIBLE_WIDTH_CJK ) {
                    bool can_add_space_before, can_add_space_after; // not used here
                    // Unlike above, we don't provide end=i+1, as this char fits and we want to know how
                    // this char does followed by its neighbour, as we're not done making the line.
                    int wa8 = fmt->getFlexibleCJKWidthAdjustment(i, pos, fmt->m_length, can_add_space_before, can_add_space_after);
                    if ( wa8 != 8 ) {
                        if ( wa8 < 0 ) { // can be reduced (ie. halfwidth)
                            // This reduction is not to be made available yet: account it in cjkReduceWidth,
                            // that we will steal from (and transfer into spaceReduceWidth) if needed.
                            wa8 = -wa8;
                            int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                            cjkReduceWidth += w - (w * wa8 / 8);
                            // Here and below, we ceil the stealable width, so we are able
                            // to fit a (floored) reduced-width char if there is only one other
                            // flexible char on this line.
                        }
                        else if ( wa8 > 0 ) { // should be reduced (ie. halfwidth)
                            // Account the reduction as we do for spaces, as it is usable from now on.
                            int w = (fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i-1] : 0));
                            spaceReduceWidth += w - (w * wa8 / 8);
                        }
                    }
                }
                if (grabbedExceedingSpace)
                    break; // delayed break
            }

            // Glyph at i exceeds available width, or mandatory break. We have
            // found a lastNormWrap, and computed spaceReduceWidth.

            // It feels there's no need to do anything if there's been one single float
            // that took all the width: we moved i and can wrap.
            if (i<=pos)
                i = pos + 1; // allow at least one character to be shown on line
            int wordpos = i-1; // Last char which fits: hyphenation does not need to check further

            #if (USE_LIBUNIBREAK==1)
                // If no normal wrap found, and if we have a deprecated wrap (a normal wrap
                // as determined by libunibreak, but prevented by "white-space: nowrap",
                // it's because the line has no wrap opportunity outside nodes with
                // "white-space: nowrap".
                // We need to wrap, and it's best to do so at a regular opportunity rather
                // than at some arbitrary point: do as it there were no "nowrap".
                if ( lastNormalWrap < 0 && lastDeprecatedWrap > 0 ) {
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif
            int normalWrapWidth = lastNormalWrap > 0 ? x + fmt->m_advance[lastNormalWrap]-w0 : 0;
            int unusedSpace = maxWidth - normalWrapWidth;
            int unusedPercent = maxWidth > 0 ? unusedSpace * 100 / maxWidth : 0;
            #if (USE_LIBUNIBREAK!=1)
                // (Different usage of deprecatedWrap than above)
                int deprecatedWrapWidth = lastDeprecatedWrap > 0 ? x + fmt->m_advance[lastDeprecatedWrap]-w0 : 0;
                if ( deprecatedWrapWidth > normalWrapWidth && unusedPercent > 3 ) { // only 3%
                    lastNormalWrap = lastDeprecatedWrap;
                }
            #endif

            // If, with normal wrapping, more than 5% of the line would not be used,
            // try to find a word (from where we stopped back to lastNormalWrap) to
            // hyphenate, if hyphenation is not forbidden by CSS.
            // todo: decide if we should hyphenate if bidi is happening up to now
            tryHyphenBreak(fmt, pos, wordpos, lastNormalWrap, lastMandatoryWrap,
                           x, w0, maxWidth, spaceReduceWidth, unusedPercent, lastHyphWrap);

            // Decide best position to end this line
            int wrapPos = lastHyphWrap;
            if ( lastMandatoryWrap>=0 )
                wrapPos = lastMandatoryWrap;
            else {
                if ( wrapPos < lastNormalWrap )
                    wrapPos = lastNormalWrap;
                if ( wrapPos < 0 ) // no wrap opportunity (e.g. very long non-hyphenable word)
                    wrapPos = i-1;
                #if (USE_LIBUNIBREAK!=1)
                if ( wrapPos <= upSkipPos ) {
                    // Ensure that what, when dealing with previous line, we pushed to
                    // next line (below) is actually on this new line.
                    //CRLog::trace("guard old wrapPos at %d", wrapPos);
                    wrapPos = upSkipPos+1;
                    //CRLog::trace("guard new wrapPos at %d", wrapPos);
                    upSkipPos = -1;
                }
                #endif
            }
            // End (not included) of current line
            int endp = wrapPos + (lastMandatoryWrap<0 ? 1 : 0);

            // Specific handling of CJK punctuation that should not happen at start or
            // end of line. When using libunibreak, we trust it to handle them correctly.
            #if (USE_LIBUNIBREAK!=1)
            // The following looks left (up) and right (down) if there are any chars/punctuation
            // that should be prevented from being at the end of line or start of line, and if
            // yes adjust wrapPos so they are pushed to next line, or brought to this line.
            // It might be a bit of a duplication of what's done above (for latin punctuations)
            // in the avoidWrap section.
            int downSkipCount = 0;
            int upSkipCount = 0;
            if (endp > 1 && isCJKLeftPunctuation(*(fmt->m_text + endp))) {
                // Next char will be fine at the start of next line.
                //CRLog::trace("skip skip punctuation %s, at index %d", LCSTR(lString32(fmt->m_text+endp, 1)), endp);
            } else if (endp > 1 && endp < fmt->m_length - 1 && isCJKLeftPunctuation(*(fmt->m_text + endp - 1))) {
                // Most right char is left punctuation: go back 1 char so this one
                // goes onto next line.
                upSkipPos = endp;
                endp--; wrapPos--;
                //CRLog::trace("up skip left punctuation %s, at index %d", LCSTR(lString32(fmt->m_text+endp, 1)), endp);
            } else if (endp > 1 && isCJKPunctuation(*(fmt->m_text + endp))) {
                // Next char (start of next line) is some right punctuation that
                // is not allowed at start of line.
                // Look if it's better to wrap before (up) or after (down), and how
                // much up or down we find an adequate wrap position, and decide
                // which to use.
                for (int epos = endp; epos<fmt->m_length; epos++, downSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                   //CRLog::trace("down skip punctuation %s, at index %d", LCSTR(lString32(fmt->m_text + epos, 1)), epos);
                }
                for (int epos = endp; epos>=start; epos--, upSkipCount++) {
                   if ( !isCJKPunctuation(*(fmt->m_text + epos)) ) break;
                   //CRLog::trace("up skip punctuation %s, at index %d", LCSTR(lString32(fmt->m_text + epos, 1)), epos);
                }
                if (downSkipCount <= upSkipCount && downSkipCount <= 2 && false ) {
                            // last check was "&& fmt->m_hanging_punctuation", but we
                            // have to skip that in this old code after the hanging
                            // punctuation handling changes
                   // Less skips if we bring next char on this line, and hanging
                   // punctuation is enabled so this punctuation will naturally
                   // find it's place in the reserved right area.
                   endp += downSkipCount;
                   wrapPos += downSkipCount;
                   //CRLog::trace("finally down skip punctuations %d", downSkipCount);
                } else if (upSkipCount <= 2) {
                   // Otherwise put it on next line (spaces or inter-ideograph spaces
                   // will be expanded for justification).
                   upSkipPos = endp;
                   endp -= upSkipCount;
                   wrapPos -= upSkipCount;
                   //CRLog::trace("finally up skip punctuations %d", upSkipCount);
                }
            }
            #endif
            if (endp > fmt->m_length)
                endp = fmt->m_length;

            if ( is_css_first_line ) {
                is_css_first_line = false;
                if ( first_line_sequance_end_reached ) {
                    // We're done: let us be exiting this loop properly
                    wrapPos = fmt->m_length-1;
                }
                else {
                    // We had a copy of the source text with CSS first-line styling,
                    // and we did not meet its end. We should fast forward skipping
                    // that first-line sequence, and once in the normal text sequence
                    // (which includes the full text since the start of the paragraph)
                    // skip the part that has just been output as first-line to restart
                    // on after where we wrapped.
                    // We initially assumed we would get the same text content in the
                    // first-line sequence as in the normal text copy we get after it,
                    // but when a list item marker is prepended, we do not.
                    // So, get the node (a cloneNode) and offset at which we stopped on,
                    // get its source node, and try to find it in the normal sequence:
                    // we can then restart on it at the same offset/charindex.
                    lUInt16 orig_offset = 0;
                    ldomNode * orig_node = (ldomNode *) fmt->m_srcs[wrapPos]->object;
                    if ( orig_node ) {
                        orig_node = orig_node->getCloneNodeSource();
                        orig_offset = fmt->m_charindex[wrapPos];
                    }
                    // First, skip from here until non-first-line-clone
                    int i = wrapPos;
                    while (i < fmt->m_length && fmt->m_srcs[i]->flags & LTEXT_IS_FIRST_LINE_CLONE)
                        i++;
                    int normal_sequence_start = i; // in case !found
                    bool found = false;
                    if ( orig_node ) { // look for it
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
                        // If we did not find it, go with our initial assumption:
                        // move in the non-first-line as much as we walked until wrap
                        // in the first-line sequence
                        i = normal_sequence_start + wrapPos;
                    }
                    // And use that as wrapPos for what follows.
                    wrapPos = i;
                    if (wrapPos >= fmt->m_length)
                        wrapPos = fmt->m_length-1;
                    // printf("first line done, moving from %d to %d (found=%d)\n", endp, i, found);
                }
            }

            // Best position to end this line found.
            bool hasInlineBoxes = firstInlineBoxPos >= 0 && firstInlineBoxPos < endp;
            addLineHorizontal( fmt, pos, endp, x, para, pos==0, wrapPos>=fmt->m_length-1, preFormattedOnly, isLastPara, hasInlineBoxes);
            pos = wrapPos + 1; // start of next line

            #if (USE_LIBUNIBREAK==1)
            // (Only when using libunibreak, which we trust decisions to wrap on hyphens.)
            if ( fmt->m_srcs[wrapPos]->lang_cfg->duplicateRealHyphenOnNextLine() && pos > 0 && pos < fmt->m_length-1 ) {
                if ( fmt->m_text[wrapPos] == '-' || fmt->m_text[wrapPos] == UNICODE_HYPHEN ) {
                    pos--; // Have that last hyphen also at the start of next line
                           // (small caveat: the duplicated hyphen at start of next
                           // line won't be part of the highlighted text)
                    // And forbid a break after this duplicated hyphen (this avoids
                    // a possible infinite loop and out of memory when no allowed
                    // wrap is found on next line, as we would continuously AddLine()
                    // lines with only this hyphen)
                    fmt->m_flags[pos] &= ~LCHAR_ALLOW_WRAP_AFTER;
                }
            }
            #endif
        }
    }

void processEmbeddedBlockHorizontal( LVFormatter* fmt, int idx )
    {
        ldomNode * node = (ldomNode *) fmt->m_pbuffer->srctext[idx].object;
        // Use current width available at current y position for the whole block
        // (Firefox would lay out this block content around the floats met along
        // the way, but it would be quite tedious to do the same... so, we don't).
        int width = fmt->getCurrentLineWidth();
        int block_x = fmt->getCurrentLineX();
        int cur_y = fmt->m_line_advance;

        bool already_rendered = false;
        { // in its own scope, so this RenderRectAccessor is forgotten when left
            RenderRectAccessor node_fmt( node );
            if ( RENDER_RECT_HAS_FLAG(node_fmt, BOX_IS_RENDERED) ) {
                already_rendered = true;
            }
        }
        // On the first rendering (after type settings changes), we want to forward
        // this block individual lines to the main page splitting context.
        // But on later calls (once already_rendered), used for drawing or text
        // selection, we want to have a single line with the inlineBox.
        // We'll mark the first rendering with is_reusable=false, so that we go
        // reformatting this final node when we need to draw it.
        // (We could mix the individual lines with the main inlineBox line, but
        // that would need added code at various places to ignore one or the
        // others depending on what's needed there.)
        if ( !already_rendered ) {
            LVRendPageContext context( NULL, fmt->m_pbuffer->page_height );
            // We don't know if the upper LVRendPageContext wants lines or not,
            // so assume it does (the main flow does).
            int rend_flags = node->getDocument()->getRenderBlockRenderingFlags();
            // We want to avoid negative margins (if allowed in global flags) and
            // going back the flow y, as the transfered lines would not reflect
            // that, and we could get some small mismatches and glitches.
            rend_flags &= ~BLOCK_RENDERING_ALLOW_NEGATIVE_COLLAPSED_MARGINS;
            int baseline = REQ_BASELINE_FOR_TABLE; // baseline of block is baseline of its first line
            // The same usable overflows provided for the container (possibly
            // adjusted for floats) can be used for this full-width inlineBox.
            int usable_left_overflow;
            int usable_right_overflow;
            fmt->getCurrentLineUsableOverflows(usable_left_overflow, usable_right_overflow);
            renderBlockElement( context, node, 0, 0, width, usable_left_overflow, usable_right_overflow,
                                fmt->m_specified_para_dir, &baseline, rend_flags);
            RenderRectAccessor node_fmt( node );
            node_fmt.setX(block_x);
            node_fmt.setY(fmt->m_line_advance);
            node_fmt.setBaseline(baseline);
            RENDER_RECT_SET_FLAG(node_fmt, BOX_IS_RENDERED);
            // Transfer individual lines from this sub-context into real frmlines (they
            // will be transferred to the upper context by renderBlockElementEnhanced())
            if ( context.getLines() ) {
                LVPtrVector<LVRendLineInfo> * lines = context.getLines();
                for ( int i=0; i < lines->length(); i++ ) {
                    LVRendLineInfo * line = lines->get(i);
                    formatted_line_t * frmline = lvtextAddFormattedLine( fmt->m_pbuffer );
                    frmline->x = block_x;
                    frmline->y = cur_y + line->getStart();
                    frmline->height = line->getHeight();
                    frmline->flags = line->getFlags();
                    if (fmt->m_has_ongoing_float)
                        frmline->flags |= LTEXT_LINE_SPLIT_AVOID_BEFORE;
                    // Unfortunaltely, we can't easily forward footnotes links
                    // gathered by this sub-context via frmlines.
                    // printf("emb line %d>%d\n", frmline->y, frmline->height);
                    fmt->m_line_advance += frmline->height;
                    // We only check for already positioned floats to ensure
                    // no page break along them. We'll positioned yet-to-be
                    // positioned floats only when done with this embedded block.
                    fmt->checkOngoingFloat();
                }
            }
            // Next time we have to use this LFormattedText for drawing, have it
            // trashed: we'll re-format it by going into the following 'else'.
            fmt->m_pbuffer->is_reusable = false;
        }
        else {
            RenderRectAccessor node_fmt( node );
            int height = node_fmt.getHeight();
            formatted_line_t * frmline = lvtextAddFormattedLine( fmt->m_pbuffer );
            frmline->x = block_x;
            frmline->width = width; // single word width
            frmline->y = cur_y;
            frmline->height = height;
            frmline->flags = 0; // no flags needed once page split has been done
            // printf("final line %d>%d\n", frmline->y, frmline->height);
            // This line has a single word: the inlineBox.
            formatted_word_t * word = lvtextAddFormattedWord(frmline);
            word->src_text_index = idx;
            word->flags = LTEXT_WORD_IS_INLINE_BOX;
            word->x = 0;
            word->width = width;
            fmt->m_line_advance = cur_y + height;
            fmt->m_pbuffer->height = fmt->m_line_advance;
        }
        // Not tested how this would work with floats...
        fmt->checkOngoingFloat();
        fmt->positionDelayedFloats();
    }


void LFormattedText::Draw( LVDrawBuf * buf, int x, int y, ldomMarkedRangeList * marks, ldomMarkedRangeList *bookmarks )
{
    int i, j;
    formatted_line_t * frmline;
    src_text_fragment_t * srcline;
    formatted_word_t * word;
    LVFont * font;
    lvRect clip;
    buf->GetClipRect( &clip );
    const lChar32 * str;
    bool is_vertical = (css_wm_is_vertical(m_pbuffer->writing_mode));
    if (is_vertical) {
        // DrawDocument passes (actual_Y, actual_X) as (x, y) due to the Y=X
        // coordinate mapping used throughout the rendering/page-split pipeline.
        // Swap them back to screen coordinates.
        int tmp = x; x = y; y = tmp;
    }
    int line_y = y;
    // For vertical-rl: line_x is the right edge of the first (rightmost) column.
    // x = draw_y = doc_y + y0 encodes the horizontal document offset.
    // clip.right - x maps this to the column's screen X position, ensuring:
    //  - doc_y=0 (first block) → line_x near clip.right (rightmost)
    //  - doc_y increases → line_x decreases (columns shift left)
    // Using only x (not y which encodes vertical position) avoids accidental
    // column overlap between blocks with different doc_x ancestry.
    int line_x = is_vertical ? (clip.right - x) : x;
    draw_extra_info_t * draw_extra_info = (draw_extra_info_t*)buf->GetDrawExtraInfo();

    bool ignore_clip = false;
    if ( m_pbuffer->frmlinecount == 1 && m_pbuffer->frmlines[0]->word_count > 0 ) {
        // If the first word of a single line block has LTEXT_MATH_TRANSFORM,
        // it's a single word that is a <mo> that might be stretched by the
        // font drawing code: ignore the clip as the original glyph might be
        // outside, but we want any part of the stretched glyph to be rendered.
        srcline = &m_pbuffer->srctext[m_pbuffer->frmlines[0]->words[0].src_text_index];
        if ( srcline->flags & LTEXT_MATH_TRANSFORM )
            ignore_clip = true;
    }

    // We might need to translate "marks" (native highlights) from relative
    // coordinates to absolute coordinates if we have to draw floats or
    // inlineBoxes: we'll do that when dealing with the first of these if any.
    ldomMarkedRangeList * absmarks = new ldomMarkedRangeList();
    bool absmarks_update_needed = marks!=NULL && marks->length()>0;

    // printf("x/y: %d/%d clip.top/bottom: %d %d\n", x, y, clip.top, clip.bottom);
    // When drawing a paragraph that spans 3 pages, we may get:
    //   x/y: 9/407 clip.top/bottom: 13 559
    //   x/y: 9/-139 clip.top/bottom: 13 583
    //   x/y: 9/-709 clip.top/bottom: 13 545

    // Vertical-rl bleed detection (work-in-progress, currently inactive).
    // Investigation found that all detected cases were false positives:
    //   - font_size-vs-strut_height offset (8px): systematic, not a visual bug
    //   - large backward jumps: column boundary transitions, not misplacements
    // Root cause of the actual "上にめり込む" bug is still unknown.
    // Counters (ltext_vert_bleed_count/max_px) and Lua API are kept for
    // future investigation via doc._document:resetVertBleedCounters() /
    // getVertBleedStats().
    int vert_prev_plain_y0 = -1;         // y0 of last drawn plain/CJK char in this column
    int vert_prev_effective_width = 0;   // effective_width of that char (= its slot height)

    for (i=0; i<m_pbuffer->frmlinecount; i++)
    {
        if (is_vertical) {
            if (line_x < clip.left && !ignore_clip)
                break;
        } else {
            if (line_y >= clip.bottom && !ignore_clip)
                break;
        }
        frmline = m_pbuffer->frmlines[i];
        bool line_visible = is_vertical
            ? ((line_x <= clip.right && line_x - (int)frmline->height >= clip.left) || ignore_clip)
            : (line_y + frmline->height > clip.top || ignore_clip);
        if (line_visible)
        {
            // This line box is or has some part in the page regular clip.
            // If it is fully inside the regular clip, we extend the clip
            // to the provided content_overflow_clip to allow any glyph
            // extending outside the line box (which can happen with a small
            // interline space) to be drawn fully in the top or bottom margins.
            // (We can't allow this for lines only partially in the clip, at
            // least because of the case of isEmbeddedBlockBoxingInlineBox()
            // below (big single line box possible spanning multiple pages)
            // whose inner content lines will have to go thru the above
            // regular clip check if we want to avoid the same inner line
            // to appear on both prev and next pages.)
            bool restore_orig_clip = false;
            lvRect origClip;
            // For vertical text, track the minimum next-allowed word->x position.
            // Japanese punctuation (、。) has negative advances stored as large lUInt16
            // values (e.g., -24 → 65512), causing word->x to retrograde (decrease) and
            // producing character overlap. When a retrograde is detected, we advance by
            // the previous normal word width instead, ensuring monotonically increasing
            // y positions for all glyphs.
            int vert_min_next_x = 0;
            // Reset per-column plain-char tracking at the start of each frmline (column)
            // to avoid false-positive overlap reports across column boundaries.
            vert_prev_plain_y0 = -1;
            vert_prev_effective_width = 0;
            if ( line_y >= clip.top && line_y + frmline->height <= clip.bottom ) {
                if ( draw_extra_info ) {
                    restore_orig_clip = true;
                    buf->GetClipRect( &origClip );
                    buf->SetClipRect( &draw_extra_info->content_overflow_clip );
                }
            }

            // process background (first) and borders (which may be drawn over background)
            bool has_inline_borders = false;

            // draw background for each word
            // (if multiple consecutive words share the same bgcolor, this will
            // actually fill a single rect encompassing these words)
            // todo: the way background color (not inherited in lvrend.cpp) is
            // handled here (only looking at the style of the inline node
            // that contains the word, and not at its other inline parents),
            // some words may not get their proper bgcolor
            // todo: this should better be handled as done for top/bottom border below,
            // with a flag and looking at parent nodes (and no need to pass a bgcl
            // to AddSourceLine()).
            // In vertical-rl mode coordinates are swapped: lastWordStart/End are screen-Y
            // values (character slot in column), line_x/frmline->height give the column
            // screen-X extent.  fillWordBgRect dispatches to the correct geometry.
            auto fillWordBgRect = [&](int wstart, int wend, lUInt32 color) {
                if (is_vertical)
                    buf->FillRect(line_x - (int)frmline->height, y + wstart, line_x, y + wend, color);
                else
                    buf->FillRect(wstart, y + frmline->y, wend, y + frmline->y + frmline->height, color);
            };
            lUInt32 lastWordColor = LTEXT_COLOR_CURRENT; // meaning unset, no bgcolor yet
            int lastWordStart = -1;
            int lastWordEnd = -1;
            for (j=0; j<frmline->word_count; j++)
            {
                word = &frmline->words[j];
                srcline = &m_pbuffer->srctext[word->src_text_index];
                if ( (srcline->flags & LTEXT_HAS_EXTRA) && getLTextExtraProperty(srcline, LTEXT_EXTRA_CSS_HIDDEN) && !buf->WantsHiddenContent() )
                    continue;
                if ( srcline->flags & LTEXT_HAS_TOP_BOTTOM_BORDER ) {
                    has_inline_borders = true;
                }
                if (word->flags & LTEXT_WORD_IS_IMAGE)
                {
                    // no background, TODO
                }
                else if (word->flags & LTEXT_WORD_IS_INLINE_BOX)
                {
                    // background if any will be drawn when drawing the box below
                }
                else if (word->flags & LTEXT_WORD_IS_PAD)
                {
                    // Draw background over left/right margin + border
                    bool is_right_pad = srcline->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT;
                    bool is_mirrored = word->flags & LTEXT_WORD_DIRECTION_IS_RTL; // will be drawn as if on the other side
                    ldomNode * node = (ldomNode *) srcline->object;
                    css_style_ref_t style = node->getStyle();
                    lUInt32 bgcl = style->background_color.type == css_val_color ? // "currentcolor" if not
                                            style->background_color.value : style->color.value;
                    if ( !IS_COLOR_FULLY_TRANSPARENT(bgcl) ) { // background color to start/continue/end
                        bgcl = LTEXT_COLOR_IS_RESERVED(bgcl) ? LTEXT_COLOR_RESERVED_REPLACE : bgcl;
                        if ( is_right_pad != is_mirrored ) { // unmirrored right pad, or mirrored left pad
                            if ( lastWordStart!=-1 && lastWordColor!=bgcl ) {
                                // Draw the background of a different color for previous words
                                if ( ((lastWordColor>>24) & 0xFF) != 0xFF ) // Not reserved, not alpha=100% (not transparent)
                                    fillWordBgRect( lastWordStart, lastWordEnd, lastWordColor );
                                lastWordStart = -1;
                            }
                            // Draw the background for this pad up to its padding+border, but not its margin
                            if ( lastWordStart < 0 ) {
                                lastWordStart = x + frmline->x + word->x;
                            }
                            lastWordEnd = x + frmline->x + word->x + word->o.height; // padding+border-right
                            lastWordColor = bgcl;
                            if ( ((lastWordColor>>24) & 0xFF) != 0xFF ) // Not reserved, not alpha=100% (not transparent)
                                fillWordBgRect( lastWordStart, lastWordEnd, lastWordColor );
                            lastWordStart = -1;
                            lastWordEnd = -1;
                            lastWordColor = LTEXT_COLOR_CURRENT;
                        }
                        else { // unmirrored left pad, or mirrored right pad
                            if ( lastWordColor!=bgcl || lastWordStart==-1 ) {
                                // Draw the background of a different color for previous words
                                if ( lastWordStart!=-1 )
                                    if ( ((lastWordColor>>24) & 0xFF) != 0xFF ) // Not reserved, not alpha=100% (not transparent)
                                        fillWordBgRect( lastWordStart, lastWordEnd, lastWordColor );
                                // Next drawing will include this pad's padding+border-left
                                lastWordColor=bgcl;
                                lastWordStart = x + frmline->x + word->x + word->width - word->o.height;
                            }
                            lastWordEnd = x+frmline->x+word->x+word->width;
                        }
                    }
                    if ( word->o.baseline ) { // We have some left/right border to draw, that we'll do below
                        has_inline_borders = true;
                    }
                }
                else
                {
                    lUInt32 bgcl = srcline->bgcolor;
                    if ( lastWordColor!=bgcl || lastWordStart==-1 ) {
                        if ( lastWordStart!=-1 )
                            if ( ((lastWordColor>>24) & 0xFF) != 0xFF ) // Not reserved, not alpha=100% (not transparent)
                                fillWordBgRect( lastWordStart, lastWordEnd, lastWordColor );
                        lastWordColor=bgcl;
                        lastWordStart = x+frmline->x+word->x;
                    }
                    lastWordEnd = x+frmline->x+word->x+word->width;
                }
            }
            if ( lastWordStart!=-1 ) {
                if ( ((lastWordColor>>24) & 0xFF) != 0xFF )
                    fillWordBgRect( lastWordStart, lastWordEnd, lastWordColor );
            }

            // Draw borders if we noticed there could be some
            // Top/bottom borders support is a bit limited and not per-specs: we don't handle any
            // top/bottom padding, and we draw a single border (the one set by the closest parent,
            // ignoring any other set by a further parent) at the line box edges.
            // (So, increasing line-height to make the borders from 2 lines more noticable/disctinct
            // won't work: the borders will go away from the text, but will continue to stick to each
            // others... It would be quite a lot more complicated to handle this properly, and hopefully
            // this implementation is good enough in practice.)
            // This limitations is also quite noticable with <img> having borders: the border may be
            // drawn over by the image, or there may be blanks between the image and some border
            // sides (ie. if an image sits at the baseline, the border will be drawn under the strut,
            // leaving some gap below the image)...
            if ( has_inline_borders ) {
                // Draw top border, and then bottom border (we use the same kind
                // of logic with lastWordStart/End as for background color above)
                for (int side=0 ; side <=2; side+=2) {
                    ldomNode * lastBorderNode = NULL;
                    int lastBorderWordStart = -1;
                    int lastBorderWordEnd = -1;
                    for (j=0; j<frmline->word_count; j++) {
                        word = &frmline->words[j];
                        srcline = &m_pbuffer->srctext[word->src_text_index];
                        ldomNode * node = (ldomNode *) srcline->object;
                        ldomNode * thisBorderNode = NULL;
                        if ( srcline->flags & LTEXT_HAS_TOP_BOTTOM_BORDER ) {
                            // Find out the nearest parent node that carries some border
                            ldomNode * tmp = node;
                            if (tmp->isEffectiveText())
                                tmp = tmp->getParentNode();
                            while ( tmp && tmp->getRendMethod() != erm_final ) {
                                int border = measureBorder(tmp, side);
                                if ( border > 0 ) {
                                    thisBorderNode = tmp;
                                    break;
                                }
                                tmp = tmp->getParentNode();
                            }
                        }
                        if ( thisBorderNode != lastBorderNode && lastBorderWordStart != -1 ) {
                            // The previous border over previous words ends: draw it
                            drawBorder(buf, lastBorderWordStart, lastBorderWordEnd,
                                        y+frmline->y, frmline->height, lastBorderNode, side);
                            lastBorderWordStart = -1;
                        }
                        lastBorderNode = thisBorderNode;
                        if ( thisBorderNode ) {
                            if ( word->flags & LTEXT_WORD_IS_PAD && thisBorderNode == node) {
                                // This is a pad that is also the one providing the border: make lastBorderWordStart/End
                                // shorter to account for outer left/right margin
                                bool is_right_pad = srcline->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT;
                                bool is_mirrored = word->flags & LTEXT_WORD_DIRECTION_IS_RTL; // will be drawn as if on the other side
                                if ( is_right_pad != is_mirrored ) { // unmirrored right pad, or mirrored left pad
                                    if ( lastBorderWordStart < 0 )
                                        lastBorderWordStart = x + frmline->x + word->x;
                                    lastBorderWordEnd = x + frmline->x + word->x + word->o.height;
                                }
                                else { // unmirrored left pad, or mirrored right pad
                                    lastBorderWordStart = x + frmline->x + word->x + word->width - word->o.height;
                                    lastBorderWordEnd = x + frmline->x + word->x + word->width;
                                }
                            }
                            else { // normal word: use its full width
                                if ( lastBorderWordStart < 0 )
                                    lastBorderWordStart = x + frmline->x + word->x;
                                lastBorderWordEnd = x + frmline->x + word->x + word->width;
                            }
                        }
                        else { // no new border
                            lastBorderWordStart = -1;
                            lastBorderWordEnd = -1;
                        }
                    }
                    // Done with this line, any previous border ends: draw it
                    if ( lastBorderNode && lastBorderWordStart != -1 ) {
                        drawBorder(buf, lastBorderWordStart, lastBorderWordEnd,
                                    y+frmline->y, frmline->height, lastBorderNode, side);
                    }
                }

                // Draw left/right border on pads.
                // We draw it after any top/bottom border, so it can be drawn over them
                // and be noticable (otherwise, top/bottom drawn over left/right margin
                // would reduce their visible height and make them shorted, possible
                // not noticable if dotted/dashed of a different color).
                for (j=0; j<frmline->word_count; j++) {
                    word = &frmline->words[j];
                    srcline = &m_pbuffer->srctext[word->src_text_index];
                    if (word->flags & LTEXT_WORD_IS_PAD && word->o.baseline ) { // there is some border to draw
                        bool is_right_pad = srcline->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT;
                        bool is_mirrored = word->flags & LTEXT_WORD_DIRECTION_IS_RTL; // will be drawn as if on the other side
                        ldomNode * node = (ldomNode *) srcline->object;
                        if ( is_right_pad != is_mirrored ) { // unmirrored right pad, or mirrored left pad
                            int x0 = x + frmline->x + word->x + word->o.height - word->o.baseline;
                            int x1 = x0 + word->o.baseline;
                            drawBorder(buf, x0, x1, y+frmline->y, frmline->height, node, 1);
                        }
                        else { // unmirrored left pad, or mirrored right pad
                            int x0 = x + frmline->x + word->x + word->width - word->o.height;
                            int x1 = x0 + word->o.baseline;
                            drawBorder(buf, x0, x1, y+frmline->y, frmline->height, node, 3);
                        }
                    }
                }
            }

            // process marks
#ifndef CR_USE_INVERT_FOR_SELECTION_MARKS
            if ( marks!=NULL && marks->length()>0 ) {
                // Here is drawn the "native highlighting" of a selection in progress
                // (We include frmline->width_overflow so any hanging punctuation overflow
                // over frmline->width is included in the drawing.)
                lvRect lineRect( frmline->x, frmline->y, frmline->x + frmline->width + frmline->width_overflow, frmline->y + frmline->height );
                for ( int i=0; i<marks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = marks->get(i);
                    // printf("marks #%d %d %d > %d %d\n", i, range->start.x, range->start.y, range->end.x, range->end.y);
                    if ( range->intersects( lineRect, mark ) ) {
                        //
                        buf->FillRect(mark.left + x, mark.top + y, mark.right + x, mark.bottom + y, m_pbuffer->highlight_options.selectionColor);
                    }
                }
            }
            if (bookmarks!=NULL && bookmarks->length()>0) {
                lvRect lineRect( frmline->x, frmline->y, frmline->x + frmline->width + frmline->width_overflow, frmline->y + frmline->height );
                for ( int i=0; i<bookmarks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = bookmarks->get(i);
                    if ( range->intersects( lineRect, mark ) ) {
                        //
                        DrawBookmarkTextUnderline(*buf, mark.left + x, mark.top + y, mark.right + x, mark.bottom + y, mark.bottom + y - 2, range->flags,
                                                  &m_pbuffer->highlight_options);
                    }
                }
            }
#endif
#ifdef CR_USE_INVERT_FOR_SELECTION_MARKS
            // process bookmarks
            if ( bookmarks != NULL && bookmarks->length() > 0 ) {
                lvRect lineRect( frmline->x, frmline->y, frmline->x + frmline->width + frmline->width_overflow, frmline->y + frmline->height );
                for ( int i=0; i<bookmarks->length(); i++ ) {
                    lvRect bookmark_rc;
                    ldomMarkedRange * range = bookmarks->get(i);
                    if ( range->intersects( lineRect, bookmark_rc ) ) {
                        buf->FillRect( bookmark_rc.left + x, bookmark_rc.top + y, bookmark_rc.right + x, bookmark_rc.bottom + y, 0xAAAAAA );
                    }
                }
            }
#endif

            int text_decoration_back_gap;
            lUInt16 lastWordSrcIndex;
            for (j=0; j<frmline->word_count; j++)
            {
                word = &frmline->words[j];
                srcline = &m_pbuffer->srctext[word->src_text_index];
                if ( (srcline->flags & LTEXT_HAS_EXTRA) && getLTextExtraProperty(srcline, LTEXT_EXTRA_CSS_HIDDEN) && !buf->WantsHiddenContent() )
                    continue;
                if (word->flags & LTEXT_WORD_IS_IMAGE)
                {
                    ldomNode * node = (ldomNode *) srcline->object;
                    if (node) {
                        LVImageSourceRef img = node->getObjectImageSource();
                        if ( img.isNull() )
                            img = LVCreateDummyImageSource( node, word->width, word->o.height );
                        int xx = x + frmline->x + word->x;
                        int yy = line_y + frmline->baseline - word->o.height + word->y;
                        buf->Draw( img, xx, yy, word->width, word->o.height );
                        //buf->FillRect( xx, yy, xx+word->width, yy+word->height, 1 );
                    }
                }
                else if (word->flags & LTEXT_WORD_IS_INLINE_BOX)
                {
                    ldomNode * node = (ldomNode *) srcline->object;
                    RenderRectAccessor node_fmt( node );
                    int node_x = node_fmt.getX();
                    int node_y = node_fmt.getY();
                    int x0, y0, doc_x_ib, doc_y_ib;
                    if (is_vertical) {
                        // With line_x = clip.right - x in Draw():
                        //   child's line_x = clip.right - x_child, where x_child = y0_inline.
                        // For ruby at column offset node_y from parent's right edge:
                        //   line_x_child = parent_line_x - node_y = (clip.right - x) - node_y
                        //   => clip.right - y0_inline = clip.right - x - node_y
                        //   => y0_inline = x + node_y
                        // x0_inline carries the vertical (Y-screen) position:
                        //   y_child = x0_inline = y + node_x (vertical offset + parent top)
                        //
                        // Clamp the inline box start to vert_min_next_x so it never
                        // starts before the preceding character's visual end.  However,
                        // ib_word_x (TTB-advance-based) is kept as a lower bound: if it
                        // exceeds vert_min_next_x (which can happen when the inline box
                        // has its own layout position further into the column), use it.
                        {
                            int ib_word_x = node_x - frmline->x;
                            int clamped_ib_x = ib_word_x < vert_min_next_x ? vert_min_next_x : ib_word_x;
                            int clamp_delta = clamped_ib_x - ib_word_x;  // ≥ 0
                            // P14 overlap diagnostic: save the OLD vert_min_next_x
                            // (= end of the preceding character) BEFORE updating it.
                            // After update, vert_min_next_x = end of THIS inline box.
                            int preceding_end = y + (int)frmline->x + vert_min_next_x;
                            x0 = y + node_x + clamp_delta;  // draw_x_rb = y + frmline->x + clamped_ib_x
                            // Use actual vertical depth (render_w from letter_spacing) so
                            // the next character starts after the ruby group's visual end,
                            // preventing the "文字が被る" overlap.  Fall back to o.width if
                            // letter_spacing was not set (non-ruby inline boxes, horizontal mode).
                            {
                                int ib_actual_depth = (srcline->letter_spacing > 0)
                                    ? (int)srcline->letter_spacing
                                    : (int)word->width;
                                vert_min_next_x = clamped_ib_x + ib_actual_depth;
                                vert_prev_plain_y0 = y + (int)frmline->x + clamped_ib_x;
                                vert_prev_effective_width = ib_actual_depth;
                            }
                            doc_x_ib = 0 - node_x;  // anchor to original node_x
                            // y0 must be x + node_y so the inner Draw places the ruby
                            // base column at the correct screen-X offset.
                            // Without this assignment, y0 is uninitialized (≈ 0 on
                            // the stack), causing all ruby groups to draw at column
                            // clip.right − annot_width regardless of their accumulated
                            // column advance node_y → displaced N columns right.
                            // y0 = x + node_y: places the inner Draw at the correct
                            // column offset so the ruby base column lands at
                            //   clip.right − node_y − annot_width.
                            // doc_y_ib = −node_y: cancels inline_box.getY()=node_y
                            // in DrawDocument so children see doc_y = 0 at this level.
                            y0 = x + node_y;
                            doc_y_ib = 0 - node_y;
                            // draw_x_inner = x0 + doc_x_ib + node_x = y + node_x + clamp_delta
                            // (DrawDocument accumulates doc_x += inline_box.getX() = node_x).
                            // If draw_x_inner < preceding_end, ruby overlaps the char above.
                            int draw_x_inner = x0 + doc_x_ib + node_x;
                            if ( draw_x_inner < preceding_end ) {
                                ltext_vert_bleed_count++;
                                int overlap_px = preceding_end - draw_x_inner;
                                if ( overlap_px > ltext_vert_bleed_max_px )
                                    ltext_vert_bleed_max_px = overlap_px;
                            }
                        }
                    } else {
                        x0 = x + node_x;
                        y0 = y + node_y;
                        doc_x_ib = 0 - node_x;
                        doc_y_ib = 0 - node_y;
                    }
                    int dx = m_pbuffer->width;
                    int dy = frmline->height; // can be > m_pbuffer->page_height
                            // A frmline can be bigger than page_height, if
                            // this inlineBox contains many long paragraphs
                    int page_height = m_pbuffer->page_height;
                    if ( absmarks_update_needed ) {
                        getAbsMarksFromMarks(marks, absmarks, node);
                        absmarks_update_needed = false;
                    }
                    if ( srcline->o.objflags & LTEXT_OBJECT_IS_EMBEDDED_BLOCK ) {
                        // With embedded blocks, we shouldn't drop the clip (as we do next
                        // for regular inline-block boxes)
                        DrawDocument( *buf, node, x0, y0, dx, dy, doc_x_ib, doc_y_ib, page_height, absmarks, bookmarks );
                    }
                    else {
                        lvRect curclip;
                        buf->GetClipRect( &curclip );
                        if ( draw_extra_info ) {
                            buf->SetClipRect( &draw_extra_info->content_overflow_clip );
                        }
                        DrawDocument( *buf, node, x0, y0, dx, dy, doc_x_ib, doc_y_ib, page_height, absmarks, bookmarks );
                        buf->SetClipRect(&curclip);
                    }
                }
                else if (word->flags & LTEXT_WORD_IS_PAD)
                {
                    // Background and border drawing has been handled above
                }
                else
                {
                    bool flgHyphen = false;
                    if ( word->flags&LTEXT_WORD_CAN_HYPH_BREAK_LINE_AFTER) {
                        if (j==frmline->word_count-1)
                            flgHyphen = true;
                        // Also do that even if it's not the last word in the line
                        // AND the line is bidi: the hyphen may be in the middle of
                        // the text, but it's fine for some people with bidi, see
                        // conversation "Bidi reordering of soft hyphen" at:
                        //   https://unicode.org/pipermail/unicode/2014-April/thread.html#348
                        // If that's not desirable, just disable hyphenation lookup
                        // in processParagraph() if m_has_bidi or if chars found in
                        // line span multilple bidi levels (so that we don't get
                        // a blank space for a hyphen not drawn after this word).
                        else if (frmline->flags & LTEXT_LINE_IS_BIDI)
                            flgHyphen = true;
                    }
                    font = (LVFont *) srcline->t.font;
                    str = srcline->t.text + word->t.start;
                    /*
                    lUInt32 srcFlags = srcline->flags;
                    if ( srcFlags & LTEXT_BACKGROUND_MARK_FLAGS ) {
                        lvRect rc;
                        rc.left = x + frmline->x + word->x;
                        rc.top = line_y + (frmline->baseline - font->getBaseline()) + word->y;
                        rc.right = rc.left + word->width;
                        rc.bottom = rc.top + font->getHeight();
                        buf->FillRect( rc.left, rc.top, rc.right, rc.bottom, 0xAAAAAA );
                    }
                    */
                    // Check if we need to continue the text decoration from previous word.
                    // For now, we only ensure it if this word and previous one are in the
                    // same text node. We wrongly won't when one of these is in a sub <SPAN>
                    // because we can't detect that rightly at this point anymore...
                    text_decoration_back_gap = 0;
                    if (j > 0 && word->src_text_index == lastWordSrcIndex) {
                        text_decoration_back_gap = word->x - lastWordEnd;
                    }
                    lUInt32 oldColor = buf->GetTextColor();
                    lUInt32 oldBgColor = buf->GetBackgroundColor();
                    lUInt32 cl = srcline->color;
                    lUInt32 bgcl = srcline->bgcolor;
                    if ( LTEXT_COLOR_IS_RESERVED(cl) ) {
                        if ( cl == LTEXT_COLOR_TRANSPARENT ) { // color: transparent
                            continue; // Don't draw this word
                        }
                        // Otherwise, LTEXT_COLOR_CURRENT: keep current buffer color
                    }
                    else {
                        buf->SetTextColor( cl );
                    }
                    if ( !LTEXT_COLOR_IS_RESERVED(bgcl) )
                        buf->SetBackgroundColor( bgcl );
                    // Add drawing flags: text decoration (underline...)
                    lUInt32 drawFlags = srcline->flags & LTEXT_TD_MASK;
                    // and chars direction, and if word begins or ends paragraph (for Harfbuzz)
                    drawFlags |= WORD_FLAGS_TO_FNT_FLAGS(word->flags);
                    // In vertical-rl/lr mode, signal to DrawTextString that vertical OpenType
                    // features (+vert/+vrt2) should be applied, substituting glyphs like ー→|.
                    // TCY words and non-CJK (Latin etc.) words are drawn horizontally.
                    // For non-CJK words, we render horizontally then rotate 90° CW as a block.
                    bool word_is_latin_in_vertical = is_vertical
                        && !(word->flags & LTEXT_WORD_IS_TCY)
                        && !(word->flags & LTEXT_WORD_IS_CJK)
                        && !(word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK)
                        && !(word->flags & LTEXT_WORD_IS_IMAGE)
                        && !(word->flags & LTEXT_WORD_IS_INLINE_BOX);
                    if (is_vertical && !(word->flags & LTEXT_WORD_IS_TCY) && !word_is_latin_in_vertical)
                        drawFlags |= LFNT_HINT_IS_VERTICAL;
                    if (word_is_latin_in_vertical)
                        drawFlags |= LFNT_HINT_RENDER_ROTATE_FOR_VERTICAL;
                    // For debugging, to visually see overlap/italic correction:
                    // if (word->flags & LTEXT_WORD__AVAILABLE_BIT_16__ ) drawFlags |= LTEXT_TD_OVERLINE;
                    int x0, y0, w, h;
                    bool vert_skip_draw = false;
                    if ( srcline->flags & LTEXT_MATH_TRANSFORM ) {
                        ldomNode * node = (ldomNode *) srcline->object;
                        // Parent of text node, which, having this flag, must be erm_final
                        // We want the glyph to be stretched to cover the erm_final rect
                        RenderRectAccessor node_fmt( node->getParentNode() );
                        x0 = x;
                        y0 = y;
                        w = node_fmt.getWidth();
                        h = node_fmt.getHeight();
                        drawFlags |= LFNT_HINT_TRANSFORM_STRETCH;
                    }
                    else {
                        if (is_vertical && (word->flags & LTEXT_WORD_IS_TCY)) {
                            // TCY (tate-chu-yoko): draw text horizontally within vertical column.
                            // The span occupies 1 em of column depth; text is centered in the column.
                            int em = font->getSize();
                            int clamped_x = (int)word->x < vert_min_next_x ? vert_min_next_x : (int)word->x;
                            // Horizontal: center the em-box within the column width
                            x0 = line_x - frmline->height + (frmline->height - em) / 2;
                            // Vertical: center font height in the 1-em slot
                            int y_slot_start = y + frmline->x + clamped_x;
                            y0 = y_slot_start + (em - font->getHeight()) / 2;
                            // Advance 1 em in the column direction
                            vert_min_next_x = clamped_x + em;
                            vert_prev_plain_y0 = y_slot_start;
                            if (y_slot_start + em > clip.bottom)
                                vert_skip_draw = true;
                        } else if (word_is_latin_in_vertical) {
                            // Non-CJK word (Latin etc.) in vertical column:
                            // render horizontally then rotate 90° CW as a single block.
                            // After rotation the block is font_height wide and _adv tall.
                            // vert_min_next_x and vert_prev_effective_width are updated below
                            // after DrawTextString using _adv (actual horizontal advance),
                            // so no intermediate TTB-advance assignment is needed here.
                            int font_h = font->getHeight();
                            // Center the rotated block (height=font_h) within the column width
                            x0 = line_x - frmline->height + (frmline->height - font_h) / 2;
                            // Vertical start of the block: vert_min_next_x is the column position
                            y0 = y + frmline->x + vert_min_next_x;
                            vert_prev_plain_y0 = y0;
                            // Skip draw if the word starts past the column bottom
                            if (y0 >= clip.bottom)
                                vert_skip_draw = true;
                        } else if (is_vertical) {
                            // For vertical-rl: line_x is the column's RIGHT edge.
                            // frmline->height = col_width = strut_height (the full column cell).
                            // Using line_x - frmline->height places the glyph's LEFT edge at
                            // the column's LEFT boundary, so full-width CJK glyphs fit exactly
                            // [line_x - col_width, line_x] with no overflow into adjacent columns.
                            // word->y encodes horizontal-mode baseline alignment (pushes small
                            // chars downward in horizontal text).  In vertical-rl this must NOT
                            // shift the glyph leftward inside the column.  Only inline-box words
                            // legitimately use word->y to select their sub-column position (e.g.
                            // ruby annotation vs base).  For all plain-text words, ignore it.
                            x0 = line_x - frmline->height;
                            // Centre plain-text chars on the column axis (JLReq 組版).
                            // Only apply when the column is NOT inflated by a ruby inline box.
                            // When frmline->height > strut, a ruby box has widened the column
                            // to accommodate the annotation sub-column.  In that case the body
                            // text sits at line_x - frmline->height (the left edge of the full
                            // column), which already aligns it with the base characters drawn
                            // by the inner ruby sub-formatter.  Adding (strut-em)/2 would push
                            // the glyph rightward into the annotation zone and mis-align it with
                            // the ruby base chars (verified via debug logging: 4px offset).
                            {
                                int em    = font->getSize();
                                int strut = m_pbuffer->strut_height;
                                if ( (int)frmline->height <= strut && em < strut )
                                    x0 += (strut - em) / 2;
                            }
                            // Use vert_min_next_x as the authoritative column position.
                            // word->x is derived from cumulative TTB y_advances; for CJK-only
                            // text word->x == vert_min_next_x (both track em_size advances).
                            // When Latin words precede this char, their TTB advance (full em)
                            // inflates word->x past the actual visual position.  Using
                            // vert_min_next_x (updated from _adv after each Latin draw) ensures
                            // CJK chars start immediately after the Latin word's visual end.
                            // Punctuation retrograde (large uint16 word->x) is handled naturally
                            // because vert_min_next_x is always >= the previous slot end.
                            int clamped_x = vert_min_next_x;
                            y0 = y + frmline->x + clamped_x;
                            // Advance vert_min_next_x by at least font_size.
                            // After the TTB-reversal fix, word->width is the accurate
                            // HarfBuzz TTB y_advance.  For most CJK chars this equals
                            // font_size.  For compressed punctuation (。、) the HarfBuzz
                            // advance is small but the full-size glyph would overlap the
                            // next char if we used the raw advance — enforce font_size.
                            int font_size = font->getSize();
                            int effective_width = ((int)word->width > font_size) ? (int)word->width : font_size;
                            vert_min_next_x = clamped_x + effective_width;
                            // Cap vert_min_next_x at the column height so that compressed
                            // punctuation (。、 TTB < font_size) cannot push the next character
                            // past clip.bottom and cause it to be silently dropped.
                            if (vert_min_next_x > clip.bottom - y)
                                vert_min_next_x = clip.bottom - y;
                            // Detect character overlap in the height (column) direction.
                            // If this character's slot start (y0) is before the previous
                            // character's slot end (vert_prev_plain_y0 + vert_prev_effective_width),
                            // two characters overlap — the "文字が被る" bug.
                            if (vert_prev_plain_y0 >= 0 && !vert_skip_draw && y0 < clip.bottom) {
                                int slot_end_prev = vert_prev_plain_y0 + vert_prev_effective_width;
                                int overlap_px = slot_end_prev - y0;
                                if (overlap_px > 0) {
                                    ltext_vert_char_overlap_count++;
                                    if (overlap_px > ltext_vert_char_overlap_max_px)
                                        ltext_vert_char_overlap_max_px = overlap_px;
                                }
                            }
                            // Track this plain-text character's y0 and effective_width for the
                            // next character's overlap check and the inline-box bleed check.
                            vert_prev_plain_y0 = y0;
                            vert_prev_effective_width = effective_width;
                            // Skip only when the character SLOT START (y0) is at or past
                            // clip.bottom — the glyph bitmap is then completely outside the
                            // visible column and shouldn't be drawn.
                            // Do NOT check y0 + descent: the descent can legitimately extend
                            // a few pixels past clip.bottom for the last character of a column
                            // (especially after the TTB m_advance fix which may place one more
                            // character in the column).  buf->Draw() clips to clip.bottom anyway.
                            if (y0 >= clip.bottom)
                                vert_skip_draw = true;
                        } else {
                            x0 = x + frmline->x + word->x;
                            y0 = line_y + (frmline->baseline - font->getBaseline()) + word->y;
                        }
                        w = h = 0; // unused
                        if ( word->flags & LTEXT_WORD_IS_CJK && m_pbuffer->cjk_width_scale_percent != 100 ) {
                            // We want the glyph drawn in the middle of the scaled width: delegate this
                            // to font->DrawTextString() (this simplifies a lot cjk width handling, as we
                            // don't need to consider it as spacing added on the right of glyph, and the
                            // need to not do it for the last glyph on the line; also, we want to have any
                            // underilne done on the scaled width, which font->DrawTextString() will do well).
                            drawFlags |= LFNT_HINT_CJK_SCALED_WIDTH;
                            w = m_pbuffer->cjk_width_scale_percent;
                            // We pass cjk_width_scale_percent via the otherwise unused 'target_w' argument.
                            // This would be not needed for non-flexible CJK chars: we don't know anymore
                            // the original glyph width here, but font->DrawTextString() will get it from
                            // the glyph and, from the (fixed) word->width we provide, could know it has
                            // to shift x by half the differences.
                            // But for flexible CJK chars, word->width may have been tweaked.
                            // So, by passing cjk_width_scale_percent, font->DrawTextString() can
                            // recompute the original scaled width, and get a correct x shift.
                        }
                        if ( word->flags & LTEXT_WORD_IS_FLEXIBLE_WIDTH_CJK ) {
                            drawFlags |= LFNT_HINT_CJK_ALTERED_WIDTH;
                            /*
                            // For debugging, showing in color what's been done with CJK flexible chars:
                            if (word->width == word->min_width) { // fully reduced: red
                                cl = 0x00FF0000; buf->SetTextColor( cl );
                            }
                            else if ( word->min_width == 0 ) { // reduced, but not fully: blue
                                cl = 0x000000FF; buf->SetTextColor( cl );
                            }
                            else { // allowed to be reduced, but not done as not needed: dark purple
                                cl = 0x00A000A0; buf->SetTextColor( cl );
                            }
                            */
                        }
                    }
                    {
                        int _adv = !vert_skip_draw ? font->DrawTextString(
                            buf,
                            x0,
                            y0,
                            str,
                            word->t.len,
                            '?',
                            NULL,
                            flgHyphen,
                            srcline->lang_cfg,
                            drawFlags,
                            srcline->letter_spacing + word->added_letter_spacing,
                            word->width,
                            text_decoration_back_gap,
                            w, h) : 0;
                        // For word_is_latin_in_vertical, DrawTextString returns the
                        // actual horizontal (x_advance) width, which may be smaller
                        // than word->width (TTB y_advance) for fonts with full-em vmtx.
                        // Update vert_min_next_x so the next char follows directly
                        // without a blank gap.
                        if (word_is_latin_in_vertical && _adv > 0) {
                            // vert_min_next_x was left at the word's start; add _adv to advance
                            // to the word's visual end (actual horizontal advance, not TTB).
                            vert_min_next_x += _adv;
                            if (vert_min_next_x > clip.bottom - y)
                                vert_min_next_x = clip.bottom - y;
                            vert_prev_effective_width = _adv;
                        }
                    }
                    // Draw 圏点/傍点 (text-emphasis marks) in vertical mode
                    if ( is_vertical && !vert_skip_draw && (srcline->flags & LTEXT_HAS_EXTRA) ) {
                        int em_style = getLTextExtraProperty(srcline, LTEXT_EXTRA_CSS_TEXT_EMPHASIS);
                        if ( em_style > 0 ) {
                            // Map style enum to Unicode mark character
                            lChar32 mark;
                            switch (em_style) {
                                case css_tes_open_dot:    mark = 0x25CB; break; // ○
                                case css_tes_open_circle: mark = 0x25CB; break; // ○
                                case css_tes_filled_sesame: mark = 0xFE45; break; // ﹅
                                case css_tes_open_sesame:   mark = 0xFE46; break; // ﹆
                                case css_tes_filled_dc:     mark = 0x25C9; break; // ◉
                                case css_tes_open_dc:       mark = 0x25CE; break; // ◎
                                case css_tes_filled_tri:    mark = 0x25B2; break; // ▲
                                case css_tes_open_tri:      mark = 0x25B3; break; // △
                                default:                    mark = 0x25CF; break; // ● filled dot/circle
                            }
                            int em = font->getSize();
                            // In vertical-rl, draw marks to the right of each char (= "before" dir)
                            // x_mark = line_x (right edge of column) — in inter-column space
                            int x_mark = line_x;
                            // Each char occupies ~1 em of column depth
                            int char_count = word->t.len;
                            int clamped_x = word->x;
                            if (clamped_x < vert_min_next_x - em * char_count)
                                clamped_x = vert_min_next_x - em * char_count;
                            for (int mc = 0; mc < char_count; mc++) {
                                int y_mark = y + frmline->x + clamped_x + mc * em;
                                if (y_mark >= clip.top && y_mark + em <= clip.bottom) {
                                    font->DrawTextString(buf, x_mark, y_mark, &mark, 1, '?',
                                        NULL, false, NULL, 0, 0, em, 0, 0, 0);
                                }
                            }
                        }
                    }
                    /* To display the added letter spacing % at end of line
                    if (j == frmline->word_count-1 && word->added_letter_spacing ) {
                        // lString32 val = lString32::itoa(word->added_letter_spacing);
                        lString32 val = lString32::itoa(100*word->added_letter_spacing / font->getSize());
                        font->DrawTextString( buf, x + frmline->x + word->x + word->width + 10,
                            line_y + (frmline->baseline - font->getBaseline()) + word->y,
                            val.c_str(), val.length(), '?', NULL, false);
                    }
                    */
                    if ( !LTEXT_COLOR_IS_RESERVED(cl) )
                        buf->SetTextColor( oldColor );
                    if ( !LTEXT_COLOR_IS_RESERVED(bgcl) )
                        buf->SetBackgroundColor( oldBgColor );
                }
                lastWordSrcIndex = word->src_text_index;
                lastWordEnd = word->x + word->width;
            }

#ifdef CR_USE_INVERT_FOR_SELECTION_MARKS
            // process marks
            if ( marks!=NULL && marks->length()>0 ) {
                lvRect lineRect( frmline->x, frmline->y, frmline->x + frmline->width + frmline->width_overflow, frmline->y + frmline->height );
                for ( int i=0; i<marks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = marks->get(i);
                    if ( range->intersects( lineRect, mark ) ) {
                        buf->InvertRect( mark.left + x, mark.top + y, mark.right + x, mark.bottom + y);
                    }
                }
            }
#endif
            if ( restore_orig_clip ) {
                buf->SetClipRect(&origClip);
            }
        }
        if (is_vertical) {
            if (frmline->height == 0) break; // safety: prevent infinite loop
            line_x -= frmline->height; // columns progress right-to-left
        } else {
            line_y += frmline->height;
        }
    }

    // Draw floats if any
    for (i=0; i<m_pbuffer->floatcount; i++) {
        embedded_float_t * flt = m_pbuffer->floats[i];
        if (flt->srctext == NULL) {
            // Ignore outer floats (they are either fake footprint floats,
            // or real outer floats not to be drawn by us)
            continue;
        }
        ldomNode * node = (ldomNode *) flt->srctext->object;

        // Only some part of this float needs to be in the clip area.
        // Also account for the overflows, so we can render fully
        // floats with negative margins.
        RenderRectAccessor node_fmt( node );
        int top_overflow = node_fmt.getTopOverflow();
        int bottom_overflow = node_fmt.getBottomOverflow();

        if (y + flt->y - top_overflow < clip.bottom && y + flt->y + (int)flt->height + bottom_overflow > clip.top) {
            // DrawDocument() parameters (y0 + doc_y must be equal to our y,
            // doc_y just shift the viewport, so anything outside is not drawn).
            int x0 = x + flt->x;
            int y0 = y + flt->y;
            int doc_x = 0 - flt->x;
            int doc_y = 0 - flt->y;
            int dx = m_pbuffer->width;
            int dy = m_pbuffer->page_height;
            int page_height = m_pbuffer->page_height;
            if ( absmarks_update_needed ) {
                getAbsMarksFromMarks(marks, absmarks, node);
                absmarks_update_needed = false;
            }
            DrawDocument( *buf, node, x0, y0, dx, dy, doc_x, doc_y, page_height, absmarks, bookmarks );
        }
    }
    delete absmarks;
}


