/*******************************************************

   CoolReader Engine C-compatible API

   lvtextfm.cpp:  Text formatter

   (c) Vadim Lopatin, 2000-2011
   This source code is distributed under the terms of
   GNU General Public License
   See LICENSE file for details

*******************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../include/crsetup.h"
#include "../include/cssdef.h"
#include "../include/lvfnt.h"
#include "../include/lvtextfm.h"
#include "../include/lvdrawbuf.h"
#include "../include/fb2def.h"

#ifdef __cplusplus
#include "../include/lvimg.h"
#include "../include/lvtinydom.h"
#include "../include/lvrend.h"
#include "../include/textlang.h"
#endif

#if USE_HARFBUZZ==1
#include <hb.h>
#endif

#if (USE_FRIBIDI==1)
#include <fribidi.h>
#endif

#define SPACE_WIDTH_SCALE_PERCENT 100
#define MIN_SPACE_CONDENSING_PERCENT 50
#define UNUSED_SPACE_THRESHOLD_PERCENT 5
#define MAX_ADDED_LETTER_SPACING_PERCENT 0
#define CJK_WIDTH_SCALE_PERCENT 100


// to debug formatter

#if defined(_DEBUG) && 0
#define TRACE_LINE_SPLITTING 1
#else
#define TRACE_LINE_SPLITTING 0
#endif

#if TRACE_LINE_SPLITTING==1
#ifdef _MSC_VER
#define TR(...) CRLog::trace(__VA_ARGS__)
#else
#define TR(x...) CRLog::trace(x)
#endif
#else
#ifdef _MSC_VER
#define TR(...)
#else
#define TR(x...)
#endif
#endif

#define FRM_ALLOC_SIZE 16
#define FLT_ALLOC_SIZE 4

formatted_line_t * lvtextAllocFormattedLine( )
{
    formatted_line_t * pline = (formatted_line_t *)calloc(1, sizeof(*pline));
    return pline;
}

formatted_line_t * lvtextAllocFormattedLineCopy( formatted_word_t * words, int word_count )
{
    formatted_line_t * pline = (formatted_line_t *)calloc(1, sizeof(*pline));
    lUInt32 size = (word_count + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    pline->words = (formatted_word_t*)malloc( sizeof(formatted_word_t)*(size) );
    memcpy( pline->words, words, word_count * sizeof(formatted_word_t) );
    return pline;
}

void lvtextFreeFormattedLine( formatted_line_t * pline )
{
    if (pline->words)
        free( pline->words );
    free(pline);
}

formatted_word_t * lvtextAddFormattedWord( formatted_line_t * pline )
{
    int size = (pline->word_count + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    if ( pline->word_count >= size)
    {
        size += FRM_ALLOC_SIZE;
        pline->words = cr_realloc( pline->words, size );
    }
    return &pline->words[ pline->word_count++ ];
}

formatted_line_t * lvtextAddFormattedLine( formatted_text_fragment_t * pbuffer )
{
    int size = (pbuffer->frmlinecount + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    if (pbuffer->frmlinecount >= size)
    {
        size += FRM_ALLOC_SIZE;
        pbuffer->frmlines = cr_realloc( pbuffer->frmlines, size );
    }
    return (pbuffer->frmlines[ pbuffer->frmlinecount++ ] = lvtextAllocFormattedLine());
}

formatted_line_t * lvtextAddFormattedLineCopy( formatted_text_fragment_t * pbuffer, formatted_word_t * words, int words_count )
{
    int size = (pbuffer->frmlinecount + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    if ( pbuffer->frmlinecount >= size)
    {
        size += FRM_ALLOC_SIZE;
        pbuffer->frmlines = cr_realloc( pbuffer->frmlines, size );
    }
    return (pbuffer->frmlines[ pbuffer->frmlinecount++ ] = lvtextAllocFormattedLineCopy(words, words_count));
}

embedded_float_t * lvtextAllocEmbeddedFloat( )
{
    embedded_float_t * flt = (embedded_float_t *)calloc(1, sizeof(*flt));
    return flt;
}

embedded_float_t * lvtextAddEmbeddedFloat( formatted_text_fragment_t * pbuffer )
{
    int size = (pbuffer->floatcount + FLT_ALLOC_SIZE-1) / FLT_ALLOC_SIZE * FLT_ALLOC_SIZE;
    if (pbuffer->floatcount >= size)
    {
        size += FLT_ALLOC_SIZE;
        pbuffer->floats = cr_realloc( pbuffer->floats, size );
    }
    return (pbuffer->floats[ pbuffer->floatcount++ ] = lvtextAllocEmbeddedFloat());
}


formatted_text_fragment_t * lvtextAllocFormatter( lUInt16 width )
{
    formatted_text_fragment_t * pbuffer = (formatted_text_fragment_t*)calloc(1, sizeof(*pbuffer));
    pbuffer->width = width;
    pbuffer->strut_height = 0;
    pbuffer->strut_baseline = 0;
    pbuffer->is_reusable = true;
    pbuffer->light_formatting = false;
    int defMode = MAX_IMAGE_SCALE_MUL > 1 ? (ARBITRARY_IMAGE_SCALE_ENABLED==1 ? 2 : 1) : 0;
    int defMult = MAX_IMAGE_SCALE_MUL;
    // mode: 0=disabled, 1=integer scaling factors, 2=free scaling
    // scale: 0=auto based on font size, 1=no zoom, 2=scale up to *2, 3=scale up to *3
    pbuffer->img_zoom_in_mode_block = defMode; /**< can zoom in block images: 0=disabled, 1=integer scale, 2=free scale */
    pbuffer->img_zoom_in_scale_block = defMult; /**< max scale for block images zoom in: 1, 2, 3 */
    pbuffer->img_zoom_in_mode_inline = defMode; /**< can zoom in inline images: 0=disabled, 1=integer scale, 2=free scale */
    pbuffer->img_zoom_in_scale_inline = defMult; /**< max scale for inline images zoom in: 1, 2, 3 */
    pbuffer->img_zoom_out_mode_block = defMode; /**< can zoom out block images: 0=disabled, 1=integer scale, 2=free scale */
    pbuffer->img_zoom_out_scale_block = defMult; /**< max scale for block images zoom out: 1, 2, 3 */
    pbuffer->img_zoom_out_mode_inline = defMode; /**< can zoom out inline images: 0=disabled, 1=integer scale, 2=free scale */
    pbuffer->img_zoom_out_scale_inline = defMult; /**< max scale for inline images zoom out: 1, 2, 3 */
    pbuffer->space_width_scale_percent = SPACE_WIDTH_SCALE_PERCENT; // 100% (keep original width)
    pbuffer->min_space_condensing_percent = MIN_SPACE_CONDENSING_PERCENT; // 50%
    pbuffer->unused_space_threshold_percent = UNUSED_SPACE_THRESHOLD_PERCENT; // 5%
    pbuffer->max_added_letter_spacing_percent = MAX_ADDED_LETTER_SPACING_PERCENT; // 0%
    pbuffer->cjk_width_scale_percent = CJK_WIDTH_SCALE_PERCENT; // 100% (keep original width)

    return pbuffer;
}

void lvtextFreeFormatter( formatted_text_fragment_t * pbuffer )
{
    if (pbuffer->srctext)
    {
        for (int i=0; i<pbuffer->srctextlen; i++)
        {
            if (pbuffer->srctext[i].flags & LTEXT_FLAG_OWNTEXT)
                free( (void*)pbuffer->srctext[i].t.text );
        }
        free( pbuffer->srctext );
    }
    if (pbuffer->frmlines)
    {
        for (int i=0; i<pbuffer->frmlinecount; i++)
        {
            lvtextFreeFormattedLine( pbuffer->frmlines[i] );
        }
        free( pbuffer->frmlines );
    }
    if (pbuffer->floats)
    {
        for (int i=0; i<pbuffer->floatcount; i++)
        {
            if (pbuffer->floats[i]->links) {
                delete pbuffer->floats[i]->links;
            }
            free(pbuffer->floats[i]);
        }
        free( pbuffer->floats );
    }
    if (pbuffer->inlineboxes_links)
    {
        LVHashTable<lUInt32, lString32Collection*>::iterator it = pbuffer->inlineboxes_links->forwardIterator();
        LVHashTable<lUInt32, lString32Collection*>::pair* pair;
        while ( (pair = it.next()) ) {
            delete pair->value;
        }
        delete pbuffer->inlineboxes_links;
    }
    free(pbuffer);
}


