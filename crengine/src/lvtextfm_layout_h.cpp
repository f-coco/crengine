// =============================================================================
// LFormattedText::Draw — vertical-rl aware draw entry point.
//
// Fork origin: this file was originally split out of upstream
// crengine/src/lvtextfm.cpp in commit f8b0bbe1.  After Phase C Step 2b the
// horizontal layout free functions were folded back into lvtextfm.cpp; this
// file now contains only LFormattedText::Draw, which carries the bulk of
// the vertical-rl rendering logic (Y=X swap, line_x decrement per column,
// ruby column inflation, per-slot offset hooks).  Step 2d will fold this
// back into lvtextfm.cpp as well, after which the file is removed.
// =============================================================================

// -----------------------------------------------------------------------------
// LFormattedText::Draw
// Origin: upstream lvtextfm.cpp `void LFormattedText::Draw(...)` (~line 5923
// pre-f8b0bbe1).  Already a non-member, so no signature rename.  Heavily
// modified for vertical-rl (Y=X swap, line_x decrementing per column, ruby
// inflation handling, per-slot offset recording hooks).  Vertical-specific
// branches are guarded by `is_vertical`.
// -----------------------------------------------------------------------------
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
    ltext_fmt_calls++;
    if (is_vertical) ltext_fmt_vert_calls++;
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
    // For vertical-rl: use the stored column anchor rather than the buffer's current
    // clip.right. content_overflow_clip.right may have been widened to allow ruby
    // annotations to draw into the right margin; that must not shift the column anchor.
    draw_extra_info_t * draw_extra_info = (draw_extra_info_t*)buf->GetDrawExtraInfo();
    int vert_anchor = (is_vertical && draw_extra_info && draw_extra_info->vert_column_clip_right)
        ? draw_extra_info->vert_column_clip_right : clip.right;
    int line_x = is_vertical ? (vert_anchor - x) : x;

    // Build the lineRect used by ldomMarkedRange::intersects() for marks/bookmarks.
    // In vertical-rl, frmline->width is set to strut_height (column WIDTH on screen),
    // not the inline content extent, so we must use m_pbuffer->width (inner_width of
    // the final block = column inline extent) as the upper bound for mark.start.x.
    auto makeMarkLineRect = [&](const formatted_line_t * fl) -> lvRect {
        int line_right = is_vertical
            ? (fl->x + (int)m_pbuffer->width)
            : (fl->x + fl->width + fl->width_overflow);
        return lvRect(fl->x, fl->y, line_right, fl->y + fl->height);
    };

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

    // Vertical-rl bleed detection.
    // ltext_vert_bleed_count fires when a ruby inline-box's screen-Y start
    // (draw_x_inner) is less than the preceding character's slot end, meaning
    // the ruby group overlaps the character above it.
    // Counters accessible via doc._document:resetVertBleedCounters() /
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
            ? ((line_x > clip.left && line_x - (int)frmline->height < clip.right) || ignore_clip)
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
                lvRect lineRect = makeMarkLineRect(frmline);
                for ( int i=0; i<marks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = marks->get(i);
                    // printf("marks #%d %d %d > %d %d\n", i, range->start.x, range->start.y, range->end.x, range->end.y);
                    if ( range->intersects( lineRect, mark ) ) {
                        // Vertical-rl: formatter-x→screen-Y (+y), formatter-y→screen-X (line_x−).
                        // For ruby-inflated columns (height > strut), the annotation zone occupies
                        // the rightmost (height - strut) pixels; draw only over the base text zone.
                        if (is_vertical) {
                            int ann_w = (int)frmline->height > m_pbuffer->strut_height
                                        ? (int)frmline->height - m_pbuffer->strut_height : 0;
                            // Apply the per-slot Y offset that docToWindowPoint applies to
                            // sboxes, so the highlight aligns with the actual rendered glyphs.
                            // Use mark.left as the slot_y key (same as lfnt_vert_set_current_slot_y_key
                            // sets when drawing the first char of the column).
                            int slot_fallback = (draw_extra_info ? draw_extra_info->vert_glyph_y_offset : 0);
                            int slot_off = lfnt_lookup_vert_slot_offset(line_x, mark.left + y, slot_fallback);
                            buf->FillRect(line_x - (int)m_pbuffer->strut_height - ann_w, mark.left + y + slot_off,
                                          line_x - ann_w, mark.right + y + slot_off, m_pbuffer->highlight_options.selectionColor);
                        } else {
                            buf->FillRect(mark.left + x, mark.top + y, mark.right + x, mark.bottom + y, m_pbuffer->highlight_options.selectionColor);
                        }
                    }
                }
            }
            if (bookmarks!=NULL && bookmarks->length()>0) {
                lvRect lineRect = makeMarkLineRect(frmline);
                for ( int i=0; i<bookmarks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = bookmarks->get(i);
                    if ( range->intersects( lineRect, mark ) ) {
                        // Vertical-rl: same axis-swap and ruby-inflation adjustment as marks above.
                        if (is_vertical) {
                            int ann_w = (int)frmline->height > m_pbuffer->strut_height
                                        ? (int)frmline->height - m_pbuffer->strut_height : 0;
                            DrawBookmarkTextUnderline(*buf, line_x - (int)m_pbuffer->strut_height - ann_w, mark.left + y,
                                                      line_x - ann_w, mark.right + y, mark.right + y - 2, range->flags,
                                                      &m_pbuffer->highlight_options);
                        } else {
                            DrawBookmarkTextUnderline(*buf, mark.left + x, mark.top + y, mark.right + x, mark.bottom + y, mark.bottom + y - 2, range->flags,
                                                      &m_pbuffer->highlight_options);
                        }
                    }
                }
            }
