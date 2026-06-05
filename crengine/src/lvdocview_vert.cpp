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
/// In vertical-rl mode, columns are anchored at page_right: the first column
/// sits at the right margin and subsequent columns progress leftward.  Any
/// unused column space on a partial page (e.g. a chapter's final page with
/// only a few short columns) MUST accumulate on the LEFT — this is the
/// vertical-rl analogue of horizontal text leaving blank space at the bottom
/// of a short last page.  We do NOT centre the content: an earlier version
/// shifted the anchor inward by half the unused width, which made a chapter's
/// last few columns float in the middle of the page instead of starting at
/// the right.  All three callers (drawPageTo, docToWindowPoint,
/// windowToDocPoint) use this function so the anchor stays consistent between
/// rendering and coordinate conversion.
int LVDocView::vertPageRight( const lvRect & pageRect, int page_content_height ) const {
    (void)page_content_height; // no centering: always anchor at the right edge
    return pageRect.right - m_pageMargins.right;
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

/// Vertical-rl doc → window for SCROLL view mode.
///   doc.y (column position) → screen.x = page_right − (doc.y − _pos)
///   doc.x (in-column pos)   → screen.y = doc.x
/// _pos is the doc-y at the viewport's right edge (entry point for forward
/// reading), kept in lockstep with corner-scroll's `_gotoPos(currentPos +
/// screen_w/3)` advances.  Mirrors PAGE-mode docToWindowPoint's vertical-rl
/// branch but with _pos in place of m_pages[page]->start and a single global
/// page_right anchor instead of per-page m_pageRects.
bool LVDocView::docToWindowPointScrollVert( lvPoint & pt ) const {
    int page_right = m_dx - m_pageMargins.right;
    int screen_x = page_right - (pt.y - _pos);
    int screen_y = pt.x;
    pt.x = screen_x;
    pt.y = screen_y;
    return true;
}

/// Reverse of docToWindowPointScrollVert.  pullInPageArea clamps screen.x to
/// the visible horizontal band (margin.left … page_right − 1) so taps in the
/// outer margins still resolve to the nearest in-page column, matching the
/// upstream SCROLL-mode pullInPageArea behaviour for horizontal text.
bool LVDocView::windowToDocPointScrollVert( lvPoint & pt, bool pullInPageArea ) const {
    int page_right = m_dx - m_pageMargins.right;
    if ( pullInPageArea ) {
        if ( pt.x < m_pageMargins.left )
            pt.x = m_pageMargins.left;
        if ( pt.x >= page_right )
            pt.x = page_right - 1;
    }
    int doc_y = page_right - pt.x + _pos;
    int doc_x = pt.y;
    pt.x = doc_x;
    pt.y = doc_y;
    return true;
}
