// =============================================================================
// Fork-only declarations used by the split formatter files.
//
// lvtextfm.cpp and lvtextfm_vert.cpp form one translation unit
// (the latter is #include'd at the bottom of lvtextfm.cpp).  This
// header concentrates fork-only forward declarations and small
// ruby-detection helpers in one place so lvtextfm.cpp stays closer
// to upstream.  It must be included after lvtinydom.h / fb2def.h /
// cssdef.h are visible (i.e. include it from lvtextfm.cpp only).
//
// Created during Phase C Step 1 (soft-fork hygiene) — see CLAUDE.md.
// =============================================================================

#ifndef LVTEXTFM_FORK_H_INCLUDED
#define LVTEXTFM_FORK_H_INCLUDED

class LVFormatter;

// Horizontal layout free functions (defined in lvtextfm.cpp,
// called from LVFormatter::splitParagraphs).
void processParagraphHorizontal( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockHorizontal( LVFormatter* fmt, int idx );

// Vertical layout free functions (defined in lvtextfm_vert.cpp).
void processParagraphVertical( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx );

// Punctuation helpers (defined in lvtextfm.cpp; used from
// measureText() in lvtextfm.cpp, which is earlier in the TU).
bool isLeftPunctuation( lChar32 c );
#if (USE_LIBUNIBREAK!=1)
bool isCJKPunctuation( lChar32 c );
bool isCJKLeftPunctuation( lChar32 c );
#endif

// Vertical-mode diagnostic globals.  Defined in lvtextfm_vert.cpp;
// incremented from various sites in lvtextfm.cpp.  Reset/getter
// functions are exposed to cre.cpp.
extern int ltext_vert_ruby_adv_diff_total;
extern int ltext_vert_ruby_adv_diff_max;
extern int ltext_vert_bleed_count;
extern int ltext_vert_bleed_max_px;
extern int ltext_vert_fmt_draws;
extern int ltext_fmt_calls;
extern int ltext_fmt_vert_calls;
extern int ltext_word_iters;
extern int ltext_vert_ib_layout_gap_total;
extern int ltext_vert_ib_layout_gap_max;
extern int ltext_vert_char_overlap_count;
extern int ltext_vert_char_overlap_max_px;

// True if node is a vertical-ruby inline box: the boxing algorithm wraps
// the ruby table in an el_inlineBox whose parent has display:ruby.
static inline bool isRubyInlineBox(ldomNode * node) {
    return node
        && node->getParentNode()
        && node->getParentNode()->getStyle()->display == css_d_ruby
        && node->getChildCount() > 0
        && node->getChildNode(0)->getRendMethod() == erm_table;
}

// True if a ruby element ID belongs to the annotation side (rt, rp, rtc).
static inline bool isRubyAnnotId(lUInt16 id) {
    return id == el_rt || id == el_rp || id == el_rtc;
}

#endif // LVTEXTFM_FORK_H_INCLUDED