#endif
#ifdef CR_USE_INVERT_FOR_SELECTION_MARKS
            // process bookmarks
            if ( bookmarks != NULL && bookmarks->length() > 0 ) {
                lvRect lineRect = makeMarkLineRect(frmline);
                for ( int i=0; i<bookmarks->length(); i++ ) {
                    lvRect bookmark_rc;
                    ldomMarkedRange * range = bookmarks->get(i);
                    if ( range->intersects( lineRect, bookmark_rc ) ) {
                        // Vertical-rl: same axis-swap and ruby-inflation adjustment as marks above.
                        if (is_vertical) {
                            int ann_w = (int)frmline->height > m_pbuffer->strut_height
                                        ? (int)frmline->height - m_pbuffer->strut_height : 0;
                            buf->FillRect( line_x - (int)m_pbuffer->strut_height - ann_w, bookmark_rc.left + y, line_x - ann_w, bookmark_rc.right + y, 0xAAAAAA );
                        } else {
                            buf->FillRect( bookmark_rc.left + x, bookmark_rc.top + y, bookmark_rc.right + x, bookmark_rc.bottom + y, 0xAAAAAA );
                        }
                    }
                }
            }
#endif

            int text_decoration_back_gap;
            lUInt16 lastWordSrcIndex;
            for (j=0; j<frmline->word_count; j++)
            {
                ltext_word_iters++;
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
                        if (is_vertical) {
                            // In vertical-rl mode after the x/y swap at Draw() entry:
                            //   x   = column advance (doc_y, ≈0 for first column)
                            //   y   = screen-Y origin of the block
                            //   line_x = clip.right - x = right edge of current column group
                            // word->width   = image physical width  = block-direction (screen-X) extent
                            // word->o.height = image physical height = inline-direction (screen-Y) extent
                            // Place the right edge of the image at line_x; clamp left to 0.
                            int x0 = line_x - (int)word->width;
                            if ( x0 < 0 ) x0 = 0;
                            int y0 = y + (int)frmline->x + (int)word->x;
                            buf->Draw( img, x0, y0, word->width, word->o.height );
                        } else {
                            int xx = x + frmline->x + word->x;
                            int yy = line_y + frmline->baseline - word->o.height + word->y;
                            buf->Draw( img, xx, yy, word->width, word->o.height );
                        }
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
                            // Diagnostic: ib_word_x > vert_min_next_x means the layout
                            // placed this box further than the draw tracker expected — a gap
                            // above the box.  Should be 0 when vert_layout_min_x correctly
                            // mirrors vert_min_next_x (e.g. spaces use actual advance, not
                            // font_size, so no inflation).
                            if (ib_word_x > vert_min_next_x) {
                                int gap = ib_word_x - vert_min_next_x;
                                ltext_vert_ib_layout_gap_total += gap;
                                if (gap > ltext_vert_ib_layout_gap_max)
                                    ltext_vert_ib_layout_gap_max = gap;
                            }
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
                        // For vertical-rl, hand the column anchor (line_x) AND the slot_y
                        // key (clip.top + frmline->x + word->x in screen coords) to the font
                        // layer.  Using word->x — the position layoutstores and getRectEx
                        // returns — instead of clamped_x (the running tracker possibly
                        // bumped by vert_min_next_x overlap correction) lets docToWindowPoint
                        // produce matching keys at sbox time.
                        if (is_vertical) {
                            lfnt_vert_set_current_anchor(line_x);
                            // y here is screen-Y origin (= clip.top for outer block; the inner
                            // block's accumulated draw_y for nested formatters such as ruby cells)
                            lfnt_vert_set_current_slot_y_key(y + (int)frmline->x + (int)word->x);
                            ltext_vert_fmt_draws++;
                        }
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
                        if (is_vertical) {
                            lfnt_vert_set_current_anchor(-1);
                            lfnt_vert_set_current_slot_y_key(-1);
                        }
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
                lvRect lineRect = makeMarkLineRect(frmline);
                for ( int i=0; i<marks->length(); i++ ) {
                    lvRect mark;
                    ldomMarkedRange * range = marks->get(i);
                    if ( range->intersects( lineRect, mark ) ) {
                        // Vertical-rl: same axis-swap and ruby-inflation adjustment as marks above.
                        if (is_vertical) {
                            int ann_w = (int)frmline->height > m_pbuffer->strut_height
                                        ? (int)frmline->height - m_pbuffer->strut_height : 0;
                            buf->InvertRect( line_x - (int)m_pbuffer->strut_height - ann_w, mark.left + y, line_x - ann_w, mark.right + y);
                        } else {
                            buf->InvertRect( mark.left + x, mark.top + y, mark.right + x, mark.bottom + y);
                        }
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

    // Oversized inline boxes (currently initial-letter) are normally drawn when
    // their owner first line is drawn, which preserves the usual text paint
    // ordering. When that first line is fully above the viewport, redraw these
    // here so the sunk part still appears lower down.
    // (oversized_inlineboxes have been made generic and could contain anything
    // but we know that currently it can only be an initial-letter on the first line;
    // as an optimization, we just ensure that looking if the first line was not drawn.)
    bool first_line_drawn = ignore_clip;
    if ( !first_line_drawn && m_pbuffer->frmlinecount > 0 ) {
        first_line_drawn = y + m_pbuffer->frmlines[0]->height > clip.top;
    }
    if ( !first_line_drawn ) {
        for (i=0; i<m_pbuffer->oversized_inlinebox_count; i++) {
            int src_index = m_pbuffer->oversized_inlineboxes[i];
            srcline = &m_pbuffer->srctext[src_index];
            ldomNode * node = (ldomNode *)srcline->object;
            RenderRectAccessor fmt( node );
            int top_overflow = fmt.getTopOverflow();
            int bottom_overflow = fmt.getBottomOverflow();
            if ( y + fmt.getY() - top_overflow >= clip.bottom
                    || y + fmt.getY() + fmt.getHeight() + bottom_overflow <= clip.top ) {
                continue;
            }
            int x0 = x + fmt.getX();
            int y0 = y + fmt.getY();
            int doc_x = 0 - fmt.getX();
            int doc_y = 0 - fmt.getY();
            int dx = m_pbuffer->width;
            int dy = m_pbuffer->page_height;
            int page_height = m_pbuffer->page_height;
            if ( absmarks_update_needed ) {
                getAbsMarksFromMarks(marks, absmarks, node);
                absmarks_update_needed = false;
            }
            lvRect curclip;
            buf->GetClipRect( &curclip );
            if ( draw_extra_info ) {
                buf->SetClipRect( &draw_extra_info->content_overflow_clip );
            }
            DrawDocument( *buf, node, x0, y0, dx, dy, doc_x, doc_y, page_height, absmarks, bookmarks );
            buf->SetClipRect(&curclip);
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