void lvtextAddSourceLine( formatted_text_fragment_t * pbuffer,
   lvfont_handle   font,     /* handle of font to draw string */
   TextLangCfg *   lang_cfg,
   const lChar32 * text,     /* pointer to unicode text string */
   lUInt32         len,      /* number of chars in text, 0 for auto(strlen) */
   lUInt32         color,    /* color */
   lUInt32         bgcolor,  /* bgcolor */
   lUInt32         flags,    /* flags */
   lInt16          interval, /* line height in screen pixels */
   lInt16          valign_dy, /* drift y from baseline */
   lInt16          indent,    /* first line indent (or all but first, when negative) */
   void *          object,    /* pointer to custom object */
   lUInt16         offset,
   lInt16          letter_spacing
                         )
{
    int srctextsize = (pbuffer->srctextlen + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    if ( pbuffer->srctextlen >= srctextsize)
    {
        srctextsize += FRM_ALLOC_SIZE;
        pbuffer->srctext = cr_realloc( pbuffer->srctext, srctextsize );
    }
    src_text_fragment_t * pline = &pbuffer->srctext[ pbuffer->srctextlen++ ];
    pline->t.font = font;
//    if (font) {
//        // DEBUG: check for crash
//        CRLog::trace("c font = %08x  txt = %08x", (lUInt32)font, (lUInt32)text);
//        ((LVFont*)font)->getVisualAligmentWidth();
//    }
//    if (font == NULL && ((flags & LTEXT_WORD_IS_IMAGE) == 0)) {
//        CRLog::fatal("No font specified for text");
//    }
    if ( !lang_cfg )
        lang_cfg = TextLangMan::getTextLangCfg(); // use main_lang
    pline->lang_cfg = lang_cfg;
    if (!len) for (len=0; text[len]; len++) ;
    if (flags & LTEXT_FLAG_OWNTEXT)
    {
        /* make own copy of text */
        // We do a bit ugly to avoid clang-tidy warning "call to 'malloc' has an
        // allocation size of 0 bytes" without having to add checks for NULL pointer
        // (in lvrend.cpp, we're normalling not adding empty text with LTEXT_FLAG_OWNTEXT)
        lUInt32 alloc_len = len > 0 ? len : 1;
        pline->t.text = (lChar32*)malloc( alloc_len * sizeof(lChar32) );
        memcpy((void*)pline->t.text, text, len * sizeof(lChar32));
    }
    else
    {
        pline->t.text = text;
    }
    pline->index = (lUInt16)(pbuffer->srctextlen-1);
    pline->object = object;
    pline->t.len = (lUInt16)len;
    pline->indent = indent;
    pline->flags = flags;
    pline->interval = interval;
    pline->valign_dy = valign_dy;
    pline->t.offset = offset;
    pline->color = color;
    pline->bgcolor = bgcolor;
    pline->letter_spacing = letter_spacing;
}

void lvtextAddSourceObject(
   formatted_text_fragment_t * pbuffer,
   lInt16         width,
   lInt16         height,
   lUInt32         flags,     /* text context flags */
   lUInt16         objflags,  /* object flags */
   lInt16          interval,  /* line height in screen pixels */
   lInt16          valign_dy, /* drift y from baseline */
   lInt16          indent,    /* first line indent (or all but first, when negative) */
   void *          object,    /* pointer to custom object */
   TextLangCfg *   lang_cfg,
   lInt16          letter_spacing
                         )
{
    int srctextsize = (pbuffer->srctextlen + FRM_ALLOC_SIZE-1) / FRM_ALLOC_SIZE * FRM_ALLOC_SIZE;
    if ( pbuffer->srctextlen >= srctextsize)
    {
        srctextsize += FRM_ALLOC_SIZE;
        pbuffer->srctext = cr_realloc( pbuffer->srctext, srctextsize );
    }
    src_text_fragment_t * pline = &pbuffer->srctext[ pbuffer->srctextlen++ ];
    pline->index = (lUInt16)(pbuffer->srctextlen-1);
    pline->flags = flags | LTEXT_SRC_IS_OBJECT;
    pline->o.objflags = objflags;
    pline->o.width = width;
    pline->o.height = height;
    pline->object = object;
    pline->indent = indent;
    pline->interval = interval;
    pline->valign_dy = valign_dy;
    pline->letter_spacing = letter_spacing;
    if ( !lang_cfg )
        lang_cfg = TextLangMan::getTextLangCfg(); // use main_lang
    pline->lang_cfg = lang_cfg;
}


#define DEPRECATED_LINE_BREAK_WORD_COUNT    3
#define DEPRECATED_LINE_BREAK_SPACE_LIMIT   64

// Fetch some extra LTEXT properties from the node style (mostly used for rare inherited
// CSS properties that don't require us to waste a bit in srcline->flags)
int getLTextExtraProperty( src_text_fragment_t * srcline, ltext_extra_t extra_property ) {
    // We return 0 when no property: be sure if returning one of multiple css_xx_something enums,
    // the one with a value of 0 is the one that requires no specific handling (inherit, none, auto...)
    if ( !(srcline->flags & LTEXT_HAS_EXTRA) )
        return 0;
    if ( !srcline->object )
        return 0;
    ldomNode * node = (ldomNode *) srcline->object;
    if ( node->isEffectiveText() )
        node = node->getParentNode();
    if ( !node || node->isNull() )
        return 0;
    css_style_ref_t style = node->getStyle();
    if ( extra_property == LTEXT_EXTRA_CSS_HIDDEN ) {
        return style->visibility >= css_v_hidden ? 1 : 0;
    }
    if ( extra_property == LTEXT_EXTRA_CSS_LINE_BREAK ) {
        return style->line_break; // more than 1 possibly interesting value
    }
    if ( extra_property == LTEXT_EXTRA_CSS_WORD_BREAK ) {
        return style->word_break; // more than 1 possibly interesting value
    }
    if ( extra_property == LTEXT_EXTRA_CSS_TEXT_EMPHASIS ) {
        return (int)style->text_emphasis_style; // css_text_emphasis_style_t value
    }
    return 0;
}

#ifdef __cplusplus

void LFormattedText::AddSourceObject(
            lUInt32         flags,     /* text context flags */
            lUInt16         objflags,  /* object flags */
            lInt16          interval,  /* line height in screen pixels */
            lInt16          valign_dy, /* drift y from baseline */
            lInt16          indent,    /* first line indent (or all but first, when negative) */
            void *          object,    /* pointer to custom object */
            TextLangCfg *   lang_cfg,
            lInt16          letter_spacing
     )
{
    ldomNode * node = (ldomNode*)object;
    if (!node || node->isNull()) {
        TR("LFormattedText::AddSourceObject(): node is NULL!");
        return;
    }
    // Whether the object is a float, an inline-block or an image,
    // nothing much to do with it at this point: we add it with
    // 0-width/height, they will be computed later.
    // (lvtextAddSourceObject will itself add to flags: | LTEXT_SRC_IS_OBJECT)
    lvtextAddSourceObject(m_pbuffer, 0, 0,
        flags, objflags, interval, valign_dy, indent, object, lang_cfg, letter_spacing );

    // Notes about the 3 cases:
    // if (objflags & LTEXT_OBJECT_IS_FLOAT):
    //   Only flags & object parameter will be used, the others are not,
    //   but they matter if this float is the first node in a paragraph,
    //   as the code may grab them from the first source
    // if (objflags & LTEXT_OBJECT_IS_INLINE_BOX):
    //   We can't yet render it to get its width & neight, as they might
    //   be in % of our main width, that we don't know yet (but only
    //   when ->Format() is called).
    // if (objflags & LTEXT_OBJECT_IS_IMAGE):
    //   Handling CSS width and height (and min/max-width/height) will be done
    //   in measureText(), where we know about the buffer width (its container
    //   width) and can better apply values in %
}

// Forward declarations for horizontal layout free functions
// (defined in lvtextfm_layout_h.cpp, called from LVFormatter::splitParagraphs)
class LVFormatter;
void processParagraphHorizontal( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockHorizontal( LVFormatter* fmt, int idx );
// Forward declarations for vertical layout free functions
// (defined in lvtextfm_layout_v.cpp)
void processParagraphVertical( LVFormatter* fmt, int start, int end, bool isLastPara );
void processEmbeddedBlockVertical( LVFormatter* fmt, int idx );
// Helper functions used by measureText() and processParagraphHorizontal()
bool isLeftPunctuation( lChar32 c );
#if (USE_LIBUNIBREAK!=1)
bool isCJKPunctuation( lChar32 c );
bool isCJKLeftPunctuation( lChar32 c );
#endif

class LVFormatter {
public:
    //LVArray<lUInt16>  widths_buf;
    //LVArray<lUInt8>   flags_buf;
    formatted_text_fragment_t * m_pbuffer;
    int       m_length;
    int       m_size;
    bool      m_staticBufs;
    static bool      m_staticBufs_inUse;
    #if (USE_LIBUNIBREAK==1)
    static bool      m_libunibreak_init_done;
    #endif
    lChar32 * m_text;
    lUInt16 * m_flags;
    src_text_fragment_t * * m_srcs;
    lUInt16 * m_charindex;
    int  *     m_advance;
    int  m_line_advance;
    int  m_max_img_height;
    bool m_has_images;
    bool m_has_inline_boxes;
    bool m_has_float_to_position;
    bool m_has_ongoing_float;
    bool m_no_clear_own_floats;
    kerning_mode_t m_kerning_mode;
    bool m_allow_strut_confining;
    bool m_has_multiple_scripts;
    int  m_usable_left_overflow;
    int  m_usable_right_overflow;
    bool m_hanging_punctuation;
    bool m_indent_first_line_done;
    int  m_indent_after_first_line;
    int  m_indent_current;
    int  m_specified_para_dir;
    int  m_writing_mode; // css_wm_horizontal_tb, css_wm_vertical_rl, css_wm_vertical_lr
    #if (USE_FRIBIDI==1)
        // Bidi/RTL support
        FriBidiCharType *    m_bidi_ctypes;
        FriBidiBracketType * m_bidi_btypes;
        FriBidiLevel *       m_bidi_levels;
        FriBidiParType       m_para_bidi_type;
    #endif
    // These default to false and LTR when USE_FRIBIDI==0,
    // just to avoid too many "#if (USE_FRIBIDI==1)"
    bool m_has_bidi; // true when Bidi (or pure RTL) detected
    bool m_para_dir_is_rtl; // boolean shortcut of m_para_bidi_type
    bool m_has_cjk; // true when some CJK met
    int  m_cjk_prev_line_added_space_div; // Used with CJK justified lines, to
    int  m_cjk_prev_line_added_space_mod; // apply same spacing on last line.

// These are not unicode codepoints: these values are put where we
// store text indexes in the source text node.
// So, when checking for these, also checks for m_flags[i] & LCHAR_IS_OBJECT.
// Note that m_charindex, being lUInt16, assume text nodes are not longer
// than 65535 chars. Things will get messy with longer text nodes...
#define IMAGE_CHAR_INDEX      ((lUInt16)0xFFFF)
#define FLOAT_CHAR_INDEX      ((lUInt16)0xFFFE)
#define INLINEBOX_CHAR_INDEX  ((lUInt16)0xFFFD)
#define PAD_CHAR_INDEX        ((lUInt16)0xFFFC)

    LVFormatter(formatted_text_fragment_t * pbuffer)
    : m_pbuffer(pbuffer), m_length(0), m_size(0), m_staticBufs(true), m_line_advance(0)
    {
        #if (USE_LIBUNIBREAK==1)
        if (!m_libunibreak_init_done) {
            m_libunibreak_init_done = true;
            // Have libunibreak build up a few lookup tables for quicker computation
            init_linebreak();
        }
        #endif
        if (m_staticBufs_inUse)
            m_staticBufs = false;
        m_text = NULL;
        m_flags = NULL;
        m_srcs = NULL;
        m_charindex = NULL;
        m_advance = NULL;
        m_has_images = false;
        m_has_inline_boxes = false;
        m_max_img_height = -1;
        m_has_float_to_position = false;
        m_has_ongoing_float = false;
        m_no_clear_own_floats = false;
        m_has_multiple_scripts = false;
        m_usable_left_overflow = 0;
        m_usable_right_overflow = 0;
        m_hanging_punctuation = false;
        m_has_cjk = false;
        m_cjk_prev_line_added_space_div = 0;
        m_cjk_prev_line_added_space_mod = 0;
        m_specified_para_dir = REND_DIRECTION_UNSET;
        #if (USE_FRIBIDI==1)
            m_bidi_ctypes = NULL;
            m_bidi_btypes = NULL;
            m_bidi_levels = NULL;
        #endif
    }

    ~LVFormatter()
    {
    }

    // Embedded floats positioning helpers.
    // Returns y of the bottom of the lowest float
    int getFloatsMaxBottomY() {
        int max_b_y = m_line_advance;
        for (int i=0; i<m_pbuffer->floatcount; i++) {
            embedded_float_t * flt = m_pbuffer->floats[i];
            // Ignore fake floats (no src) made from outer floats footprint
            if ( flt->srctext != NULL ) {
                int b_y = flt->y + flt->height;
                if (b_y > max_b_y)
                    max_b_y = b_y;
            }
        }
        return max_b_y;
    }
    // Returns min y for next float
    int getNextFloatMinY(css_clear_t clear) {
        int y = m_line_advance; // current line y
        for (int i=0; i<m_pbuffer->floatcount; i++) {
            embedded_float_t * flt = m_pbuffer->floats[i];
            if (flt->to_position) // ignore not yet positioned floats
                continue;
            // A later float should never be positioned above an earlier float
            if ( flt->y > y )
                y = flt->y;
            if ( clear > css_c_none) {
                if ( (clear == css_c_both) || (clear == css_c_left && !flt->is_right)
                                           || (clear == css_c_right && flt->is_right) ) {
                    int b_y = flt->y + flt->height;
                    if (b_y > y)
                        y = b_y;
                }
            }
        }
        return y;
    }
    // Returns available width (for text or a new float) available at y
    // and between y and y+h
    // Also set offset_x to the x where this width is available
    int getAvailableWidthAtY(int start_y, int h, int & offset_x) {
        if (m_pbuffer->floatcount == 0) { // common short path when no float
            offset_x = 0;
            // For vertical text, the "line width" is the column height (page_height).
            // Using m_pbuffer->width (horizontal block width) would make alignLine
            // think the column massively overflows, causing heavy space compression.
            // Return page_height directly: processParagraphVertical already limits
            // column content via char_count_adv so frmline->width ≤ page_height,
            // giving extra_width ≥ 0 and no spurious space reduction.  The previous
            // (page_height - strut_height) value caused extra_width = -strut which
            // triggered space reduction, shifting ruby inline-box word->x values
            // upward without the vert_min_next_x clamping that protects plain chars.
            if ( m_pbuffer->writing_mode == css_wm_vertical_rl ||
                 m_pbuffer->writing_mode == css_wm_vertical_lr ) {
                return m_pbuffer->page_height;
            }
            return m_pbuffer->width;
        }
        int fl_left_max_x = 0;
        int fl_right_min_x = m_pbuffer->width;
        // We need to scan line by line from start_y to start_y+h to be sure
        int y = start_y;
        while (y <= start_y + h) {
            for (int i=0; i<m_pbuffer->floatcount; i++) {
                embedded_float_t * flt = m_pbuffer->floats[i];
                if (flt->to_position) // ignore not yet positioned floats
                    continue;
                if (flt->y <= y && flt->y + (int)flt->height > y) { // this float is spanning this y
                    if (flt->is_right) {
                        if (flt->x < fl_right_min_x)
                            fl_right_min_x = flt->x;
                    }
                    else {
                        if (flt->x + flt->width > fl_left_max_x)
                            fl_left_max_x = flt->x + flt->width;
                    }
                }
            }
            y += 1;
        }
        offset_x = fl_left_max_x;
        return fl_right_min_x - fl_left_max_x;
    }
    // Returns next y after start_y where required_width is available
    // Also set offset_x to the x where that width is available
    int getYWithAvailableWidth(int start_y, int required_width, int required_height, int & offset_x, bool get_right_offset_x=false) {
        int y = start_y;
        int w;
        while (true) {
            w = getAvailableWidthAtY(y, required_height, offset_x);
            if (w >= required_width) // found it
                break;
            if (w == m_pbuffer->width) { // We're past all floats
                // returns this y even if required_width is larger than
                // m_pbuffer->width and it will overflow
                offset_x = 0;
                break;
            }
            y += 1;
        }
        if (get_right_offset_x) {
            int left_floats_w = offset_x;
            int right_floats_w = m_pbuffer->width - left_floats_w - w;
            offset_x = m_pbuffer->width - right_floats_w - required_width;
            if (offset_x < 0) // overflow
                offset_x = 0;
        }
        return y;
    }
    // The following positioning codes is not the most efficient, as we
    // call the previous functions that do many of the same kind of loops.
    // But it's the clearest to express the decision flow

    /// Embedded (among other inline elements) floats management
    void addFloat(src_text_fragment_t * src, int currentTextWidth) {
        embedded_float_t * flt =  lvtextAddEmbeddedFloat( m_pbuffer );
        flt->srctext = src;

        ldomNode * node = (ldomNode *) src->object;
        css_float_t float_ = node->getStyle()->float_;
        flt->is_right = ( ( float_ == css_f_right ) ||
                          ( m_para_dir_is_rtl && float_ == css_f_inline_start ) ||
                          (!m_para_dir_is_rtl && float_ == css_f_inline_end ) );
        // clear was not moved to the floatBox: get it from its single child
        flt->clear = node->getChildNode(0)->getStyle()->clear;
        if ( flt->clear == css_c_inline_start ) {
            flt->clear = m_para_dir_is_rtl ? css_c_right : css_c_left;
        }
        else if ( flt->clear == css_c_inline_end ) {
            flt->clear = m_para_dir_is_rtl ? css_c_left : css_c_right;
        }

        // Thanks to the wrapping floatBox element, which has no
        // margin, we can set its RenderRectAccessor to be exactly
        // our embedded_float coordinates and sizes.
        //   If the wrapped element has margins, its renderRectAccessor
        //   will be positioned/sized at the level of borders or padding,
        //   as crengine does naturally with:
        //       fmt.setWidth(width - margin_left - margin_right);
        //       fmt.setHeight(height - margin_top - margin_bottom);
        //       fmt.setX(x + margin_left);
        //       fmt.setY(y + margin_top);
        // So, the RenderRectAccessor(floatBox) can act as a cache
        // of previously rendered and positioned floats!
        int width;
        int height;
        // This formatting code is called when rendering, but can also be called when
        // looking for links, highlighting... so it may happen that floats have
        // already been rendered and positioned, and we already know their width
        // and height.
        bool already_rendered = false;
        { // in its own scope, so this RenderRectAccessor is forgotten when left
            RenderRectAccessor fmt( node );
            if ( RENDER_RECT_HAS_FLAG(fmt, BOX_IS_RENDERED) )
                already_rendered = true;
            // We could also directly use fmt.getX/Y() if it has already been
            // positioned, and avoid the positioning code below.
            // But let's be fully deterministic with that, and redo it.
        }
        if ( !already_rendered ) {
            LVRendPageContext alt_context( NULL, m_pbuffer->page_height, 0, false );
            // We render the float with the specified direction (from upper dir=), even
            // if UNSET (and not with the direction determined by fribidi from the text).
            // We provide 0,0 as the usable left/right overflows, so no glyph/hanging
            // punctuation will leak outside the floatBox.
            renderBlockElement( alt_context, node, 0, 0, m_pbuffer->width, 0, 0, m_specified_para_dir );
            // (renderBlockElement will ensure style->height if requested.)
            // Gather footnotes links accumulated by alt_context
            // (We only need to gather links in the rendering phase, for
            // page splitting, so no worry if we don't when already_rendered)
            lString32Collection * link_ids = alt_context.getLinkIds();
            if (link_ids->length() > 0) {
                flt->links = new lString32Collection();
                for ( int n=0; n<link_ids->length(); n++ ) {
                    flt->links->add( link_ids->at(n) );
                }
            }
        }
        // (renderBlockElement() above may update our RenderRectAccessor(),
        // so (re)get it only now)
        RenderRectAccessor fmt( node );
        width = fmt.getWidth();
        height = fmt.getHeight();

        flt->width = width;
        flt->height = height;
        flt->to_position = true;

        if ( node->getChildCount() > 0 ) {
            // The margins were used to position the original
            // float node in its wrapping floatBox - so get it
            // back from their relative positions
            RenderRectAccessor cfmt(node->getChildNode(0));
            if ( flt->is_right )
                flt->inward_margin = cfmt.getX();
            else
                flt->inward_margin = width - (cfmt.getX() + cfmt.getWidth());
        }

        // If there are already floats to position, don't position any more for now
        if ( !m_has_float_to_position ) {
            if ( getNextFloatMinY(flt->clear) == m_line_advance ) {
                // No previous float, nor any clear:'ing, prevents having this one
                // on current line,
                // See if it can still fit on this line, accounting for the current
                // width used by the text before this inline float (getCurrentLineWidth()
                // accounts for already positioned floats on this line)
                if ( currentTextWidth + flt->width <= getCurrentLineWidth() ) {
                    // Call getYWithAvailableWidth() just to get x
                    int x;
                    int y = getYWithAvailableWidth(m_line_advance, flt->width + currentTextWidth, 0, x, flt->is_right);
                    if (y == m_line_advance) { // should always be true, but just to be sure
                        if (flt->is_right) // correct x: add currentTextWidth we added
                            x = x + currentTextWidth;  // to the width for computation
                        flt->x = x;
                        flt->y = y;
                        flt->to_position = false;
                        fmt.setX(flt->x);
                        fmt.setY(flt->y);
                        if (flt->is_right)
                            RENDER_RECT_SET_FLAG(fmt, FLOATBOX_IS_RIGHT);
                        else
                            RENDER_RECT_UNSET_FLAG(fmt, FLOATBOX_IS_RIGHT);
                        RENDER_RECT_SET_FLAG(fmt, BOX_IS_RENDERED);
                        // Small trick for elements with negative margins (invert dropcaps)
                        // that would overflow above flt->x, to avoid a page split by
                        // sticking the line to the hopefully present margin-top that
                        // precedes this paragraph
                        // (we may want to deal with that more generically by storing these
                        // overflows so we can ensure no page split on the other following
                        // lines as long as they are not consumed)
                        RenderRectAccessor cfmt( node->getChildNode(0));
                        if (cfmt.getY() < 0)
                            m_has_ongoing_float = true;
                        return; // all done with this float
                    }
                }
            }
            m_has_float_to_position = true;
        }
    }
    void positionDelayedFloats() {
        // m_line_advance has been updated, position delayed floats
        if (!m_has_float_to_position)
            return;
        for (int i=0; i<m_pbuffer->floatcount; i++) {
            embedded_float_t * flt = m_pbuffer->floats[i];
            if (!flt->to_position)
                continue;
            int x = 0;
            int y = getNextFloatMinY(flt->clear);
            y = getYWithAvailableWidth(y, flt->width, flt->height, x, flt->is_right);
            flt->x = x;
            flt->y = y;
            flt->to_position = false;
            ldomNode * node = (ldomNode *) flt->srctext->object;
            RenderRectAccessor fmt( node );
            fmt.setX(flt->x);
            fmt.setY(flt->y);
            if (flt->is_right)
                RENDER_RECT_SET_FLAG(fmt, FLOATBOX_IS_RIGHT);
            else
                RENDER_RECT_UNSET_FLAG(fmt, FLOATBOX_IS_RIGHT);
            RENDER_RECT_SET_FLAG(fmt, BOX_IS_RENDERED);
        }
        m_has_float_to_position = false;
    }
    void finalizeFloats() {
        // Adds blank lines to fill the vertical space still occupied by our own
        // inner floats (we don't fill the height of outer floats (float_footprint)
        // as they can still apply over our siblings.)
        fillAndMoveToY( getFloatsMaxBottomY() );
    }
    void fillAndMoveToY(int target_y) {
        // Adds blank lines to fill the vertical space from current m_line_advance to target_y.
        // We need to use 1px lines to get a chance to allow a page wrap at
        // vertically stacked floats boundaries
        if ( target_y <= m_line_advance ) // bogus: we won't rewind y
            return;
        bool has_ongoing_float;
        while ( m_line_advance < target_y ) {
            formatted_line_t * frmline =  lvtextAddFormattedLine( m_pbuffer );
            frmline->y = m_line_advance;
            frmline->x = 0;
            frmline->height = 1;
            frmline->baseline = 1; // no word to draw, does not matter
            // Check if there are floats spanning that y, so we
            // can avoid a page split
            has_ongoing_float = false;
            for (int i=0; i<m_pbuffer->floatcount; i++) {
                embedded_float_t * flt = m_pbuffer->floats[i];
                if (flt->to_position) // ignore not yet positioned floats (even if
                    continue;         // there shouldn't be any when this is called)
                if (flt->y < m_line_advance && flt->y + (int)flt->height > m_line_advance) {
                    has_ongoing_float = true;
                    break;
                }
                // flt->y == m_line_advance is fine: the float starts on this line,
                // we can split on it
            }
            if (has_ongoing_float) {
                frmline->flags |= LTEXT_LINE_SPLIT_AVOID_BEFORE;
            }
            m_line_advance += 1;
            m_pbuffer->height = m_line_advance;
        }
        checkOngoingFloat();
    }
    void floatClearText( int flags ) {
        // Handling of "clear: left/right/both" is different if the 'clear:'
        // is carried by a <BR> or by a float'ing box (for floating boxes, it
        // is done in addFloat()). Here, we deal with <BR style="clear:..">.
        // If a <BR/> has a "clear:", it moves the text below the floats, and the
        // text continues from there.
        // (Only a <BR> can carry a clear: among the non-floating inline elements.)
        if ( flags & LTEXT_SRC_IS_CLEAR_LEFT ) {
            int y = getNextFloatMinY( css_c_left );
            if (y > m_line_advance)
                fillAndMoveToY( y );
        }
        if ( flags & LTEXT_SRC_IS_CLEAR_RIGHT ) {
            int y = getNextFloatMinY( css_c_right );
            if (y > m_line_advance)
                fillAndMoveToY( y );
        }
    }
    int getCurrentLineWidth() {
        int x;
        // m_pbuffer->strut_height is all we can check for at this point,
        // but the text that will be put on this line may exceed it if
        // there's some vertical-align or font size change involved.
        // So, the line could be pushed down and conflict with a float below.
        // But this will do for now...
        return getAvailableWidthAtY(m_line_advance, m_pbuffer->strut_height, x);
    }
    int getCurrentLineX() {
        int x;
        getAvailableWidthAtY(m_line_advance, m_pbuffer->strut_height, x);
        return x;
    }
    bool isCurrentLineWithFloat() {
        int x;
        int w = getAvailableWidthAtY(m_line_advance, m_pbuffer->strut_height, x);
        return w < m_pbuffer->width;
    }
    bool isCurrentLineWithFloatOnLeft() {
        int x;
        getAvailableWidthAtY(m_line_advance, m_pbuffer->strut_height, x);
        return x > 0;
    }
    bool isCurrentLineWithFloatOnRight() {
        int x;
        int w = getAvailableWidthAtY(m_line_advance, m_pbuffer->strut_height, x);
        return x + w < m_pbuffer->width;
    }
    void checkOngoingFloat() {
        // Check if there is still some float spanning at current m_line_advance
        // If there is, next added line will ensure no page split
        // between it and the previous line
        m_has_ongoing_float = false;
        for (int i=0; i<m_pbuffer->floatcount; i++) {
            embedded_float_t * flt = m_pbuffer->floats[i];
            if (flt->to_position) // ignore not yet positioned floats, as they
                continue;         // are not yet running past m_line_advance
            if (flt->y < m_line_advance && flt->y + (int)flt->height > m_line_advance) {
                m_has_ongoing_float = true;
                break;
            }
            // flt->y == m_line_advance is fine: the float starts on this line,
            // no need to avoid page split by next line
        }
    }
    // We prefer to not use the fully usable left overflow, but keep
    // a bit of the margin it comes from
    #define USABLE_OVERFLOW_USABLE_RATIO 0.8
    // Use this for testing computations and get visually perfect fitting
    // #define USABLE_OVERFLOW_USABLE_RATIO 1
    void getCurrentLineUsableOverflows( int & usable_left_overflow, int & usable_right_overflow ) {
        if (m_pbuffer->floatcount > 0) {
            // We have left or right floats on this line, that might
            // make m_usable_left/right_overflow no more relevant.
            // We'll allow the main text to overflow in these floats'
            // inward margin (the float element content itself is also
            // allowed to overflow in it, so its margin is shared;
            // hopefully, both overflowing in it at the same position
            // will be rare).
            // Note that if the float that sets the text min or max x
            // have some large inward margin, an other further float
            // with less inward margin might be the one that should
            // limit the usable overflow.
            int fl_left_max_x = 0;
            int fl_left_max_x_overflow = - m_usable_left_overflow;
            int fl_right_min_x = m_pbuffer->width;
            int fl_right_min_x_overflow = m_pbuffer->width + m_usable_right_overflow;
            // We need to scan pixel line by pixel line along the strut height to be sure
            int y = m_line_advance;
            int end_y = y + m_pbuffer->strut_height;
            while (y <= end_y) {
                for (int i=0; i<m_pbuffer->floatcount; i++) {
                    embedded_float_t * flt = m_pbuffer->floats[i];
                    if (flt->to_position) // ignore not yet positioned floats
                        continue;
                    if (flt->y <= y && flt->y + (int)flt->height > y) { // this float is spanning this y
                        if (flt->is_right) {
                            if (flt->x < fl_right_min_x)
                                fl_right_min_x = flt->x;
                            if (flt->x + flt->inward_margin < fl_right_min_x_overflow)
                                fl_right_min_x_overflow = flt->x + flt->inward_margin;
                                // (inward_margin is the left margin of a right float)
                        }
                        else {
                            if (flt->x + flt->width > fl_left_max_x)
                                fl_left_max_x = flt->x + flt->width;
                            if (flt->x + flt->width - flt->inward_margin > fl_left_max_x_overflow)
                                fl_left_max_x_overflow = flt->x + flt->width - flt->inward_margin;
                                // (inward_margin is the right margin of a left float)
                        }
                    }
                }
                y += 1;
            }
            usable_left_overflow  = fl_left_max_x - fl_left_max_x_overflow;
            usable_right_overflow = fl_right_min_x_overflow - fl_right_min_x;
        }
        else {
            usable_left_overflow  = m_usable_left_overflow;
            usable_right_overflow = m_usable_right_overflow;
        }
        usable_left_overflow  =  usable_left_overflow * USABLE_OVERFLOW_USABLE_RATIO;
        usable_right_overflow = usable_right_overflow * USABLE_OVERFLOW_USABLE_RATIO;
    }

    /// allocate buffers for paragraph
    void allocate( int start, int end )
    {
        int pos = 0;
        int i;
        // PASS 1: calculate total length (characters + objects)
        for ( i=start; i<end; i++ ) {
            src_text_fragment_t * src = &m_pbuffer->srctext[i];
            if ( src->flags & LTEXT_SRC_IS_OBJECT ) {
                pos++;
                if ( (src->o.objflags & LTEXT_OBJECT_IS_IMAGE) && !m_has_images) {
                    // Compute images max height only when we meet an image,
                    // and only for the first one as it's the same for all
                    // images in this paragraph
                    ldomNode * node = (ldomNode *) src->object;
                    if ( node && !node->isNull() ) {
                        // We have to limit the image height so that the line
                        // that contains it does fit in the page without any
                        // uneeded page break
                        m_max_img_height = m_pbuffer->page_height;
                        // remove parent nodes' margin/border/padding, and any strut height
                        // below baseline for any erm_final parent (mostly always one: this
                        // paragraph, but may be more if inside inlineBox or floatBox)
                        m_max_img_height -= node->getSurroundingAddedHeight(true);
                        m_has_images = true;
                    }
                }
                else if ( (src->o.objflags & LTEXT_OBJECT_IS_INLINE_BOX) && !m_has_inline_boxes ) {
                    m_has_inline_boxes = true;
                }
            }
            else {
                pos += src->t.len - src->t.offset;
            }
        }

        // allocate buffers
        m_length = pos;

        TR("allocate(%d)", m_length);
        // We start with static buffers, but when m_length reaches STATIC_BUFS_SIZE,
        // we switch to dynamic buffers and we keep using them (realloc'ating when
        // needed).
        // The code in this file will fill these buffers with m_length items, so
        // from index [0] to [m_length-1], and read them back.
        // Willingly or not (bug?), this code may also access the buffer one slot
        // further at [m_length], and we need to set this slot to zero to avoid
        // a segfault. So, we need to reserve this additional slot when
        // allocating dynamic buffers, or checking if the static buffers can be
        // used.
        // (memset()'ing all buffers on their full allocated size to 0 would work
        // too, but there's a small performance hit when doing so. Just setting
        // to zero the additional slot seems enough, as all previous slots seems
        // to be correctly filled.)

#define STATIC_BUFS_SIZE 8192
#define ITEMS_RESERVED 16

        // "m_length+1" to keep room for the additional slot to be zero'ed
        if ( !m_staticBufs || m_length+1 > STATIC_BUFS_SIZE ) {
            // if (!m_staticBufs && m_text == NULL) printf("allocating dynamic buffers\n");
            if ( m_length+1 > m_size ) {
                // realloc
                m_size = m_length+ITEMS_RESERVED;
                m_text = cr_realloc(m_staticBufs ? NULL : m_text, m_size);
                m_flags = cr_realloc(m_staticBufs ? NULL : m_flags, m_size);
                m_charindex = cr_realloc(m_staticBufs ? NULL : m_charindex, m_size);
                m_srcs = cr_realloc(m_staticBufs ? NULL : m_srcs, m_size);
                m_advance = cr_realloc(m_staticBufs ? NULL : m_advance, m_size);
                #if (USE_FRIBIDI==1)
                    // Note: we could here check for RTL chars (and have a flag
                    // to then not do it in copyText()) so we don't need to allocate
                    // the following ones if we won't be using them.
                    m_bidi_ctypes = cr_realloc(m_staticBufs ? NULL : m_bidi_ctypes, m_size);
                    m_bidi_btypes = cr_realloc(m_staticBufs ? NULL : m_bidi_btypes, m_size);
                    m_bidi_levels = cr_realloc(m_staticBufs ? NULL : m_bidi_levels, m_size);
                #endif
            }
            m_staticBufs = false;
        } else {
            // static buffer space
            static lChar32 m_static_text[STATIC_BUFS_SIZE];
            static lUInt16 m_static_flags[STATIC_BUFS_SIZE];
            static src_text_fragment_t * m_static_srcs[STATIC_BUFS_SIZE];
            static lUInt16 m_static_charindex[STATIC_BUFS_SIZE];
            static int m_static_widths[STATIC_BUFS_SIZE];
            #if (USE_FRIBIDI==1)
                static FriBidiCharType m_static_bidi_ctypes[STATIC_BUFS_SIZE];
                static FriBidiBracketType m_static_bidi_btypes[STATIC_BUFS_SIZE];
                static FriBidiLevel m_static_bidi_levels[STATIC_BUFS_SIZE];
            #endif
            m_text = m_static_text;
            m_flags = m_static_flags;
            m_charindex = m_static_charindex;
            m_srcs = m_static_srcs;
            m_advance = m_static_widths;
            m_staticBufs = true;
            m_staticBufs_inUse = true;
            // printf("using static buffers\n");
            #if (USE_FRIBIDI==1)
                m_bidi_ctypes = m_static_bidi_ctypes;
                m_bidi_btypes = m_static_bidi_btypes;
                m_bidi_levels = m_static_bidi_levels;
            #endif
        }
        memset( m_flags, 0, sizeof(lUInt16)*m_length ); // start with all flags set to zero

        // We set to zero the additional slot that the code may peek at (with
        // the checks against m_length we did, we know this slot is allocated).
        // (This can be removed if we find this was a bug and can fix it)
        m_flags[m_length] = 0;
        m_text[m_length] = 0;
        m_charindex[m_length] = 0;
        m_srcs[m_length] = NULL;
        m_advance[m_length] = 0;
        #if (USE_FRIBIDI==1)
            m_bidi_ctypes[m_length] = 0;
            m_bidi_btypes[m_length] = 0;
            m_bidi_levels[m_length] = 0;
        #endif
    }

    /// copy text of current paragraph to buffers
    void copyText( int start, int end )
    {
        // We might disable/tweak some kerning-like behaviour depending on this setting
        m_kerning_mode = fontMan->GetKerningMode();

        #if (USE_LIBUNIBREAK==1)
        struct LineBreakContext lbCtx;
        // Let's init it before the first char, by adding a leading Zero-Width Joiner
        // (Word Joiner, non-breakable) which should not change the behaviour with
        // the real first char coming up. We then can just use lb_process_next_char()
        // with the real text.
        // The lang lb_props will be plugged in from the TextLangCfg of the
        // coming up text node. We provide NULL in the meantime.
        lb_init_break_context(&lbCtx, 0x200D, NULL); // ZERO WIDTH JOINER
        #endif

        m_has_bidi = false; // will be set if fribidi detects it is bidirectional text
        m_para_dir_is_rtl = false;
        #if (USE_FRIBIDI==1)
        bool has_rtl = false; // if no RTL char, no need for expensive bidi processing
        // todo: according to https://www.w3.org/TR/css-text-3/#bidi-linebox
        // the bidi direction, if determined from the text itself (no dir= from
        // outer containers) must follow up to next paragraphs (separated by <BR/> or newlines).
        // Here in lvtextfm, each gets its own call to copyText(), so we might need some state.
        // This link also points out that line box direction and its text content direction
        // might be different... Could be we have that right (or not).
        // If this para final node or some upper block node specifies dir=rtl, assume fribidi
        // is needed, and avoid checking for rtl chars
        if ( m_specified_para_dir == REND_DIRECTION_RTL ) {
            has_rtl = true;
        }
        #endif

        bool has_non_space = false; // If we have non-empty text, we can do strut confining

        int pos = 0;
        int i;
        bool prev_was_space = true; // start with true, to get rid of all leading spaces
        bool is_locked_spacing = false;
        int last_non_collapsed_space_pos = 0; // reset to -1 if first char is not a space
        int last_non_space_pos = -1; // to get rid of all trailing spaces
        src_text_fragment_t * prev_src = NULL;

        for ( i=start; i<end; i++ ) {
            src_text_fragment_t * src = &m_pbuffer->srctext[i];

            // We will compute wrap rules as if there were no "white-space: nowrap", as
            // we might end up not ensuring nowrap. We just flag all chars (but the last
            // one) inside a text node with "nowrap" with LCHAR_DEPRECATED_WRAP_AFTER,
            // and processParagraph() will deal with chars that have both ALLOW_WRAP_AFTER
            // and DEPRECATED_WRAP_AFTER.
            bool nowrap = src->flags & LTEXT_FLAG_NOWRAP;
            if ( nowrap && pos > 0 ) {
                // We still need to do the right thing at boundaries between 2 nodes
                // with nowrap - and update flags on the last char of previous node.
                // If NOWRAP|NOWRAP: wrap after last char of 1st node is permitted
                // If NOWRAP|WRAP  : wrap after last char of 1st node is permitted
                // If   WRAP|NOWRAP: wrap after last char of 1st node is permitted
                // If   WRAP|WRAP  : it depends
                bool handled = false;
                if ( prev_src && (prev_src->flags & LTEXT_FLAG_NOWRAP) ) {
                    // We don't have much context about these text nodes.
                    // 2 consecutive text nodes might both have "white-space: nowrap",
                    // but it might be allowed to wrap between them if the node that
                    // contains them isn't "nowrap".
                    // So, try to do it that way:
                    // - if both have it, and not their common parent container (so
                    //   it's not inherited): a wrap should be allowed between them.
                    // - if both have it, and their parent container too, a wrap
                    //   shouldn't be allowed between them
                    ldomNode * prev_node = (ldomNode *)prev_src->object;
                    ldomNode * this_node = (ldomNode *)src->object;
                    if ( prev_node && this_node ) {
                        ldomXRange r = ldomXRange( ldomXPointer(prev_node,0), ldomXPointer(this_node,0) );
                        ldomNode * parent = r.getNearestCommonParent();
                        if ( parent && parent->getStyle()->white_space == css_ws_nowrap ) {
                            m_flags[pos-1] |= LCHAR_DEPRECATED_WRAP_AFTER;
                            handled = true;
                        }
                    }
                    else {
                        // One of the 2 nodes is some generated content (list marker,
                        // quote char, BDI wrapping chars) that does not map to a
                        // document node (and we can't reach its parent from here).
                        // Not sure if this would be always good, but let's assume
                        // we want nowrap continuity.
                        m_flags[pos-1] |= LCHAR_DEPRECATED_WRAP_AFTER;
                        handled = true;
                    }
                }
                if ( !handled && (src->flags & LTEXT_SRC_IS_OBJECT)
                              && (src->o.objflags & (LTEXT_OBJECT_IS_IMAGE|LTEXT_OBJECT_IS_INLINE_BOX) ) ) {
                    // Not per-spec, but might be handy:
                    // If an image or our internal inlineBox element has been set
                    // to "white-space: nowrap", it's most probably that it has
                    // inherited it from its parent node - as it's quite unprobable
                    // in real-life that an image was set to "white-space: nowrap"
                    // itself, as it would have no purpose. As for inlineBox,
                    // the original element that has "display: inline-block;
                    // white-space: nowrap" is actually the child of the inlineBox,
                    // and will have it - but they are not propagated up to the
                    // inlineBox wrapper.
                    // So, assume that if such image or inlineBox has it, while
                    // its parent does not, it's because it has been set via
                    // a Style tweak, and that we have used that trick in the
                    // aim to prevent a wrap around it. libunibreak defaults to
                    // allowing a wrap on both sides of such replaced elements;
                    // this allows to easily change this when needed.
                    // (Use-case seen: book with footnotes links that are
                    // set "display:inline-block", which libunibreak could
                    // put at start of line - while we'd rather want them
                    // stuck to the word they follow).
                    ldomNode * this_node = (ldomNode *)src->object;
                    if ( this_node ) {
                        ldomNode * parent = this_node->getParentNode();
                        if ( parent && parent->getStyle()->white_space != css_ws_nowrap ) {
                            m_flags[pos-1] |= LCHAR_DEPRECATED_WRAP_AFTER; // avoid wrap before it
                            m_flags[pos]   |= LCHAR_DEPRECATED_WRAP_AFTER; // avoid wrap after it
                        }
                    }
                }
            }

            // CSS tweaks to line breaking via line-break: and word-break:
            // ("white-space: nowrap" has precedence)
            #if (USE_LIBUNIBREAK==1)
            css_line_break_t css_linebreak = css_lb_auto; // no specific tweak
            css_word_break_t css_wordbreak = css_wb_normal; // no specific tweak
            bool has_css_line_breaking_tweaks = false;
            if ( !nowrap && src->flags & LTEXT_HAS_EXTRA ) {
                css_linebreak = (css_line_break_t)getLTextExtraProperty(src, LTEXT_EXTRA_CSS_LINE_BREAK);
                css_wordbreak = (css_word_break_t)getLTextExtraProperty(src, LTEXT_EXTRA_CSS_WORD_BREAK);
                has_css_line_breaking_tweaks = css_linebreak > css_lb_auto || css_wordbreak > css_wb_break_word;
            }
            #endif

            if ( src->flags & LTEXT_SRC_IS_OBJECT ) {
                if ( src->o.objflags & LTEXT_OBJECT_IS_FLOAT ) {
                    m_text[pos] = 0;
                    m_srcs[pos] = src;
                    m_charindex[pos] = FLOAT_CHAR_INDEX; //0xFFFE;
                    m_flags[pos] = LCHAR_IS_OBJECT;
                        // Note: m_flags was a lUInt8, and there were already 8 LCHAR_IS_* bits/flags
                        //   so we couldn't add our own. But using LCHAR_IS_OBJECT should not hurt,
                        //   as we do the FLOAT tests before it is used.
                        //   m_charindex[pos] is the one to use to detect FLOATs
                        // m_flags has since be updated to lUint16, but no real need
                        // to change what we did for floats to use a new flag.
                    pos++;
                    // No need to update prev_was_space or last_non_space_pos
                    // No need for libunibreak object replacement character
                }
                else if ( src->o.objflags & LTEXT_OBJECT_IS_INLINE_BOX ) {
                    // Note: we shouldn't meet any EmbeddedBlock inlineBox here (and in
                    // processParagraph(), addLine() and alignLine()) as they are dealt
                    // with specifically in splitParagraphs() by processEmbeddedBlock().
                    m_text[pos] = 0;
                    m_srcs[pos] = src;
                    m_charindex[pos] = INLINEBOX_CHAR_INDEX; //0xFFFD;
                    m_flags[pos] = LCHAR_IS_OBJECT;
                    #if (USE_LIBUNIBREAK==1)
                        // Let libunibreak know there was an object, for the followup text
                        // to set LCHAR_ALLOW_WRAP_AFTER on it.
                        // (it will allow wrap before and after an object, unless it's near
                        // some punctuation/quote/paren, whose rules will be ensured it seems).
                        int brk = lb_process_next_char(&lbCtx, (utf32_t)0xFFFC); // OBJECT REPLACEMENT CHARACTER
                        if (pos > 0) {
                            if (brk == LINEBREAK_ALLOWBREAK)
                                m_flags[pos-1] |= LCHAR_ALLOW_WRAP_AFTER;
                            else
                                m_flags[pos-1] &= ~LCHAR_ALLOW_WRAP_AFTER;
                        }
                    #else
                        m_flags[pos] |= LCHAR_ALLOW_WRAP_AFTER;
                    #endif
                    last_non_space_pos = pos;
                    last_non_collapsed_space_pos = -1;
                    prev_was_space = false;
                    is_locked_spacing = false;
                    pos++;
                }
                else if ( src->o.objflags & LTEXT_OBJECT_IS_IMAGE ) {
                    m_text[pos] = 0;
                    m_srcs[pos] = src;
                    m_charindex[pos] = IMAGE_CHAR_INDEX; //0xFFFF;
                    m_flags[pos] = LCHAR_IS_OBJECT;
                    #if (USE_LIBUNIBREAK==1)
                        // Let libunibreak know there was an object
                        int brk = lb_process_next_char(&lbCtx, (utf32_t)0xFFFC); // OBJECT REPLACEMENT CHARACTER
                        if (pos > 0) {
                            if (brk == LINEBREAK_ALLOWBREAK)
                                m_flags[pos-1] |= LCHAR_ALLOW_WRAP_AFTER;
                            else
                                m_flags[pos-1] &= ~LCHAR_ALLOW_WRAP_AFTER;
                        }
                    #else
                        m_flags[pos] |= LCHAR_ALLOW_WRAP_AFTER;
                    #endif
                    last_non_space_pos = pos;
                    last_non_collapsed_space_pos = -1;
                    prev_was_space = false;
                    is_locked_spacing = false;
                    pos++;
                }
                else if ( src->o.objflags & LTEXT_OBJECT_IS_PAD ) {
                    // In case BiDi handling is needed, have a left pad appears as '(' and
                    // a right pad as ')': this looks like it's just enough to have the pads
                    // properly positionnned and ordered as with Firefox.
                    // (Tried initially with using Bidi FSI/PDI, that would also stick them
                    // to their inner content, but with nested inline elements with different
                    // LTR/RTL content, we don't get at all what Firefox renders...
                    // For the parens to be balanced and not mess BiDi level detection, we
                    // need to get both left and right parts as LTEXT_OBJECT_IS_PAD, even if
                    // one side has zero margin/border/padding and would not need it, which
                    // lvrend.cpp's RenderFinalBlock() ensures.
                    m_text[pos] = (src->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT) ? ')' : '(';
                    m_srcs[pos] = src;
                    m_charindex[pos] = PAD_CHAR_INDEX; //0xFFFC;
                    m_flags[pos] = LCHAR_IS_OBJECT;
                    // We don't handle LCHAR_ALLOW_WRAP_AFTER in any m_flags[] slot here, and we
                    // don't feed anything to libunibreak, as this pad should be transparent to the
                    // flow of chars. We let it unset, and will forward the flag that has been
                    // set on the last one to the first one (for left pads) and ensure no wrap
                    // between any of them and next non-pad char (and conversely for right pads).
                    // We will do this in measureText() taking advantage of the loop it does).
                    last_non_space_pos = pos;
                    last_non_collapsed_space_pos = -1;
                    prev_was_space = false;
                    is_locked_spacing = false;
                    pos++;
                }
                else {
                    // Should not happen
                    crFatalError(128, "Unexpected object type");
                }
            }
            else {
                #if (USE_LIBUNIBREAK==1)
                // We hack into lbCtx private member and switch its lbpLang
                // on-the-fly to the props for a possibly new language.
                lbCtx.lbpLang = src->lang_cfg->getLBProps();
                #endif

                int len = src->t.len - src->t.offset;
                lStr_ncpy( m_text+pos, src->t.text + src->t.offset, len );
                if ( i==0 || (src->flags & LTEXT_FLAG_NEWLINE) )
                    m_flags[pos] = LCHAR_MANDATORY_NEWLINE;

                // On non PRE-formatted text, our XML parser have already removed
                // consecutive spaces, \t, \r and \n in each single text node
                // (inside and at boundaries), keeping only (if any) one leading
                // space and one trailing space.
                // These text nodes were simply appended (by lvrend) as is into
                // the src_text_fragment_t->t.text that we are processing here.
                // It may happen then that we, here, do get consecutive spaces, eg with:
                //   "<div> Some <span> text </span> and <span> </span> even more. </div>"
                // which would give us here:
                //   " Some  text  and   even more "
                //
                // https://www.w3.org/TR/css-text-3/#white-space-processing states, for
                // non-PRE paragraphs:
                // (a "segment break" is just a \n in the HTML source - but a space ' ' is not)
                //   (a) A sequence of segment breaks and other white space between two Chinese,
                //       Japanese, or Yi characters collapses into nothing.
                // (So it looks like CJY is CJK minus K - with Korean, if there is a
                // space between K chars, it should be kept, as indeed Korean uses
                // an ascii space to separate words.)
                //   (b) A zero width space before or after a white space sequence containing a
                //       segment break causes the entire sequence of white space to collapse
                //       into a zero width space.
                //   (c) Otherwise, consecutive white space collapses into a single space.
                //
                // For now, we only implement (c).
                // (b) can't really be implemented, as we don't know at this point
                // if there was a segment break or not, as any would have already been
                // converted to a space.
                // (a) can't really be implemented, as here, we can't distinguish any longer
                // a \n from a space, as it has been converted to a space by our XML parser,
                // and only \n should collapse, but not a space. Note that Edge/Chromium
                // don't collapse any of \n or ' ', while Firefox does the right thing by
                // only collapsing \n and keeping ' '.
                //
                // It also states:
                //     Any space immediately following another collapsible space - even one
                //     outside the boundary of the inline containing that space, provided both
                //     spaces are within the same inline formatting context - is collapsed to
                //     have zero advance width. (It is invisible, but retains its soft wrap
                //     opportunity, if any.)
                // (lvtextfm actually deals with a single "inline formatting context", what
                // crengine calls a "final block".)
                //
                // It also states:
                //     - A sequence of collapsible spaces at the beginning of a line is removed.
                //     - A sequence of collapsible spaces at the end of a line is removed.
                //
                // The specs don't say which, among the consecutive collapsible spaces, to
                // keep, so let's keep the first one (they may have different width,
                // eg with: <big> some </big> <small> text </small> )
                //
                // Note: we can't "remove" any char: m_text, src_text_fragment_t->t.text
                // and the ldomNode text node own text need all to be in-sync: a shift
                // because of a removed char in any of them will cause wrong XPointers
                // and Rects (displaced highlights, etc...)
                // We can just "replace" a char (only in m_text, gone after this paragraph
                // processing) or flag (in m_flags for the time of paragraph processing,
                // in word->flags if needed later for drawing).

                bool preformatted = (src->flags & LTEXT_FLAG_PREFORMATTED);
                for ( int k=0; k<len; k++ ) {
                    lChar32 c = m_text[pos];

                    // We flag some chars as we want them to be ignored: some font
                    // would render a glyph (like "[PDI]") for some control chars
                    // that shouldn't be rendered (Harfbuzz would skip them by itself,
                    // but we also want to skip them when using FreeType directly).
                    // We don't skip them when filling these buffer, as some of them
                    // can give valuable information to the bidi algorithm.
                    // Ignore the unicode direction hints (that we may have added ourselves
                    // in lvrend.cpp when processing <bdi>, <bdo> and the dir= attribute).
                    // Try to balance the searches:
                    bool is_to_ignore = false;
                    if ( c >= 0x202A ) {
                        if ( c <= 0x2069 ) {
                            if ( c <= 0x202E ) is_to_ignore = true;      // 202A>202E
                            else if ( c >= 0x2066 ) is_to_ignore = true; // 2066>2069
                        }
                    }
                    else if ( c <= 0x009F ) {
                        // Also ignore some ASCII and Unicode control chars
                        // in the ranges 00>1F and 7F>9F, except a few.
                        // (Some of these can be found in old documents or
                        // badly converted ones)
                        if ( c <= 0x001F ) {
                            // Let \t \n \r be (they might have already been
                            // expanded to spaces, converted or skipped)
                            if ( c != 0x000A && c!= 0x000D && c!= 0x0009 )
                                is_to_ignore = true; // 0000>001F except those above
                        }
                        else if ( c >= 0x007F ) {
                            is_to_ignore = true;     // 007F>009F
                        }
                    }
                    // We might want to add some others when we happen to meet them.
                    // todo: see harfbuzz hb-unicode.hh is_default_ignorable() for how
                    // to do this kind of check fast

                    // If not on a 'pre' text node, we should strip trailing
                    // spaces and collapse consecutive spaces (other spaces
                    // like UNICODE_NO_BREAK_SPACE should not collapse).
                    bool is_space = (c == ' ');
                    if ( is_to_ignore ) {
                        m_flags[pos] = LCHAR_IS_TO_IGNORE;
                        // Don't update any space related state when meeting an ignorable
                    }
                    else if ( is_space && !preformatted ) {
                        if ( prev_was_space ) {
                            // On non-pre text nodes, flag spaces following a space
                            // so we can discard them later.
                            // Note: the behaviour with consecutive spaces in a mix
                            // of pre and non-pre text nodes has not been tested,
                            // and what we do here might be wrong.
                            // Note: with a mix of normal spaces and non-break-spaces,
                            // we seem to behave just as Firefox.
                            // Note: for the empty lines or indentation we might add
                            // with 'txform->AddSourceLine(U" "...)', we need to
                            // provide LTEXT_FLAG_PREFORMATTED if we don't want them
                            // to be collapsed.
                            m_flags[pos] = LCHAR_IS_COLLAPSED_SPACE | LCHAR_ALLOW_WRAP_AFTER;
                            // m_text[pos] = '_'; // uncomment when debugging
                            // (We can replace the char to see it in printf() (m_text is not the
                            // text that is drawn, it's measured but we correct the measure
                            // by setting a zero width, it's just used here for analysis.
                            // But best to let it as-is except for debugging)
                        }
                        else {
                            last_non_collapsed_space_pos = pos;
                        }
                        // Locked spacing can be set on any space among contiguous spaces,
                        // but will be useful only on the non-collapsed one. We propagate
                        // it on all previous and following spaces so we don't have to
                        // redo-it after any BiDi re-ordering (not sure thus this will
                        // be alright...)
                        // (This is for now only used with FB2 run-in footnotes to ensure
                        // a constant width between the footnote number and its following
                        // text, but could be used with list item markers/numbers.)
                        if ( src->flags & LTEXT_LOCKED_SPACING )
                            is_locked_spacing = true;
                        if ( is_locked_spacing ) {
                            m_flags[pos] |= LCHAR_LOCKED_SPACING;
                            if ( last_non_collapsed_space_pos >= 0 ) { // update previous spaces
                                for ( int j=last_non_collapsed_space_pos; j<pos; j++ ) {
                                    m_flags[j] |= LCHAR_LOCKED_SPACING;
                                }
                            }
                        }
                        prev_was_space = true;
                    }
                    else {
                        // don't strip traling spaces if pre
                        last_non_space_pos = pos;
                        last_non_collapsed_space_pos = -1;
                        is_locked_spacing = false;
                        if ( !has_non_space ) {
                            if ( !is_space && c != UNICODE_NO_BREAK_SPACE ) {
                                has_non_space = true;
                            }
                        }
                        if ( preformatted && is_space ) {
                            // Be sure the various places we may change the width
                            // of a space don't trigger
                            m_flags[pos] |= LCHAR_LOCKED_SPACING;
                        }
                        prev_was_space = is_space || (c == '\n');
                            // We might meet '\n' in PRE text, which shouldn't make any space
                            // collapsed - except when "white-space: pre-line". So, have
                            // a space following a \n be allowed to collapse.
                    }

                    if ( lStr_isCJK(c) ) {
                        // We have some specific code for handling CJK typography, that we don't
                        // need to trigger if we didn't meet any CJK char.
                        if ( !m_has_cjk ) {
                            m_has_cjk = true;
                        }
                        m_flags[pos] |= LCHAR_IS_CJK;
                        // Some CJK fullwidth punctuation char usually have a good amount of
                        // their glyph width blank, and we can reduce their width if needed.
                        // We explicitely don't set this flag (which is enough to not have
                        // any related processing done) when kerning is disabled (as this is
                        // doing some kind of kerning) to allow comparing, and in case some
                        // people prefer to get the legacy non-tweaked rendering.
                        if ( m_kerning_mode != KERNING_MODE_DISABLED && getCJKCharType(c) != cjkt_other )
                            m_flags[pos] |= LCHAR_IS_FLEXIBLE_WIDTH_CJK;
                    }

                    // if ( ch == '-' || ch == 0x2010 || ch == '.' || ch == '+' || ch==UNICODE_NO_BREAK_SPACE )
                    //     m_flags[pos] |= LCHAR_DEPRECATED_WRAP_AFTER;
                    // Some of these (in the 2 commented lines just above) will be set
                    // in lvfntman measureText().
                    // We might want to have them all done here, for clarity.

                    // Note: the overhead of using one of the following is quite minimal, so do if needed
                    /*
                    utf8proc_category_t uc = utf8proc_category(c);
                    if (uc == UTF8PROC_CATEGORY_CF)
                        printf("format char %x\n", c);
                    else if (uc == UTF8PROC_CATEGORY_CC)
                        printf("control char %x\n", c);
                    // Alternative, using HarfBuzz:
                    int uc = hb_unicode_general_category(hb_unicode_funcs_get_default(), c);
                    if (uc == HB_UNICODE_GENERAL_CATEGORY_FORMAT)
                        printf("format char %x\n", c);
                    else if (uc == HB_UNICODE_GENERAL_CATEGORY_CONTROL)
                        printf("control char %x\n", c);
                    */

                    #if (USE_LIBUNIBREAK==1)
                    if ( nowrap ) {
                        // If "white-space: nowrap", we flag everything but the last char
                        // (So, for a 1 char long text node, no flag.)
                        if ( k < len-1 ) {
                            m_flags[pos] |= LCHAR_DEPRECATED_WRAP_AFTER;
                        }
                    }
                    lChar32 ch = m_text[pos];
                    if ( src->lang_cfg->hasLBCharSubFunc() ) {
                        // Lang specific function may want to substitute char (for
                        // libunibreak only) to tweak line breaking around it
                        ch = src->lang_cfg->getLBCharSubFunc()(&lbCtx, m_text, pos, len-1 - k);
                        // We do this before the following, to allow this lang specific function
                        // to possibly tweak the more generic getCssLbCharSub()
                    }
                    if ( has_css_line_breaking_tweaks ) {
                        // CSS line breaking tweaks by char substitution (we need to provide our 'ch'
                        // as it may have been tweaked and differ from m_text[pos]...)
                        ch = src->lang_cfg->getCssLbCharSub(css_linebreak, css_wordbreak, &lbCtx, m_text, pos, len-1 - k, ch);
                    }
                    int brk = lb_process_next_char(&lbCtx, (utf32_t)ch);
                    if ( pos > 0 ) {
                        // printf("between <%c%c>: brk %d\n", m_text[pos-1], m_text[pos], brk);
                        // printf("between <%x.%x>: brk %d\n", m_text[pos-1], m_text[pos], brk);
                        if (brk != LINEBREAK_ALLOWBREAK) {
                            m_flags[pos-1] &= ~LCHAR_ALLOW_WRAP_AFTER;
                        }
                        else {
                            m_flags[pos-1] |= LCHAR_ALLOW_WRAP_AFTER;
                            // brk is set on the last space in a sequence of multiple spaces.
                            //   between <ne>: brk 2
                            //   between <ed>: brk 2
                            //   between <d.>: brk 2
                            //   between <. >: brk 2
                            //   between <  >: brk 2
                            //   between <  >: brk 2
                            //   between < T>: brk 1
                            //   between <Th>: brk 2
                            //   between <he>: brk 2
                            //   between <ey>: brk 2
                            //   between <y >: brk 2
                            //   between <  >: brk 2
                            //   between < h>: brk 1
                            //   between <ha>: brk 2
                            //   between <av>: brk 2
                            //   between <ve>: brk 2
                            //   between <e >: brk 2
                            //   between < a>: brk 1
                            //   between <as>: brk 2
                            // Given the algorithm described in addLine(), we want the break
                            // after the first space, so the following collapsed spaces can
                            // be at start of next line where they will be ignored.
                            // (Not certain this is really needed, but let's do it, as the
                            // code expecting that has been quite well tested and fixed over
                            // the months, so let's avoid adding uncertainty.)
                            if ( m_text[pos-1] == ' ' ) {
                                // Allowed break after a space. If we have other spaces before,
                                // we are allowed to break after each of them too.
                                // This space and the previous ones (except the first) are probably
                                // LCHAR_IS_COLLAPSED_SPACE, but they can also be non-collapsable
                                // spaces if from white-space:pre nodes (which can be mixed).
                                // We should still be allowed to break on any of them (and this
                                // really matter with white-space:pre, as we don't want a long
                                // sequence of spaces to not break (otherwise, the only break
                                // could be with hyphenating the previous word...)
                                // (If white-space:nowrap, wrap will be prevented later thanks
                                // to LCHAR_DEPRECATED_WRAP_AFTER we have set earlier.)
                                int j = pos-2;
                                while ( j >= 0 && ( m_text[j] == ' ' ) ) {
                                    m_flags[j] |= LCHAR_ALLOW_WRAP_AFTER;
                                    j--;
                                }
                            }
                        }
                    }
                    #endif

                    #if (USE_FRIBIDI==1)
                        // Also try to detect if we have RTL chars, so that if we don't have any,
                        // we don't need to invoke expensive fribidi processing below (which
                        // may add a 50% duration increase to the text rendering phase).
                        if ( !has_rtl ) {
                            has_rtl = lStr_isRTL(c);
                        }
                    #endif

                    m_charindex[pos] = k;
                    m_srcs[pos] = src;
                    pos++;
                }
            }
            prev_src = src;
        }
        // Also flag as collapsed all spaces at the end of text
        pos = pos-1; // get back last pos++
        if (last_non_space_pos >= 0 && last_non_space_pos+1 <= pos) {
            for ( int k=last_non_space_pos+1; k<=pos; k++ ) {
                if (m_flags[k] == LCHAR_IS_OBJECT)
                    continue; // don't unflag floats
                if (m_flags[k] & LCHAR_IS_TO_IGNORE)
                    continue;
                m_flags[k] = LCHAR_IS_COLLAPSED_SPACE | LCHAR_ALLOW_WRAP_AFTER;
                // m_text[k] = '='; // uncomment when debugging
            }
        }
        TR("%s", LCSTR(lString32(m_text, m_length)));

        // Whether any "-cr-hint: strut-confined" should be applied: only when
        // we have non-space-only text in the paragraph - standalone images
        // possibly separated by spaces don't need to be reduced in size.
        // And only when we actually have a strut set (list item markers
        // with "list-style-position: outside" don't have any set).
        m_allow_strut_confining = has_non_space && m_pbuffer->strut_height > 0;

        #if (USE_FRIBIDI==1)
        if ( has_rtl ) {
            // Trust the direction determined by renderBlockElementEnhanced() from the
            // upper nodes dir= attributes or CSS style->direction.
            if ( m_specified_para_dir == REND_DIRECTION_RTL ) {
                m_para_bidi_type = FRIBIDI_PAR_RTL; // Strong RTL
            }
            else if ( m_specified_para_dir == REND_DIRECTION_LTR ) {
                m_para_bidi_type = FRIBIDI_PAR_LTR; // Strong LTR
            }
            else { // REND_DIRECTION_UNSET
                m_para_bidi_type = FRIBIDI_PAR_WLTR; // Weak LTR (= auto with a bias toward LTR)
            }

            // Compute bidi levels
            fribidi_get_bidi_types( (const FriBidiChar*)m_text, m_length, m_bidi_ctypes);
            fribidi_get_bracket_types( (const FriBidiChar*)m_text, m_length, m_bidi_ctypes, m_bidi_btypes);

            // We would have simply done:
            //   int max_level = fribidi_get_par_embedding_levels_ex(m_bidi_ctypes, m_bidi_btypes,
            //                     m_length, (FriBidiParType*)&m_para_bidi_type, m_bidi_levels);
            // But unfortunately, fribidi_get_par_embedding_levels_ex() only works on a single
            // paragraph, and will set bogus levels for the text following the first \n (or other
            // Unicode Block Separators, BS), which may happen if this text is white-space:pre.
            // FriBiDi expects us to work only on individual paragraphs. But we
            // still want to process the whole text here so that we're done with it.
            // So, split on BS and call fribidi_get_par_embedding_levels_ex() on
            // each segment - hoping doing it that way is OK...
            // Note that if we added Unicode BiDi control chars to ensure dir='rtl' carried
            // by inner inline elements encompassing text nodes containing '\n', we will
            // lose their state/balancing and get wrong results... We anyway try to remember
            // and forward the latest active one met (enough or not? better than nothing...).
            FriBidiCharType active_ctrl_char = 0;
            int restore_bs_idx = -1;
            src_text_fragment_t * cur_src = NULL;
            int max_level = 0;
            int s_start = 0;
            int i = 0;
            while ( i <= m_length ) {
                if ( i == m_length || m_bidi_ctypes[i] == FRIBIDI_TYPE_BS ) {
                    int s_length = i - s_start;
                    if (i < m_length)
                        s_length += 1; // include BS at i in segment
                    FriBidiCharType *    bidi_ctypes = (FriBidiCharType *)   (m_bidi_ctypes + s_start);
                    FriBidiBracketType * bidi_btypes = (FriBidiBracketType *)(m_bidi_btypes + s_start);
                    FriBidiLevel *       bidi_levels = (FriBidiLevel *)      (m_bidi_levels + s_start);
                    int this_max_level = fribidi_get_par_embedding_levels_ex(bidi_ctypes, bidi_btypes,
                                                                s_length, &m_para_bidi_type, bidi_levels);
                    if ( this_max_level > max_level )
                        max_level = this_max_level;
                    if ( restore_bs_idx >= 0 ) {
                        // Be polite and restore the original bidi type (not certain it is
                        // really needed, but we reuse these array again in AddLine().)
                        m_bidi_ctypes[restore_bs_idx] = FRIBIDI_TYPE_BS;
                        restore_bs_idx = -1;
                    }
                    if ( i == m_length )
                        break;
                    if ( active_ctrl_char ) {
                        // We can override this \n bidi type, by the one still active,
                        // and include this masqueraded char in the next segment handling
                        s_start = i;
                        m_bidi_ctypes[i] = active_ctrl_char;
                        restore_bs_idx = i;
                    }
                    else {
                        // Otherwise, skip this \n, and handle next segment
                        s_start = i+1;
                    }
                }
                if ( m_srcs[i] != cur_src ) { // (Only waste time checking this when we're crossing sources)
                    cur_src = m_srcs[i];
                    if ( cur_src->flags & LTEXT_FLAG_OWNTEXT && cur_src->t.len == 1 && cur_src->object && ((ldomNode *)cur_src->object)->isElement() ) {
                        // This char is from a 1-char text fragment, and not from a regular text node: it is
                        // text we have explicitely added in renderFinalBlock(), and it may be one of our
                        // BiDi control char we added when handling dir='rtl'.
                        // (This is ok because we ended up using only single-char such BiDi control
                        // chars, and not the 2-chars combinations.)
                        switch ( m_bidi_ctypes[i] ) {
                            case FRIBIDI_TYPE_LRI:
                                active_ctrl_char = FRIBIDI_TYPE_LRI;
                                break;
                            case FRIBIDI_TYPE_RLI:
                                active_ctrl_char = FRIBIDI_TYPE_RLI;
                                break;
                            case FRIBIDI_TYPE_FSI:
                                // Possibly wrong to forward this one, as it's about the first strong
                                // isolate following it - not the first on we will meet on the next
                                // segment... But this might be better than nothing.
                                active_ctrl_char = FRIBIDI_TYPE_FSI;
                                break;
                            case FRIBIDI_TYPE_PDI:
                                // pop (no stack, so we won't restore a previous one)
                                active_ctrl_char = 0;
                                break;
                        }
                    }
                }
                i++;
            }

            // If computed max level == 1, we are in plain and only LTR, so no need for
            // more bidi work later.
            if ( max_level > 1 ) {
                m_has_bidi = true;
            }
            if ( m_para_bidi_type == FRIBIDI_PAR_RTL || m_para_bidi_type == FRIBIDI_PAR_WRTL )
                m_para_dir_is_rtl = true;

            // fribidi_shape(FRIBIDI_FLAG_SHAPE_MIRRORING, m_bidi_levels, m_length, NULL, (FriBidiChar*)m_text);
            // No use mirroring at this point I think, as it's not the text that will
            // be drawn. Hoping parens & al. have the same widths when mirrored.
            // We'll do that in addLine() when processing words when meeting
            // a rtl one, with fribidi_get_mirror_char().

            /* For debugging:
                printf("par_type %d , max_level %d\n", m_para_bidi_type, max_level);
                for (int i=0; i<m_length; i++)
                    printf("%d", m_bidi_levels[i]);
                printf("\n");
            // We get:
            //   pure LTR: par_type 272 , max_level 1  0000000000
            //   pure RTL: par_type 273 , max_level 2  1111111111
            //   LTR at start with later some RTL: par_type 272 , max_level 2  00000111111000000000000000
            //   RTL at start with later some LTR: par_type 273 , max_level 3  1111111111112222222222222221
            */
        }
        #endif
    }

    void resizeImage( int & width, int & height, int maxw, int maxh, bool isInline )
    {
        //CRLog::trace("Resize image (%dx%d) max %dx%d %s", width, height, maxw, maxh, isInline ? "inline" : "block");
        bool arbitraryImageScaling = false;
        int maxScale = 1;
        bool zoomIn = width<maxw && height<maxh;
        if ( isInline ) {
            if ( zoomIn ) {
                if ( m_pbuffer->img_zoom_in_mode_inline==0 )
                    return; // no zoom
                arbitraryImageScaling = m_pbuffer->img_zoom_in_mode_inline == 2;
                maxScale = m_pbuffer->img_zoom_in_scale_inline;
            } else {
//                if ( m_pbuffer->img_zoom_out_mode_inline==0 )
//                    return; // no zoom
                arbitraryImageScaling = m_pbuffer->img_zoom_out_mode_inline == 2;
                maxScale = m_pbuffer->img_zoom_out_scale_inline;
            }
        } else {
            if ( zoomIn ) {
                if ( m_pbuffer->img_zoom_in_mode_block==0 )
                    return; // no zoom
                arbitraryImageScaling = m_pbuffer->img_zoom_in_mode_block == 2;
                maxScale = m_pbuffer->img_zoom_in_scale_block;
            } else {
//                if ( m_pbuffer->img_zoom_out_mode_block==0 )
//                    return; // no zoom
                arbitraryImageScaling = m_pbuffer->img_zoom_out_mode_block == 2;
                maxScale = m_pbuffer->img_zoom_out_scale_block;
            }
        }
        resizeImage( width, height, maxw, maxh, arbitraryImageScaling, maxScale );
    }

    void resizeImage( int & width, int & height, int maxw, int maxh, bool arbitraryImageScaling, int maxScaleMult )
    {
        if (width <= 0 || height <= 0) {
            // Reject nonsensical values (and avoids the potential for an FPE if 0)
            printf("CRE WARNING: resizeImage(width<=0 or height<=0)\n");
            return;
        }
        if (maxw <= 0 || maxh <= 0) {
            // Ditto
            printf("CRE WARNING: resizeImage(maxw<=0 or maxh<=0)\n");
            return;
        }
        //CRLog::trace("Resize image (%dx%d) max %dx%d %s  *%d", width, height, maxw, maxh, arbitraryImageScaling ? "arbitrary" : "integer", maxScaleMult);

        if ( maxScaleMult<1 ) {
            maxScaleMult = 1;
        }

        if ( !arbitraryImageScaling ) {
            // Integer scaling, constrained to maxScaleMult
            for ( int i = maxScaleMult; i > 0; i-- ) {
                // Use the largest integer multiplier that fits
                int scaled_width = width * i;
                int scaled_height = height * i;
                if ( scaled_width <= maxw && scaled_height <= maxh ) {
                    width = scaled_width;
                    height = scaled_height;
                    return;
                }
            }

            // Fall through to arbitrary scaling
        }

        // Make sure we never blow past maxScaleMult while still fitting inside maxw/maxh
        int bbox_width = width * maxScaleMult > maxw ? maxw : width * maxScaleMult;
        int bbox_height = height * maxScaleMult > maxh ? maxh : height * maxScaleMult;

        int scaled_width;
        int scaled_height;
        // And now see whether we need to compute width or height to honor the AR.
        // c.f., QSize::scaled @ https://github.com/qt/qtbase/blob/dev/src/corelib/tools/qsize.cpp for Qt::KeepAspectRatio
        int rescaled_width = bbox_height * width / height;
        if ( rescaled_width <= bbox_width ) {
            scaled_width = rescaled_width;
            scaled_height = bbox_height;
        } else {
            scaled_width = bbox_width;
            scaled_height = bbox_width * height / width;
        }

        // We're done, update out pointers
        width = scaled_width;
        height = scaled_height;
    }

    /// measure word
    bool measureWord(formatted_word_t * word, int & width)
    {
        src_text_fragment_t * srcline = &m_pbuffer->srctext[word->src_text_index];
        LVFont * srcfont= (LVFont *) srcline->t.font;
        const lChar32 * str = srcline->t.text + word->t.start;
        // Avoid malloc by using static buffers. Returns false if word too long.
        #define MAX_MEASURED_WORD_SIZE 127
        static lUInt16 widths[MAX_MEASURED_WORD_SIZE+1];
        static lUInt8 flags[MAX_MEASURED_WORD_SIZE+1];
        if (word->t.len > MAX_MEASURED_WORD_SIZE)
            return false;
        lUInt32 hints = WORD_FLAGS_TO_FNT_FLAGS(word->flags);
        srcfont->measureText(
                str,
                word->t.len,
                widths, flags,
                0x7FFF,
                '?',
                srcline->lang_cfg,
                srcline->letter_spacing,
                false,
                hints );
        width = widths[word->t.len-1];
        return true;
    }

    /// measure text of current paragraph
    void measureText()
    {
        int i;
        src_text_fragment_t * lastSrc = NULL;
        LVFont * lastFont = NULL;
        lInt16 lastLetterSpacing = 0;
        int start = 0;
        int lastWidth = 0;
        #define MAX_TEXT_CHUNK_SIZE 4096
        static lUInt16 widths[MAX_TEXT_CHUNK_SIZE+1];
        static lUInt8 flags[MAX_TEXT_CHUNK_SIZE+1];
        int tabIndex = -1;
        #if (USE_FRIBIDI==1)
            FriBidiLevel lastBidiLevel = 0;
            FriBidiLevel newBidiLevel;
        #endif
        #if (USE_HARFBUZZ==1)
            bool usingHarfbuzz = m_kerning_mode == KERNING_MODE_HARFBUZZ;
            // Unicode script change (note: hb_script_t is uint32_t)
            lUInt32 prevScript = HB_SCRIPT_COMMON;
            hb_unicode_funcs_t* _hb_unicode_funcs = hb_unicode_funcs_get_default();
            bool prevSpecificScriptIsCursive = false;
        #endif
        int first_word_len = 0; // set to -1 when done with it (only used to check
                                // for single char first word, see below)
        for ( i=0; i<=m_length; i++ ) {
            LVFont * newFont = NULL;
            lInt16 newLetterSpacing = 0;
            src_text_fragment_t * newSrc = NULL;
            if ( tabIndex<0 && m_text[i]=='\t' ) {
                tabIndex = i;
            }
            bool isObject = false;
            bool prevCharIsObject = false;
            if ( i<m_length ) {
                newSrc = m_srcs[i];
                isObject = m_flags[i] & LCHAR_IS_OBJECT; // image, float or inline box
                newFont = isObject ? NULL : (LVFont *)newSrc->t.font;
                newLetterSpacing = newSrc->letter_spacing; // 0 for objects
            }
            if (i > 0)
                prevCharIsObject = m_flags[i-1] & LCHAR_IS_OBJECT; // image, float or inline box
            if ( !lastFont )
                lastFont = newFont;
            if (i == 0) {
                lastSrc = newSrc;
                lastLetterSpacing = newLetterSpacing;
            }
            bool srcChangedAndUsingHarfbuzz = false;
            #if (USE_HARFBUZZ==1)
                // When 2 contiguous text nodes have the same font, we measure the
                // whole combined segment. But when making words, we split on
                // text node change. When using full harfbuzz, we don't want it
                // to make ligatures at such text nodes boundaries: we need to
                // measure each text node individually.
                if ( usingHarfbuzz && newSrc != lastSrc && newFont && newFont == lastFont ) {
                    srcChangedAndUsingHarfbuzz = true;
                }
            #endif
            bool bidiLevelChanged = false;
            int lastDirection = 0; // unknown
            #if (USE_FRIBIDI==1)
                lastDirection = 1; // direction known: LTR if no bidi found
                if (m_has_bidi) {
                    newBidiLevel = m_bidi_levels[i];
                    if (i == 0)
                        lastBidiLevel = newBidiLevel;
                    else if ( newBidiLevel != lastBidiLevel )
                        bidiLevelChanged = true;
                    if ( FRIBIDI_LEVEL_IS_RTL(lastBidiLevel) )
                        lastDirection = -1; // RTL
                }
            #endif
            // When measuring with Harfbuzz, we should also split on Unicode script change,
            // even in a same bidi level (mixed hebrew and arabic in a single text node
            // should be handled as multiple segments, or Harfbuzz would shape the whole
            // text with the script of the first kind of text it meets).
            bool scriptChanged = false;
            #if (USE_HARFBUZZ==1)
                if ( usingHarfbuzz && !isObject ) {
                    // While we have the hb_script here, we'll update m_flags[i]
                    // with LCHAR_LOCKED_SPACING if the script is cursive
                    hb_script_t script = hb_unicode_script(_hb_unicode_funcs, m_text[i]);
                    if ( script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_UNKNOWN ) {
                        if ( script != prevScript ) {
                            if ( prevScript != HB_SCRIPT_COMMON ) {
                                // We previously met a real script, and we're meeting a new one
                                scriptChanged = true;
                                m_has_multiple_scripts = true;
                                // When only a single script found in a paragraph, we don't need
                                // to do that same kind of work in AddLine() to split on script
                                // change, as there's only one.
                            }
                            prevSpecificScriptIsCursive = isHBScriptCursive(script);
                        }
                        prevScript = script; // Real script met
                        if ( prevSpecificScriptIsCursive )
                            m_flags[i] |= LCHAR_LOCKED_SPACING;
                    }
                    // else: assume HB_SCRIPT_COMMON/INHERITED/UNKNOWN, even among cursive glyphs,
                    // can be letter_space'd for justification.
                }
            #endif
            // Note: some additional tweaks (like disabling letter-spacing when
            // a cursive script is detected) are done in measureText() and drawTextString().

            // Make a new segment to measure when any property changes from previous char
            if ( i>start && (   newFont != lastFont
                             || newLetterSpacing != lastLetterSpacing
                             || srcChangedAndUsingHarfbuzz
                             || bidiLevelChanged
                             || scriptChanged
                             || isObject
                             || prevCharIsObject
                             || i >= start+MAX_TEXT_CHUNK_SIZE
                             || (m_flags[i] & LCHAR_IS_TO_IGNORE)
                             || (m_flags[i] & LCHAR_MANDATORY_NEWLINE) ) ) {
                // measure start..i-1 chars
                bool measuring_object = m_flags[i-1] & LCHAR_IS_OBJECT;
                if ( !measuring_object && lastFont ) { // text node
                        // In our context, we'll always have a non-NULL lastFont, but
                        // have it checked explicitely to avoid clang-tidy warning.
                    // measure text
                    // Note: we provide text in the logical order, and measureText()
                    // will apply kerning in that order, which might be wrong if some
                    // text fragment happens to be RTL (except for Harfbuzz which will
                    // do the right thing).
                    int len = i - start;
                    // Provide direction and start/end of paragraph hints, for Harfbuzz
                    lUInt32 hints = 0;
                    if ( start == 0 ) hints |= LFNT_HINT_BEGINS_PARAGRAPH;
                    if ( i == m_length ) hints |= LFNT_HINT_ENDS_PARAGRAPH;
                    if ( css_wm_is_vertical(m_writing_mode) ) {
                        hints |= LFNT_HINT_IS_VERTICAL;
                        hints |= LFNT_HINT_DIRECTION_KNOWN;
                        hints |= LFNT_HINT_DIRECTION_IS_TTB;
                    }
                    else if ( lastDirection ) {
                        hints |= LFNT_HINT_DIRECTION_KNOWN;
                        if ( lastDirection < 0 )
                            hints |= LFNT_HINT_DIRECTION_IS_RTL;
                    }
                    int max_width_for_measure = 0x7FFF; //pbuffer->width,
                    if ( css_wm_is_vertical(m_writing_mode) ) {
                        max_width_for_measure = m_pbuffer->page_height;
                    }
                    int chars_measured = lastFont->measureText(
                            m_text + start,
                            len,
                            widths, flags,
                            max_width_for_measure,
                            '?',
                            lastSrc->lang_cfg,
                            lastLetterSpacing,
                            false,
                            hints
                            );
                    if ( chars_measured<len ) {
                        // printf("######### chars_measured %d < %d\n", chars_measured, len);
                        // too long line
                        int newlen = chars_measured;
                        if (newlen == 0) {
                            // measureText returned 0 — advance at least 1 to avoid infinite loop
                            newlen = 1;
                            chars_measured = 1;
                            widths[0] = 0;
                        }
                        i = start + newlen;
                        len = newlen;
                        // As we're going to continue measuring this text node,
                        // reset newFont (the font of the next text node), so
                        // it does not replace lastFont at the end of the loop.
                        newFont = NULL;
                        // If we didn't measure the full text, src, letter spacing and
                        // bidi level are to stay the same
                        newSrc = lastSrc;
                        newLetterSpacing = lastLetterSpacing;
                        #if (USE_FRIBIDI==1)
                            if (m_has_bidi)
                                newBidiLevel = lastBidiLevel;
                        #endif
                    }


                    // Deal with chars flagged as collapsed spaces:
                    // make each zero-width, so they are not accounted
                    // in the words width and position calculation.
                    // Note: widths[] (obtained from lastFont->measureText)
                    // and the m_advance[] we build have cumulative widths
                    // (width[k] is the length of the rendered text from
                    // chars 0 to k included).
                    // Also handle space width scaling if requested.
                    bool scale_space_width = m_pbuffer->space_width_scale_percent != 100;
                    if ( scale_space_width && lastSrc ) { // but not if <pre>
                        if ( lastSrc->flags & LTEXT_FLAG_PREFORMATTED )
                            scale_space_width = false;
                    }
                    int cumulative_width_removed = 0;
                    int prev_orig_measured_width = 0;
                    int char_width = 0; // current single char width
                    for ( int k=0; k<len; k++ ) {
                        // printf("%c %x f=%d w=%d\n", m_text[start+k], m_text[start+k], flags[k], widths[k]);
                        char_width = widths[k] - prev_orig_measured_width;
                        prev_orig_measured_width = widths[k];
                        if ( m_flags[start + k] & LCHAR_IS_COLLAPSED_SPACE) {
                            cumulative_width_removed += char_width;
                            // make it zero width: same cumulative width as previous char's
                            widths[k] = k>0 ? widths[k-1] : 0;
                            flags[k] = 0; // remove SPACE/WRAP/... flags
                        }
                        else if ( flags[k] & LCHAR_IS_SPACE ) {
                            // LCHAR_IS_SPACE has just been guessed, and is available in flags[], not yet in m_flags[]
                            if ( scale_space_width ) {
                                int scaled_width = char_width * m_pbuffer->space_width_scale_percent / 100;
                                // We can just account for the space reduction (or increase) in cumulative_width_removed
                                cumulative_width_removed += char_width - scaled_width;
                            }
                            // remove, from the measured cumulative width, what we just, and previously, removed
                            widths[k] -= cumulative_width_removed;
                            if ( first_word_len >= 0 ) { // This is the space (or nbsp) after first word
                                bool keep_checking = false;
                                if ( first_word_len == 0 ) { // No word yet on the left
                                    // Leading space(s), probably no-break-space, which might be used
                                    // as indentation (ie. with poetry): don't allow their width to
                                    // be changed by text justification to keep similar lines aligned.
                                    // (Note: in RTL paragraphs, this would seem to not be needed, may
                                    // be because trailing spaces are part of the last word and won't
                                    // be expanded in alignLine().)
                                    flags[k] |= LCHAR_LOCKED_SPACING;
                                    keep_checking = true;
                                }
                                if ( first_word_len == 1 ) { // Previous word is a single char
                                    if ( k > 0 && isLeftPunctuation(m_text[k-1]) ) {
                                        // This space follows one of the common opening quotation marks or
                                        // dashes used to introduce a quotation or a part of a dialog:
                                        // https://en.wikipedia.org/wiki/Quotation_mark
                                        // Don't allow this space to change width, so text justification
                                        // doesn't move away next word, so that other similar paragraphs
                                        // get their real first words vertically aligned.
                                        flags[k] |= LCHAR_LOCKED_SPACING;
                                        // Also prevent that quotation mark or dash from getting
                                        // additional letter spacing for justification
                                        flags[k-1] |= LCHAR_LOCKED_SPACING;
                                        // If what's coming next is also such a char, continue doing that
                                        if ( k+1 < len && isLeftPunctuation(m_text[k+1]) ) {
                                            keep_checking = true;
                                        }
                                        //
                                        // Note: we do this check here, with the text still in logical
                                        // order, so we get that working with RTL text too (where, in
                                        // visual order, we'll have lost track of which word is the
                                        // first word - untested though).
                                    }
                                }
                                if ( keep_checking )
                                    first_word_len = 0;
                                else
                                    first_word_len = -1; // We don't need to deal with this anymore
                            }
                        }
                        else {
                            // remove, from the measured cumulative width, what we previously removed
                            widths[k] -= cumulative_width_removed;
                            if ( first_word_len >= 0 ) {
                                // Not a collapsed space and not a space: this will be part of first word
                                first_word_len++;
                            }
                            if ( m_has_cjk ) {
                                lChar32 ch = m_text[start+k];
                                if ( ch <= 0x201D && ch >= 0x2018 && (ch <= 0x2019 || ch >= 0x201C) ) {
                                    // Most CJK fonts provide a fullwidth glyph for U+2018/2019/201C/201D
                                    // LEFT/RIGHT SINGLE/DOUBLE QUOTATION MARK (we checked all the non-CJK
                                    // punctuation ranges with various CJK fonts, and found out only these
                                    // four get a fullwidth glyph.)
                                    // This is also dependant on the language/locl: a same font may give
                                    // them fullwidth for Chinese, but not for Japanese.
                                    // Try to guess if this is the case: most "Sans" CJK fonts don't make all
                                    // the glyphs have their width = 1em, so allow for a little less.
                                    if ( char_width >= lastFont->getSize() * 4/5 ) {
                                        // Consider this char as CJK, and as a flexible CJK char
                                        // if kerning is not disabled.
                                        m_flags[start+k] |= LCHAR_IS_CJK | (m_kerning_mode != KERNING_MODE_DISABLED ? LCHAR_IS_FLEXIBLE_WIDTH_CJK : 0);
                                    }
                                }
                                if ( m_pbuffer->cjk_width_scale_percent != 100 && m_flags[start+k] & LCHAR_IS_CJK && char_width > 0 ) {
                                    int added_width = char_width * m_pbuffer->cjk_width_scale_percent / 100 - char_width;
                                    widths[k] += added_width;
                                    cumulative_width_removed -= added_width; // (a negative cumulative_width_removed is cumulative width added)
                                }
                            }
                        }
                        m_advance[start + k] = lastWidth + widths[k];
                        #if (USE_LIBUNIBREAK==1)
                        // Reset these flags if lastFont->measureText() has set them, as we trust
                        // only libunibreak (which is more clever with hyphens, that our code flag
                        // with LCHAR_DEPRECATED_WRAP_AFTER).
                        flags[k] &= ~(LCHAR_ALLOW_WRAP_AFTER|LCHAR_DEPRECATED_WRAP_AFTER);
                        #endif
                        m_flags[start + k] |= flags[k];
                        // printf("  => w=%d\n", m_advance[start + k]);
                    }

                    /* If the following was ever needed, it was wrong to do it at this step
                     * of measureText(), as we then get additional fixed spacing that we may
                     * not need in some contexts. So don't do it: browsers do not.
                     * We'll handle that if LTEXT_FIT_GLYPHS when positioning words
                     * (not implemented for now.)

                    // This checks whether we're the last char of a text node, and if
                    // this node is italic, it adds the glyph italic overflow to the
                    // last char width.
                    // This might not be needed if the next text node is also italic,
                    // or if there is a space at start of next text node, and it might
                    // be needed at start of node too as the italic can overflow there too.
                    // It might also confuse our adjustment at start or end of line.
                    int dw = getAdditionalCharWidth(i-1, m_length);
                    if ( lastDirection < 0 ) // ignore it for RTL (as right side bearing is measured)
                        dw = 0;
                    if ( dw ) {
                        m_advance[i-1] += dw;
                        lastWidth += dw;
                    }
                    */

                    if (len > 0)
                        lastWidth += widths[len-1]; //len<m_length ? len : len-1];
                }
                else if ( measuring_object ) {
                    // We have start=i-1 and m_flags[i-1] & LCHAR_IS_OBJECT
                    if (start != i-1) {
                        crFatalError(126, "LCHAR_IS_OBJECT with start!=i-1");
                    }
                    if ( m_charindex[start] == FLOAT_CHAR_INDEX ) {
                        // Embedded floats can have a zero width in this process of
                        // text measurement. They'll be measured when positioned.
                        m_advance[start] = lastWidth;
                        // Don't touch first_word_len: we might want to ensure locked
                        // spacing on what's after.
                    }
                    else if ( m_charindex[start] == INLINEBOX_CHAR_INDEX ) {
                        // Render this inlineBox to get its width, similarly to how we
                        // render floats in addFloat(). See there for more comments.
                        src_text_fragment_t * src = m_srcs[start];
                        ldomNode * node = (ldomNode *) src->object;
                        bool already_rendered = false;
                        { // in its own scope, so this RenderRectAccessor is forgotten when left
                            RenderRectAccessor fmt( node );
                            if ( RENDER_RECT_HAS_FLAG(fmt, BOX_IS_RENDERED) ) {
                                already_rendered = true;
                            }
                        }
                        // Pre-compute render_w for ruby inline boxes: m_pbuffer->width equals
                        // the column height in vertical mode (large), so we can't use it as
                        // the available width for rendering a narrow ruby table.  Instead
                        // estimate max(base_depth, annot_depth) from char counts and font sizes.
                        bool vert_inline_box = (m_pbuffer->writing_mode == css_wm_vertical_rl ||
                                                m_pbuffer->writing_mode == css_wm_vertical_lr);
                        int advance_per_char_pre = lastFont ? lastFont->getSize()
                                                 : (m_pbuffer->strut_height > 0 ? m_pbuffer->strut_height : 1);
                        int base_char_count_pre = 0;
                        int annot_char_count_pre = 0;
                        int annot_font_size_pre = 0;
                        // Actual horizontal advance of base text (for Latin chars in vertical ruby).
                        // getCharWidth() returns the horizontal advance of each glyph; summing these
                        // gives the visual column depth when the word is rendered rotated 90°.
                        // For CJK chars getCharWidth ≈ font_size, so this is a no-op for CJK ruby.
                        int base_horiz_advance_pre = 0;
                        // Only apply ruby-specific logic when this inlineBox wraps a ruby table.
                        bool is_ruby_inline_pre = vert_inline_box
                            && node->getParentNode()
                            && node->getParentNode()->getStyle()->display == css_d_ruby
                            && node->getChildCount() > 0
                            && node->getChildNode(0)->getRendMethod() == erm_table;
                        if (is_ruby_inline_pre && advance_per_char_pre > 0) {
                            // Post-boxing structure:
                            //   inlineBox → rbox1(erm_table) → [rbox2_base(T="rbc"), rbox2_annot(T="rtc")]
                            // Both rbox2 elements have nodeId el_rubyBox, so we can't
                            // distinguish them by nodeId alone. Distinguish by first child:
                            //   rbox2_annot first child = el_rt/el_rtc → annotation row
                            //   rbox2_base first child  = el_rubyBox/el_rb → base row
                            ldomNode * rbox1_pre = node->getChildNode(0);
                            int cc_pre = rbox1_pre->getChildCount();
                            for (int ci = 0; ci < cc_pre; ci++) {
                                ldomNode * rbox2 = rbox1_pre->getChildNode(ci);
                                if (!rbox2 || !rbox2->isElement()) continue;
                                bool is_annot = false;
                                lUInt16 cid = rbox2->getNodeId();
                                if (cid == el_rt || cid == el_rp || cid == el_rtc) {
                                    is_annot = true;
                                } else if (rbox2->getChildCount() > 0) {
                                    ldomNode * fc = rbox2->getChildNode(0);
                                    if (fc && fc->isElement()) {
                                        lUInt16 fid = fc->getNodeId();
                                        is_annot = (fid == el_rt || fid == el_rtc || fid == el_rp);
                                    }
                                }
                                if (is_annot) {
                                    lString32 t = rbox2->getText();
                                    for (int k = 0; k < t.length(); k++)
                                        if (t[k] > 0x20) annot_char_count_pre++;
                                    if (annot_font_size_pre == 0) {
                                        ldomNode * rt = (cid == el_rt || cid == el_rtc) ? rbox2
                                                      : (rbox2->getChildCount() > 0 ? rbox2->getChildNode(0) : NULL);
                                        if (rt) {
                                            LVFontRef f = rt->getFont();
                                            if (!f.isNull()) annot_font_size_pre = f->getSize();
                                        }
                                    }
                                } else {
                                    lString32 t = rbox2->getText();
                                    LVFontRef base_font = rbox2->getFont();
                                    for (int k = 0; k < t.length(); k++) {
                                        lChar32 c = t[k];
                                        if (c > 0x20) {
                                            base_char_count_pre++;
                                            if (!base_font.isNull())
                                                base_horiz_advance_pre += base_font->getCharWidth(c);
                                        }
                                    }
                                }
                            }
                            if (base_char_count_pre < 1) base_char_count_pre = 1;
                            if (annot_font_size_pre == 0 && advance_per_char_pre > 1)
                                annot_font_size_pre = advance_per_char_pre / 2;
                        } else if (vert_inline_box && advance_per_char_pre > 0) {
                            base_char_count_pre = 1;
                        }
                        // Compute render_w here so it's available for both rendering and logging.
                        int render_w;
                        if (is_ruby_inline_pre && advance_per_char_pre > 0) {
                            int base_depth = base_char_count_pre * advance_per_char_pre;
                            int annot_depth = annot_char_count_pre * annot_font_size_pre;
                            render_w = annot_depth > base_depth ? annot_depth : base_depth;
                        } else {
                            render_w = m_pbuffer->width;
                        }
                        if ( !already_rendered ) {
                            LVRendPageContext alt_context( NULL, m_pbuffer->page_height, 0, false );
                            // inline-block and inline-table have a baseline, that renderBlockElement()
                            // will compute and give us back.
                            int baseline = REQ_BASELINE_FOR_INLINE_BLOCK;
                            if ( node->getChildNode(0)->getStyle()->display == css_d_inline_table ) {
                                baseline = REQ_BASELINE_FOR_TABLE;
                            }
                            else if ( node->getParentNode()->getStyle()->display == css_d_ruby
                                        && node->getChildNode(0)->getRendMethod() == erm_table ) {
                                // Ruby sub-tables don't carry css_d_inline_table, so check rend method;
                                // (a table could be in a "display: inline-block" container, and it
                                // would be erm_table - but we should still use REQ_BASELINE_FOR_INLINE_BLOCK,
                                // so check that the parent is really css_d_ruby)
                                baseline = REQ_BASELINE_FOR_TABLE;
                            }
                            // We render the inlineBox with the specified direction (from upper dir=), even
                            // if UNSET (and not with the direction determined by fribidi from the text).
                            // We provide 0,0 as the usable left/right overflows, so no glyph/hanging
                            // punctuation will leak outside the inlineBox (we might provide the widths
                            // of any blank space on either side, but here is too early as it might be
                            // shuffled by BiDi reordering.)
                            renderBlockElement( alt_context, node, 0, 0, render_w, 0, 0, m_specified_para_dir, &baseline );
                            // (renderBlockElement will ensure style->height if requested.)

                            // Note: this inline box we just rendered can have some overflow
                            // (i.e. if it has some negative margins). As these overflows are
                            // usually small, we'll handle that in LFormattedText::Draw() by
                            // just dropping the page rect clip when drawing it, so that the
                            // overflowing content might be drawn in the page margins.
                            // (Otherwise, we'd need to upgrade our frmline to store a line
                            // top and bottom overflows, use LTEXT_LINE_SPLIT_AVOID_BEFORE/AFTER
                            // to stick that line to previous or next, with the risk of bringing
                            // a large top margin to top of page just to display that small
                            // overflow in it...)

                            RenderRectAccessor fmt( node );
                            fmt.setBaseline(baseline);
                            RENDER_RECT_SET_FLAG(fmt, BOX_IS_RENDERED);
                            // We'll have alignLine() do the fmt.setX/Y once it is fully positioned

                            // Gather footnote links accumulated by alt_context
                            lString32Collection * link_ids = alt_context.getLinkIds();
                            if (link_ids->length() > 0) {
                                if ( m_pbuffer->inlineboxes_links == NULL ) {
                                    m_pbuffer->inlineboxes_links = new LVHashTable<lUInt32, lString32Collection*>(16);
                                }
                                lString32Collection * links;
                                lUInt32 key = node->getDataIndex();
                                if ( !m_pbuffer->inlineboxes_links->get(key, links) ) {
                                    links = new lString32Collection();
                                    m_pbuffer->inlineboxes_links->set(key, links);
                                }
                                for ( int n=0; n<link_ids->length(); n++ ) {
                                    links->add( link_ids->at(n) );
                                }
                            }
                        }
                        // (renderBlockElement() above may update our RenderRectAccessor(),
                        // so (re)get it only now)
                        RenderRectAccessor fmt( node );
                        int width = fmt.getWidth();
                        int height = fmt.getHeight();
                        int baseline = fmt.getBaseline();
                        // For vertical ruby with Latin base text, fmt.getWidth() is TTB-based
                        // (text formatter uses vertical TTB advances ≈ font_size per char).
                        // The visual column depth is the horizontal advance of the base text
                        // (the word is rendered rotated 90°), which is measured via getCharWidth().
                        // Override advance with the measured horizontal value so that:
                        //   o.width  → frmline layout uses visual depth (no phantom spacing)
                        //   letter_spacing → vert_min_next_x set to visual end of the word
                        // For CJK base text, getCharWidth ≈ font_size, so result is unchanged.
                        int advance;
                        if (is_ruby_inline_pre && vert_inline_box && base_horiz_advance_pre > 0) {
                            int annot_depth = annot_char_count_pre * annot_font_size_pre;
                            advance = base_horiz_advance_pre > annot_depth
                                    ? base_horiz_advance_pre : annot_depth;
                        } else {
                            advance = width;
                        }
                        m_srcs[start]->o.width = advance; // word->width uses o.width for frmline advance
                        m_srcs[start]->o.height = height;
                        m_srcs[start]->o.baseline = baseline;
                        // Store the visual column depth in letter_spacing so vert_min_next_x
                        // in Draw() is set to the actual visual end of the inline box.
                        if (is_ruby_inline_pre && vert_inline_box)
                            m_srcs[start]->letter_spacing = (lInt16)advance;
                        lastWidth += advance;
                        m_advance[start] = lastWidth;
                        // This object could be a small bullet, and we might want to ensure locked
                        // spacing on the following space - but it could also be a bigger image or
                        // a verbose inline box. Not really knowing that and what comes after,
                        // give up on ensuring locked spacing.
                        first_word_len = -1;
                    }
                    else if ( m_charindex[start] == IMAGE_CHAR_INDEX ) {
                        // measure image
                        // assume i==start+1
                        src_text_fragment_t * src = m_srcs[start];
                        ldomNode * node = (ldomNode *) src->object;
                        int width = 0;
                        int height = 0;
                        // We have yet no container height to provide for CSS heights in %,
                        // so they won't apply
                        getStyledImageSize( node, width, height, m_pbuffer->width, -1 );
                        // Ensure they are constrained to this paragraph width and page height
                        // Note: resizeImage() may do some additional scaling depending on image_scaling_options,
                        // use mode=0 scale=1 for these if this is not desirable.
                        if (!STYLE_HAS_CR_HINT(node->getStyle(), NO_CAP_IMAGE_SIZE))
                            resizeImage(width, height, m_pbuffer->width, m_max_img_height, m_length>1);
                        if ( (m_srcs[start]->flags & LTEXT_STRUT_CONFINED) && m_allow_strut_confining ) {
                            // Text with "-cr-hint: strut-confined" might just be vertically shifted,
                            // but won't change widths. But images who will change height must also
                            // have their width reduced to keep their aspect ratio.
                            if ( height > m_pbuffer->strut_height ) {
                                // Don't make image taller than initial strut height, so adjust width
                                // to keep aspect ratio.
                                width = width * m_pbuffer->strut_height / height;
                                height = m_pbuffer->strut_height;
                            }
                        }
                        // Store the computed image dimensions
                        m_srcs[start]->o.width = width;
                        m_srcs[start]->o.height = height;
                        lastWidth += width;
                        m_advance[start] = lastWidth;
                        /*
                        printf("measureText img: o.w=%d o.h=%d (max %d %d is_inline=%d) %s\n",
                            width, height, m_pbuffer->width, m_max_img_height, m_length>1,
                            UnicodeToLocal(ldomXPointer((ldomNode*)m_srcs[start]->object, 0).toString()).c_str());
                        */
                        first_word_len = -1; // As for INLINEBOX_CHAR_INDEX
                    }
                    else if ( m_charindex[start] == PAD_CHAR_INDEX ) {
                        // measure pad
                        src_text_fragment_t * src = m_srcs[start];
                        bool is_right_pad = src->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT;
                        ldomNode * node = (ldomNode *) src->object;
                        css_style_ref_t style = node->getStyle();
                        int base_width = m_pbuffer->width;
                        int margin, border, padding;
                        // is_right_pad actually means "logical right" (so, "end"). If bidi has made
                        // it RTL, it will be shown on the "left" side of a text segment, and so should
                        // use the left margin/border/padding values (hoping its left pad buddy will also
                        // be RTL and will rightly use the right margin values).
                        // (In the context of inline elements, margin/border/padding-inline-start/end
                        // would be more natural to use than -left/right - but it's a more recent CSS
                        // addition that we don't support.)
                        bool is_mirrored = lastDirection < 0;
                        if ( is_right_pad != is_mirrored ) { // unmirrored right pad, or mirrored left pad
                            // Use right margin/border/padding values
                            margin = lengthToPx( node, style->margin[1], base_width );
                            border = measureBorder(node, 1);
                            padding = lengthToPx( node, style->padding[1], base_width );
                            // Give up on locked spacing if it is a right pad
                            first_word_len = -1;
                        }
                        else {
                            // Use left margin/border/padding values
                            margin = lengthToPx( node, style->margin[0], base_width );
                            border = measureBorder(node, 3);
                            padding = lengthToPx( node, style->padding[0], base_width );
                            // Don't touch first_word_len: we might want to ensure locked
                            // spacing on the first space(s) following a left pad.
                        }
                        // No support for any negative value
                        if ( margin < 0 ) margin = 0;
                        if ( border < 0 ) border = 0;
                        if ( padding < 0 ) padding = 0;
                        // We store these computed values in the available fields
                        int width = margin + border + padding;
                        m_srcs[start]->o.width = width;             // the full width taken by this pad
                        m_srcs[start]->o.height = padding + border; // padding + border (background-color extends into this)
                        m_srcs[start]->o.baseline = border;         // border thickness (for drawing it)
                        lastWidth += width;
                        m_advance[start] = lastWidth;
                        // Update ALLOW_WRAP_AFTER flags (that we didn't do in copyText())
                        if ( start < m_length-1 && m_charindex[start+1] != PAD_CHAR_INDEX ) {
                            // We are the last of possibly multiple consecutive left/right pads.
                            // All previous ones did not get any allow_wrap set or unset, only
                            // the last one did (marking whether wrap between the char before
                            // all consecutive pads and the char after all of them is allowed).
                            if ( is_right_pad ) {
                                // Let this pad carry the one it got (if allowed, we can wrap
                                // after this right pad itself)
                            }
                            else { // left pad
                                // We can't wrap after a left pad
                                bool allow_wrap_after = m_flags[start] & LCHAR_ALLOW_WRAP_AFTER;
                                m_flags[start] &= ~LCHAR_ALLOW_WRAP_AFTER; // remove it
                                if ( !allow_wrap_after ) {
                                    // Handle an edge case: a followup leading space may get allow_wrap_after,
                                    // and it would look odd if we allowed a break after left-pad+space.
                                    // So consider it as if the left pad had it (this might need more work
                                    // if there are more spaces on the right, including collapsed spaces...)
                                    // Note that Firefox, when there are spaces at boundaries of consecutive
                                    // inline nodes with padding, just prevent any line break at the boundary.
                                    // This doesn't feel per-specs, and looks like some implementation side effect.
                                    if ( !(m_flags[start+1] & LCHAR_IS_OBJECT) && m_text[start+1] == ' ' ) {
                                        allow_wrap_after = m_flags[start+1] & LCHAR_ALLOW_WRAP_AFTER;
                                        m_flags[start+1] &= ~LCHAR_ALLOW_WRAP_AFTER; // remove it
                                    }
                                }
                                // Forward it to the nearest right-pad or char on the logical left
                                for ( int k=start-1; k >=0; k-- ) {
                                    if ( m_charindex[k] == PAD_CHAR_INDEX && !(m_srcs[k]->o.objflags & LTEXT_OBJECT_IS_PAD_RIGHT) ) {
                                        continue; // left-pad: look at the next on its left
                                    }
                                    if ( allow_wrap_after )
                                        m_flags[k] |= LCHAR_ALLOW_WRAP_AFTER;
                                    else
                                        m_flags[k] &= ~LCHAR_ALLOW_WRAP_AFTER;
                                    break;
                                }
                            }
                        }
                        else {
                            // We're not the last pad: ensure obvious forbidden breaks
                            if ( is_right_pad ) {
                                // Don't allow a break between previous char/pad and this right pad
                                if ( start > 0 ) {
                                    m_flags[start-1] &= ~LCHAR_ALLOW_WRAP_AFTER;
                                }
                            }
                            else {
                                // Don't allow a break after this left pad
                                m_flags[start] &= ~LCHAR_ALLOW_WRAP_AFTER;
                            }
                        }
                    }
                    else {
                        // Should not happen
                        crFatalError(129, "Attempting to measure unexpected object type");
                    }
                }
                else {
                    // Should not happen
                    crFatalError(127, "Attempting to measure Text node without a font");
                }
                start = i;
                #if (USE_HARFBUZZ==1)
                    prevScript = HB_SCRIPT_COMMON; // Reset as next segment can start with any script
                #endif
            }
            // Skip measuring chars to ignore.
            if ( m_flags[i] & LCHAR_IS_TO_IGNORE) {
                m_advance[start] = lastWidth;
                start++;
                // This whole function here is very convoluted, it could really
                // be made simpler and be more readable.
                // This simple test here feels out of place, but it seems to
                // work in the various cases (ignorable char at start, standalone,
                // multiples, or at end).
            }
            //
            if (newFont)
                lastFont = newFont;
            lastSrc = newSrc;
            lastLetterSpacing = newLetterSpacing;
            #if (USE_FRIBIDI==1)
                if (m_has_bidi)
                    lastBidiLevel = newBidiLevel;
            #endif
        }
        if ( tabIndex >= 0 && m_srcs[0]->indent < 0) {
            // Used by obsolete rendering of css_d_list_item_legacy when css_lsp_outside,
            // where the marker width is provided as negative/hanging indent.
            int tabPosition = -m_srcs[0]->indent; // has been set to marker_width
            if ( tabPosition>0 && tabPosition > m_advance[tabIndex] ) {
                int dx = tabPosition - m_advance[tabIndex];
                for ( i=tabIndex; i<m_length; i++ )
                    m_advance[i] += dx;
            }
        }
//        // debug dump
//        lString32 buf;
//        for ( int i=0; i<m_length; i++ ) {
//            buf << U" " << lChar32(m_text[i]) << U" " << lString32::itoa(m_advance[i]);
//        }
//        TR("%s", LCSTR(buf));
    }

    int getFlexibleCJKWidthAdjustment( int pos, int start, int end, bool &can_add_space_before, bool &can_add_space_after) {
        // Note: start and end represent the context: they can be the full (0, m_text) indices
        // when checking for start or end or paragraph, or the start and end of a line when checking
        // how flexible the char is at its position (possibly start or end) in the line.
        // (As in other functions, 'end' is exclusive)
        //
        // Reference: https://www.w3.org/TR/jlreq/#reduction_and_addition_of_intercharacter_space
        //
        // In https://www.w3.org/TR/jlreq/#character_sequences_which_do_not_allow_space_insertion_as_part_of_line_adjustment_processing
        // it is mentionned that we should not alter space between some character classes, and chars
        // we handle here are a subset of these classes. It is also said explicitely "the inseparable
        // character rule has to be applied to the following cases: Before or after... mostly all the
        // chars we flagged as LCHAR_IS_FLEXIBLE_WIDTH_CJK...
        // Testing with both can_add_space_before/after set to false on these doesn't really give
        // a satisfying result: some bits stay glued together, resulting in more space added to
        // other segments, and even more unbalance, and a lot of small different shifts in the grid.
        // It feels it would be better to add spaces before and after flexible chars (that will most
        // often end up staying fullwidth) as if expansion for text justification would be done on all
        // lines, this would ensure the "grid" is kept. But doing this in small paragraph widths (where
        // expansion for justification may add large spaces) would uglily spread out the punctuations
        // away from what they open/close...
        // Let's do something in between that feels intuitively better: prevent space addition near
        // what they open or close, and allow it on the other side (if not prevented by another
        // punctuation). This will still generates small shifts, but it's a lot less ugly.

        // This CJK char can have its nominal width modified.
        // What we can do depends on its type, context (start or end of line) and neighbour.
        cjk_type_t cjk_type = getCJKCharType(m_text[pos]);

        // cjkt_opening_bracket, unlike all other cjk_type_t, is to be checked against
        // the char that precedes it.
        if ( cjk_type == cjkt_opening_bracket ) {
            can_add_space_after = false; // keep it near what it opens
            // Find previous char index, skipping collapsed spaces
            cjk_type_t prev_type = cjkt_other;
            int prev = pos - 1;
            while ( prev >= start && m_flags[prev] & (LCHAR_IS_COLLAPSED_SPACE|LCHAR_IS_TO_IGNORE) )
                prev--;
            if ( prev < start ) { // no previous char
                prev_type = cjkt_start_of_line;
            }
            else {
                if ( (m_flags[prev] & LCHAR_IS_CJK) ) {
                    if ( (m_flags[prev] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) ) {
                        prev_type = getCJKCharType(m_text[prev]);
                    }
                }
                else { // Previous char is not CJK.
                    // It is not rare for CJK text to have mixed CJK and ASCII/Unicode punctuations
                    // (regular comma or period, Unicode single and double quotation marks...),
                    // so we need to check if the previous char is considered punctuation and
                    // masquerade it as a flexible CJK of the right type for the lookup.
                    lUInt16 prev_props = lGetCharProps(m_text[prev]);
                    if ( CH_PROP_IS_PUNCT(prev_props) ) {
                        if ( CH_PROP_IS_PUNCT_OPENING(prev_props) ) {
                            prev_type = cjkt_opening_bracket;
                        }
                        else if ( CH_PROP_IS_PUNCT_CLOSING(prev_props) ) {
                            prev_type = cjkt_closing_bracket;
                        }
                        else {
                            // Not sure if we should do more checks to map to some more similar
                            // catagories. For now, masquerade any other punctuation as a comma,
                            // which usually allows for large reduction.
                            // (This might not be welcome with Japanese when a fullstop is followed
                            // by U+2014 --, which would ensure no spacing between them...)
                            prev_type = cjkt_comma;
                        }
                    }
                }
            }
            return m_srcs[pos]->lang_cfg->getCJKWidthAdjustment(cjkt_opening_bracket, prev_type);
        }
        else {
            can_add_space_before = false; // keep it near what it follows
            // Find next char index, skipping collapsed spaces
            cjk_type_t next_type = cjkt_other;
            int next = pos + 1;
            while ( next < end && m_flags[next] & (LCHAR_IS_COLLAPSED_SPACE|LCHAR_IS_TO_IGNORE) )
                next++;
            if ( next >= end ) { // no next char ('end' is exclusive)
                next_type = cjkt_end_of_line;
            }
            else {
                if ( (m_flags[next] & LCHAR_IS_CJK) ) {
                    if ( (m_flags[next] & LCHAR_IS_FLEXIBLE_WIDTH_CJK) ) {
                        next_type = getCJKCharType(m_text[next]);
                    }
                }
                else { // Next char is not CJK.
                    lUInt16 next_props = lGetCharProps(m_text[next]);
                    if ( CH_PROP_IS_PUNCT(next_props) ) {
                        if ( CH_PROP_IS_PUNCT_OPENING(next_props) ) {
                            next_type = cjkt_opening_bracket;
                        }
                        else if ( CH_PROP_IS_PUNCT_CLOSING(next_props) ) {
                            next_type = cjkt_closing_bracket;
                        }
                        else {
                            next_type = cjkt_comma;
                        }
                    }
                }
                // In case we feel the spacing should be different whether the next
                // char is a CJK letter or a western letter/digit, we may add another
                // chk_type_t : cjkt_other_non_cjk and use it in the tables.
            }
            return m_srcs[pos]->lang_cfg->getCJKWidthAdjustment(cjk_type, next_type);
        }
    }

    /// split source data into paragraphs
    void splitParagraphs()
    {
        int start = 0;
        int i;

        int srctextlen = m_pbuffer->srctextlen;
        int clear_after_last_flag = 0;
        if ( srctextlen>0 && (m_pbuffer->srctext[srctextlen-1].flags & LTEXT_SRC_IS_CLEAR_LAST) ) {
            // Ignorable source line added to carry a last <br clear=>.
            clear_after_last_flag = m_pbuffer->srctext[srctextlen-1].flags & LTEXT_SRC_IS_CLEAR_BOTH;
            srctextlen -= 1; // Don't process this last srctext
        }

        for ( i=1; i<=srctextlen; i++ ) {
            // Split on LTEXT_FLAG_NEWLINE, mostly set when <BR/> met
            // (we check m_pbuffer->srctext[i], the next srctext that we are not
            // adding to the current paragraph, as <BR> and its clear= are carried
            // by the following text.)
            bool isLastPara = (i == srctextlen);
            if ( isLastPara || (m_pbuffer->srctext[i].flags & LTEXT_FLAG_NEWLINE) ) {
                if ( m_pbuffer->srctext[start].flags & LTEXT_SRC_IS_CLEAR_BOTH ) {
                    // (LTEXT_SRC_IS_CLEAR_BOTH is a mask, will match _LEFT and _RIGHT too)
                    floatClearText( m_pbuffer->srctext[start].flags & LTEXT_SRC_IS_CLEAR_BOTH );
                }
                // We do not need to go thru processParagraph*() to handle an embedded block
                // (bogus block element children of an inline element): we have a dedicated
                // handler for it.
                bool is_vertical = (css_wm_is_vertical(m_writing_mode));
                if ( i == start + 1 && m_pbuffer->srctext[start].flags & LTEXT_SRC_IS_OBJECT
                                    && m_pbuffer->srctext[start].o.objflags & LTEXT_OBJECT_IS_EMBEDDED_BLOCK ) {
                    // Embedded block among inlines had been surrounded by LTEXT_FLAG_NEWLINE,
                    // so we'll get one standalone here.
                    if ( is_vertical ) {
                        processEmbeddedBlockVertical( this, start );
                    }
                    else {
                        processEmbeddedBlockHorizontal( this, start );
                    }
                }
                else {
                    if ( is_vertical ) {
                        processParagraphVertical( this, start, i, isLastPara );
                    }
                    else {
                        processParagraphHorizontal( this, start, i, isLastPara );
                    }
                }
                start = i;
            }
        }
        if ( !m_no_clear_own_floats ) {
            // Clear our own floats so they are fully contained in this final block.
            finalizeFloats();
        }
        if ( clear_after_last_flag ) {
            floatClearText( clear_after_last_flag );
        }
    }

    void dealloc()
    {
        if ( !m_staticBufs ) {
            free( m_text );
            free( m_flags );
            free( m_srcs );
            free( m_charindex );
            free( m_advance );
            m_text = NULL;
            m_flags = NULL;
            m_srcs = NULL;
            m_charindex = NULL;
            m_advance = NULL;
            #if (USE_FRIBIDI==1)
                free( m_bidi_ctypes );
                free( m_bidi_btypes );
                free( m_bidi_levels );
                m_bidi_ctypes = NULL;
                m_bidi_btypes = NULL;
                m_bidi_levels = NULL;
            #endif
            m_staticBufs = true;
            // printf("freeing dynamic buffers\n");
        }
        else {
            m_staticBufs_inUse = false;
            // printf("releasing static buffers\n");
        }
    }

    /// format source data
    int format()
    {
        // split and process all paragraphs
        splitParagraphs();
        // cleanup
        dealloc();
        TR("format() finished: h=%d  lines=%d", m_line_advance, m_pbuffer->frmlinecount);
        return m_line_advance;
    }
};

bool LVFormatter::m_staticBufs_inUse = false;
#if (USE_LIBUNIBREAK==1)
bool LVFormatter::m_libunibreak_init_done = false;
#endif

static void freeFrmLines( formatted_text_fragment_t * m_pbuffer )
{
    // clear existing formatted data, if any
    if (m_pbuffer->frmlines)
    {
        for (int i=0; i<m_pbuffer->frmlinecount; i++)
        {
            lvtextFreeFormattedLine( m_pbuffer->frmlines[i] );
        }
        free( m_pbuffer->frmlines );
    }
    m_pbuffer->frmlines = NULL;
    m_pbuffer->frmlinecount = 0;

    // Also clear floats
    if (m_pbuffer->floats)
    {
        for (int i=0; i<m_pbuffer->floatcount; i++)
        {
            if (m_pbuffer->floats[i]->links) {
                delete m_pbuffer->floats[i]->links;
            }
            free( m_pbuffer->floats[i] );
        }
        free( m_pbuffer->floats );
    }
    m_pbuffer->floats = NULL;
    m_pbuffer->floatcount = 0;

    // Also clear inlinebox links containers
    if (m_pbuffer->inlineboxes_links)
    {
        LVHashTable<lUInt32, lString32Collection*>::iterator it = m_pbuffer->inlineboxes_links->forwardIterator();
        LVHashTable<lUInt32, lString32Collection*>::pair* pair;
        while ( (pair = it.next()) ) {
            delete pair->value;
        }
        free( m_pbuffer->inlineboxes_links );
    }
    m_pbuffer->inlineboxes_links = NULL;
}

// experimental formatter
lUInt32 LFormattedText::Format(lUInt16 width, lUInt16 page_height, int para_direction,
                int writing_mode, int usable_left_overflow, int usable_right_overflow, bool hanging_punctuation,
                BlockFloatFootprint * float_footprint)
{
    // clear existing formatted data, if any
    freeFrmLines( m_pbuffer );
    // setup new page size
    m_pbuffer->width = width;
    m_pbuffer->height = 0;
    m_pbuffer->page_height = page_height;
    m_pbuffer->is_reusable = !m_pbuffer->light_formatting;
    // format text
    LVFormatter formatter( m_pbuffer );

    // Set (as properties of the whole final block) the text-indent computed
    // values for the first line and for the next lines, by taking it
    // from the first src_text_fragment_t added (see comment in lvrend.cpp
    // renderFinalBlock() why we do it that way - while it might be better
    // if it were provided as a parameter to LFormattedText::Format()).
    int indent = m_pbuffer->srctextlen > 0 ? m_pbuffer->srctext[0].indent : 0;
    formatter.m_indent_first_line_done = false;
    if ( indent >= 0 ) { // positive indent affects only first line
        formatter.m_indent_current = indent;
        formatter.m_indent_after_first_line = 0;
    }
    else { // negative indent affects all but first lines
        formatter.m_indent_current = 0;
        formatter.m_indent_after_first_line = -indent;
    }

    // Set specified para direction (can be REND_DIRECTION_UNSET, in which case
    // it will be detected by fribidi)
    formatter.m_specified_para_dir = para_direction;

    // Set writing mode (horizontal-tb, vertical-rl, vertical-lr)
    formatter.m_writing_mode = writing_mode;
    m_pbuffer->writing_mode = (lInt16)writing_mode; // stored for Draw()


    formatter.m_usable_left_overflow = usable_left_overflow;
    formatter.m_usable_right_overflow = usable_right_overflow;
    formatter.m_hanging_punctuation = hanging_punctuation;

    if (float_footprint) {
        formatter.m_no_clear_own_floats = float_footprint->no_clear_own_floats;

        // BlockFloatFootprint provides a set of floats to represent
        // outer floats possibly having some footprint over the final
        // block that is to be formatted.
        // See FlowState->getFloatFootprint() for details.
        // So, for each of them, just add an embedded_float_t (without
        // a scrtext as they are not ours) to the buffer so our
        // positioning code can handle them.
        for (int i=0; i<float_footprint->floats_cnt; i++) {
            embedded_float_t * flt =  lvtextAddEmbeddedFloat( m_pbuffer );
            flt->srctext = NULL; // not our own float
            flt->x = float_footprint->floats[i][0];
            flt->y = float_footprint->floats[i][1];
            flt->width = float_footprint->floats[i][2];
            flt->height = float_footprint->floats[i][3];
            flt->is_right = (bool)(float_footprint->floats[i][4]);
            flt->inward_margin = float_footprint->floats[i][5];
        }
    }

    lUInt32 h = formatter.format();

    if ( float_footprint && float_footprint->no_clear_own_floats ) {
        // If we did not finalize/clear our embedded floats, forward
        // them to FlowState so it can ensure layout around them of
        // other block or final nodes.
        for (int i=0; i<m_pbuffer->floatcount; i++) {
            embedded_float_t * flt = m_pbuffer->floats[i];
            if (flt->srctext == NULL) // ignore outer floats given to us by flow
                continue;
            float_footprint->forwardOverflowingFloat(flt->x, flt->y, flt->width, flt->height,
                                        flt->is_right, (ldomNode *)flt->srctext->object);
        }
    }

    return h;
}

lString32Collection * LFormattedText::GetInlineBoxLinks( ldomNode * node ) {
    if ( m_pbuffer->inlineboxes_links ) {
        lString32Collection * links;
        if ( m_pbuffer->inlineboxes_links->get(node->getDataIndex(), links) ) {
            return links;
        }
    }
    return NULL;
}

void LFormattedText::setImageScalingOptions( img_scaling_options_t * options )
{
    m_pbuffer->img_zoom_in_mode_block = options->zoom_in_block.mode;
    m_pbuffer->img_zoom_in_scale_block = options->zoom_in_block.max_scale;
    m_pbuffer->img_zoom_in_mode_inline = options->zoom_in_inline.mode;
    m_pbuffer->img_zoom_in_scale_inline = options->zoom_in_inline.max_scale;
    m_pbuffer->img_zoom_out_mode_block = options->zoom_out_block.mode;
    m_pbuffer->img_zoom_out_scale_block = options->zoom_out_block.max_scale;
    m_pbuffer->img_zoom_out_mode_inline = options->zoom_out_inline.mode;
    m_pbuffer->img_zoom_out_scale_inline = options->zoom_out_inline.max_scale;
}

void LFormattedText::setSpaceWidthScalePercent(int spaceWidthScalePercent)
{
    if (spaceWidthScalePercent>=10 && spaceWidthScalePercent<=500)
        m_pbuffer->space_width_scale_percent = spaceWidthScalePercent;
}

void LFormattedText::setMinSpaceCondensingPercent(int minSpaceCondensingPercent)
{
    if (minSpaceCondensingPercent>=25 && minSpaceCondensingPercent<=100)
        m_pbuffer->min_space_condensing_percent = minSpaceCondensingPercent;
}

void LFormattedText::setUnusedSpaceThresholdPercent(int unusedSpaceThresholdPercent)
{
    if (unusedSpaceThresholdPercent>=0 && unusedSpaceThresholdPercent<=20)
        m_pbuffer->unused_space_threshold_percent = unusedSpaceThresholdPercent;
}

void LFormattedText::setMaxAddedLetterSpacingPercent(int maxAddedLetterSpacingPercent)
{
    if (maxAddedLetterSpacingPercent>=0 && maxAddedLetterSpacingPercent<=20)
        m_pbuffer->max_added_letter_spacing_percent = maxAddedLetterSpacingPercent;
}

void LFormattedText::setCJKWidthScalePercent(int cjkWidthScalePercent)
{
    if (cjkWidthScalePercent>=100 && cjkWidthScalePercent<=150)
        m_pbuffer->cjk_width_scale_percent = cjkWidthScalePercent;
}

/// set colors for selection and bookmarks
void LFormattedText::setHighlightOptions(text_highlight_options_t * v)
{
    m_pbuffer->highlight_options.selectionColor = v->selectionColor;
    m_pbuffer->highlight_options.commentColor = v->commentColor;
    m_pbuffer->highlight_options.correctionColor = v->correctionColor;
    m_pbuffer->highlight_options.bookmarkHighlightMode = v->bookmarkHighlightMode;
}


void DrawBookmarkTextUnderline(LVDrawBuf & drawbuf, int x0, int y0, int x1, int y1, int y, int flags, text_highlight_options_t * options) {
    if (!(flags & (4 | 8)))
        return;
    if (options->bookmarkHighlightMode == highlight_mode_none)
        return;
    bool isGray = drawbuf.GetBitsPerPixel() <= 8;
    lUInt32 cl = 0x000000;
    if (isGray) {
        if (options->bookmarkHighlightMode == highlight_mode_solid)
            cl = (flags & 4) ? 0xCCCCCC : 0xAAAAAA;
    } else {
        cl = (flags & 4) ? options->commentColor : options->correctionColor;
    }

    if (options->bookmarkHighlightMode == highlight_mode_solid) {
        // solid fill
        lUInt32 cl2 = (cl & 0xFFFFFF) | 0xA0000000;
        drawbuf.FillRect(x0, y0, x1, y1, cl2);
    }

    if (options->bookmarkHighlightMode == highlight_mode_underline) {
        // underline
        cl = (cl & 0xFFFFFF);
        lUInt32 cl2 = cl | 0x80000000;
        int step = 4;
        int index = 0;
        for (int x = x0; x < x1; x += step ) {

            int x2 = x + step;
            if (x2 > x1)
                x2 = x1;
            if (flags & 8) {
                // correction
                int yy = (index & 1) ? y - 1 : y;
                drawbuf.FillRect(x, yy-1, x+1, yy, cl2);
                drawbuf.FillRect(x+1, yy-1, x2-1, yy, cl);
                drawbuf.FillRect(x2-1, yy-1, x2, yy, cl2);
            } else if (flags & 4) {
                if (index & 1)
                    drawbuf.FillRect(x, y-1, x2 + 1, y, cl);
            }
            index++;
        }
    }
}

static void getAbsMarksFromMarks(ldomMarkedRangeList * marks, ldomMarkedRangeList * absmarks, ldomNode * node) {
    // Provided ldomMarkedRangeList * marks are ranges made from the words
    // of a selection currently being made (native highlights by crengine).
    // Their coordinates have been translated from absolute to relative
    // to the final node, by the DrawDocument() that called
    // LFormattedText::Draw() for this final node.
    // In LFormattedText::Draw(), when we need to call DrawDocument() to
    // draw floats or inlineBoxes, we need to translate them back to
    // absolute coordinates (DrawDocument() will translate them again
    // to relative coordinates in the drawn float or inlineBox).
    // (They are matched in LFormattedText::Draw() against the lineRect,
    // which have coordinates in the context of where we are drawing.)
    // The 'node' provided to this function must be a floatBox or inlineBox:
    // its parent is either the final node that contains them, or some
    // inline node contained in it.

    // We need to know the current final node that contains the provided
    // node, and its absolute coordinates
    ldomNode * final_node = node->getParentNode();
    for ( ; final_node; final_node = final_node->getParentNode() ) {
        int rm = final_node->getRendMethod();
        if ( rm == erm_final )
            break;
    }
    lvRect final_node_rect = lvRect();
    if ( final_node )
        final_node->getAbsRect( final_node_rect, true );

    // Fill the second provided ldomMarkedRangeList with marks in absolute
    // coordinates.
    for ( int i=0; i<marks->length(); i++ ) {
        ldomMarkedRange * mark = marks->get(i);
        ldomMarkedRange * newmark = new ldomMarkedRange( *mark );
        newmark->start.y += final_node_rect.top;
        newmark->end.y += final_node_rect.top;
        newmark->start.x += final_node_rect.left;
        newmark->end.x += final_node_rect.left;
            // (Note: early when developping this, NOT updating x gave the
            // expected results, although logically it should be updated...
            // But now, it seems to work, and is needed to correctly shift
            // highlight marks in inlineBox by the containing final block's
            // left margin...)
        absmarks->add(newmark);
    }
}

// bdidx is border index: Top=0, Right=1, Bottom=2, Left=3 ("TRouBLe")
static void drawBorder(LVDrawBuf * buf, int x0, int x1, int y, int h, ldomNode * borderNode, int bdidx) {
    css_style_ref_t style = borderNode->getStyle();
    css_length_t border_color = style->border_color[bdidx];
    lUInt32 bdcl = border_color.type == css_val_color ? // "currentcolor" if not
                        border_color.value : style->color.value;
    if ( !IS_COLOR_FULLY_TRANSPARENT(bdcl) ) {
        int border_width = measureBorder(borderNode, bdidx);
        css_border_style_type_t border_style;
        switch (bdidx){
            case 0: border_style = style->border_style_top;    break;
            case 1: border_style = style->border_style_right;  break;
            case 2: border_style = style->border_style_bottom; break;
            case 3: border_style = style->border_style_left;   break;
            default:
                    assert(0);
                    border_style = css_border_none;
                    break;
        }
        int dot, interval;
        switch (border_style){
            case css_border_dotted: dot = interval = border_width;     break;
            case css_border_dashed: dot = interval = 3 * border_width; break;
            default: dot = 1; interval = 0;                            break;
                // To keep things simple (vs the huge lvrend.cpp's DrawBorder()),
                // we handle every other style just as solid (no real need/room
                // to care for groove/ridge/inset/outset/double...)
        }
        if ( bdidx == 0 ) { // top border
            buf->DrawLine(x0, y, x1, y + border_width, bdcl, dot, interval, 0);
        }
        else if ( bdidx == 2 ) { // bottom border
            buf->DrawLine(x0, y + h - border_width, x1, y + h, bdcl, dot, interval, 0);
        }
        else { // left or right border
            buf->DrawLine(x0, y, x1, y + h, bdcl, dot, interval, 1);
        }
    }
}

#include "lvtextfm_layout_h.cpp"
#include "lvtextfm_layout_v.cpp"

#endif
