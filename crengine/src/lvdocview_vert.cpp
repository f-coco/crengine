// =============================================================================
// Fork-only LVDocView vertical-rl helpers.
//
// #included at the end of lvdocview.cpp so these methods belong to the
// same translation unit as LVDocView's other definitions and can access
// private members (m_doc, m_pages, m_dx, m_pageMargins).
//
// Created during the upstream-merge-friendliness pass to keep lvdocview.cpp
// closer to upstream.  Future upstream changes to LVDocView won't conflict
// with these methods since they live in a separate fork-only file.
// =============================================================================

/// Returns the screen-X anchor for the right edge of a vertical-rl page.
/// In vertical-rl mode, columns are anchored at clip.right = page_right.
/// page.height = N × strut ≤ page_width, so the remainder accumulates on the
/// left.  Distribute it equally by shifting the anchor inward by half the gap.
/// All three callers (drawPageTo, docToWindowPoint, windowToDocPoint) use this
/// function so the formula lives in exactly one place.
int LVDocView::vertPageRight( const lvRect & pageRect, int page_content_height ) const {
    int page_right = pageRect.right - m_pageMargins.right;
    int page_width = pageRect.width() - m_pageMargins.left - m_pageMargins.right;
    int centering_offset = (page_width - page_content_height) / 2;
    if ( centering_offset < 0 ) centering_offset = 0;
    return page_right - centering_offset;
}

/// Returns true if the document root body uses vertical-rl or vertical-lr.
/// Phase 1: checks the body element only.  Mixed-mode documents (Phase 2)
/// not yet supported.  Falls back to a page-height heuristic when the
/// style lookup fails (e.g. before first render).
bool LVDocView::isVerticalText() const {
    if (m_doc) {
        ldomNode * root = m_doc->getRootNode();
        if (root) {
            // Walk shallowly to find <body> (typically root → DocFragment → body).
            for (int depth = 0; depth < 4; depth++) {
                if (!root) break;
                if (root->isElement() && root->getNodeId() == el_body) {
                    css_style_ref_t style = root->getStyle();
                    if (!style.isNull()) {
                        int wm = style->writing_mode;
                        if (css_wm_is_vertical(wm)) {
                            return true;
                        }
                        if (wm == css_wm_horizontal_tb) {
                            return false;
                        }
                    }
                    break;
                }
                // Descend to first element child.
                ldomNode * next = NULL;
                int cnt = root->getChildCount();
                for (int i = 0; i < cnt; i++) {
                    ldomNode * c = root->getChildNode(i);
                    if (c && c->isElement()) { next = c; break; }
                }
                root = next;
            }
        }
    }
    // Fallback: scan pages for any with height ≤ m_dx + 32 (cover pages and
    // tall image pages don't qualify; vertical content pages do).
    if (m_pages.length() == 0) return false;
    for (int i = 0; i < m_pages.length(); i++) {
        int page_h = m_pages[i]->height;
        if (page_h > 0 && page_h <= m_dx + 32) {
            return true;
        }
    }
    return false;
}
