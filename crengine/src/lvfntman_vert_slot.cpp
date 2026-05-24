// =============================================================================
// Per-slot vertical-rl glyph-Y diagnostic + offset record machinery.
//
// Fork-only file: extracted from m-tky/crengine lvfntman.cpp on 2026-05-24 to
// reduce soft-fork divergence in lvfntman.cpp.  No upstream counterpart — the
// machinery is fork-only.
//
// State is file-local; lvfntman.cpp interacts with it only through:
//   - the public hooks declared in lvfntman.h (used by the formatter and
//     lvdocview.cpp for set/reset/lookup)
//   - lfnt_vert_record_glyph_draw(), the internal hook called once per
//     non-rotated vertical glyph draw from DrawTextString.
// =============================================================================

#include "../include/lvfntman.h"

// Vertical glyph-Y diagnostic.
// Records (gy − y) for each non-rotated vertical glyph draw.
// The consistent formula gy = y + (_baseline − origin_y) − y_offset gives a
// near-constant offset for all full-width CJK glyphs (≈ |descender|, 4-10 px).
// A formula that removes the (_baseline − origin_y) term (like gy = y − y_offset)
// gives avg ≈ 0 but HIGH VARIANCE because y_offset differs per character
// (e.g. punctuation vs kanji), producing irregular inter-character spacing.
// Tracks: sum for average, sum-of-squares for variance, min/max for spread.
// Reset via lfnt_reset_vert_gy_diag(); read via lfnt_get_vert_gy_diag().
int lfnt_vert_gy_count       = 0;
int lfnt_vert_gy_sum         = 0;
int lfnt_vert_gy_sum_sq      = 0;  // for variance: E[x^2] - E[x]^2
int lfnt_vert_gy_min         = 0x7fffffff;
int lfnt_vert_gy_max         = -0x7fffffff;

// Per-slot record of (column_anchor_estimate, slot_y, offset) for vertical-rl
// glyph draws.  Used by docToWindowPoint to align the highlight sbox with the
// actual glyph position per character, instead of using a single page-wide
// minimum (which leaves the sbox shifted upward by (per_glyph_offset − min)).
//
// Key matching is EXACT (no fuzzy window) — unmatched lookups fall back to the
// caller-provided default (the existing min), so behavior degrades gracefully
// for ruby annotations, rotated Latin words, and any other draw path that
// doesn't reach this record point.
#define LFNT_VERT_SLOT_MAX 2048
struct VertSlotRecord { int anchor; int slot_y; int offset; };
static VertSlotRecord lfnt_vert_slot_records[LFNT_VERT_SLOT_MAX];
int lfnt_vert_slot_count = 0;
// Set by the formatter (LFormattedText::Draw in lvtextfm.cpp) immediately before each
// DrawTextString call that draws into a vertical column.  Carries BOTH the
// column anchor (line_x in screen coords) and the slot_y KEY for record lookup
// purposes — the latter is the value docToWindowPoint will compute from the
// getRectEx-returned rect.left, which is based on word->x (the layout-stored
// inline position), NOT clamped_x (the per-draw running tracker possibly
// clamped to prevent overlap).  Using clamped_x as the record key causes
// lookups to miss whenever vert_min_next_x clamping has nudged a glyph past
// its layout position.  -1 = unset (skip recording).
static int lfnt_vert_current_anchor = -1;
static int lfnt_vert_current_slot_y_key = -1;

// Lookup hit/miss counters (diagnostic only).  Reset together with the slot
// records by lfnt_reset_vert_gy_diag().  Used by tests to verify that
// docToWindowPoint actually finds records (high hit ratio) instead of silently
// falling back to the page-wide min.
int lfnt_vert_lookup_hits   = 0;
int lfnt_vert_lookup_misses = 0;

// Diagnostic: count how many times set_current_anchor was called with a
// non-negative value.  Compared against lfnt_vert_slot_count, this isolates
// whether records are lost because the formatter never set the anchor
// (anchor_sets low) or because the draw path didn't reach the record point
// (anchor_sets high but slot_count low).
int lfnt_vert_anchor_sets = 0;
// Cumulative counter NEVER reset by lfnt_reset_vert_gy_diag().  Reveals how
// many vertical glyph draws happened over the test window even if the
// per-page reset wipes the slot records before the test reads them.
int lfnt_vert_lifetime_draws = 0;

// Forward declarations of the lookup-log statics (defined later) so the reset
// function can clear them without a circular declaration.
#define LFNT_VERT_LOOKUP_LOG_MAX 16
struct VertLookupLog { int anchor; int slot_y; int returned; bool hit; };
static VertLookupLog lfnt_vert_lookup_log[LFNT_VERT_LOOKUP_LOG_MAX];
static int lfnt_vert_lookup_log_count = 0;

void lfnt_vert_set_current_anchor(int anchor) {
    lfnt_vert_current_anchor = anchor;
    if (anchor >= 0) lfnt_vert_anchor_sets++;
}

void lfnt_vert_set_current_slot_y_key(int slot_y_key) {
    lfnt_vert_current_slot_y_key = slot_y_key;
}

void lfnt_reset_vert_gy_diag() {
    lfnt_vert_gy_count  = 0;
    lfnt_vert_gy_sum    = 0;
    lfnt_vert_gy_sum_sq = 0;
    lfnt_vert_gy_min    = 0x7fffffff;
    lfnt_vert_gy_max    = -0x7fffffff;
    lfnt_vert_slot_count = 0;
    lfnt_vert_current_anchor = -1;
    lfnt_vert_current_slot_y_key = -1;
    lfnt_vert_lookup_hits   = 0;
    lfnt_vert_lookup_misses = 0;
    lfnt_vert_anchor_sets   = 0;
    lfnt_vert_lookup_log_count = 0;
}

// Returns count, sum, sum_of_squares, min, max of (gy − y).
void lfnt_get_vert_gy_diag(int *count_out, int *sum_out, int *sum_sq_out,
                            int *min_out,   int *max_out) {
    *count_out  = lfnt_vert_gy_count;
    *sum_out    = lfnt_vert_gy_sum;
    *sum_sq_out = lfnt_vert_gy_sum_sq;
    *min_out    = lfnt_vert_gy_count > 0 ? lfnt_vert_gy_min : 0;
    *max_out    = lfnt_vert_gy_count > 0 ? lfnt_vert_gy_max : 0;
}

// Record a per-glyph offset.  Called from the vertical glyph draw path.
// `anchor` is the column anchor estimate (= x + _size; matches the column's
// screen-X = line_x in the formatter for plain text where strut == em == _size).
// `slot_y` is the glyph's slot top in screen coords (= y argument to DrawTextString
// for that glyph, before adding (_baseline − origin_y − y_offset)).
// `offset` is gy − slot_y.
static void lfnt_record_vert_slot(int anchor, int slot_y, int offset) {
    if (lfnt_vert_slot_count >= LFNT_VERT_SLOT_MAX)
        return;
    VertSlotRecord & r = lfnt_vert_slot_records[lfnt_vert_slot_count++];
    r.anchor = anchor;
    r.slot_y = slot_y;
    r.offset = offset;
}

// Look up the per-glyph offset for a given (column_anchor, slot_y).
// Returns the recorded offset on EXACT match; otherwise returns `fallback`.
// O(N) linear scan; N ≤ LFNT_VERT_SLOT_MAX, and lookup count per page is small
// (one per sbox corner), so total work is negligible.
int lfnt_lookup_vert_slot_offset(int anchor, int slot_y, int fallback, bool *hit_out) {
    int found_offset = fallback;
    bool hit = false;
    for (int i = 0; i < lfnt_vert_slot_count; i++) {
        const VertSlotRecord & r = lfnt_vert_slot_records[i];
        if (r.anchor == anchor && r.slot_y == slot_y) {
            found_offset = r.offset;
            hit = true;
            break;
        }
    }
    if (hit) lfnt_vert_lookup_hits++;
    else     lfnt_vert_lookup_misses++;
    if (lfnt_vert_lookup_log_count < LFNT_VERT_LOOKUP_LOG_MAX) {
        VertLookupLog & e = lfnt_vert_lookup_log[lfnt_vert_lookup_log_count++];
        e.anchor   = anchor;
        e.slot_y   = slot_y;
        e.returned = found_offset;
        e.hit      = hit;
    }
    if (hit_out) *hit_out = hit;
    return found_offset;
}

int lfnt_vert_lookup_log_size() { return lfnt_vert_lookup_log_count; }

bool lfnt_vert_get_lookup_log(int i, int *anchor_out, int *slot_y_out,
                              int *returned_out, bool *hit_out) {
    if (i < 0 || i >= lfnt_vert_lookup_log_count) return false;
    const VertLookupLog & e = lfnt_vert_lookup_log[i];
    if (anchor_out)   *anchor_out   = e.anchor;
    if (slot_y_out)   *slot_y_out   = e.slot_y;
    if (returned_out) *returned_out = e.returned;
    if (hit_out)      *hit_out      = e.hit;
    return true;
}

// Diagnostic accessors: return the i-th recorded slot, and lookup hit/miss
// counts.  Used by tests (see spec/unit/vertical_highlight_align_spec.lua) to
// verify the per-slot lookup mechanism actually hits during sbox computation.
int lfnt_vert_slot_record_count() {
    return lfnt_vert_slot_count;
}

bool lfnt_vert_get_slot_record(int i, int *anchor_out, int *slot_y_out, int *offset_out) {
    if (i < 0 || i >= lfnt_vert_slot_count) return false;
    const VertSlotRecord & r = lfnt_vert_slot_records[i];
    if (anchor_out) *anchor_out = r.anchor;
    if (slot_y_out) *slot_y_out = r.slot_y;
    if (offset_out) *offset_out = r.offset;
    return true;
}

void lfnt_vert_get_lookup_counts(int *hits_out, int *misses_out) {
    if (hits_out)   *hits_out   = lfnt_vert_lookup_hits;
    if (misses_out) *misses_out = lfnt_vert_lookup_misses;
}

int lfnt_vert_get_anchor_sets() {
    return lfnt_vert_anchor_sets;
}

int lfnt_vert_get_lifetime_draws() {
    return lfnt_vert_lifetime_draws;
}

// Internal hook called once per non-rotated vertical glyph draw from
// DrawTextString in lvfntman.cpp.  `gy` is the computed glyph-Y (slot top plus
// the per-glyph offset (_baseline − origin_y − y_offset)); `slot_top_y` is the
// untranslated slot top (y argument to DrawTextString for this glyph).  Folds
// the diagnostic accumulation and the per-slot record write into a single call
// so lvfntman.cpp does not need to read the file-local current_anchor /
// current_slot_y_key state.
void lfnt_vert_record_glyph_draw(int gy, int slot_top_y) {
    int d = gy - slot_top_y;
    lfnt_vert_gy_count++;
    lfnt_vert_gy_sum    += d;
    lfnt_vert_gy_sum_sq += d * d;
    if (d < lfnt_vert_gy_min) lfnt_vert_gy_min = d;
    if (d > lfnt_vert_gy_max) lfnt_vert_gy_max = d;
    lfnt_vert_lifetime_draws++;
    // Per-slot record for sbox alignment lookup.
    // anchor = line_x (column screen-X), and slot_y_key =
    // y_param + frmline->x + word->x in screen coords (matches
    // what docToWindowPoint will compute from rect.left).
    // Both are set by the formatter just before this call.
    // The recorded offset is gy − slot_y_key, which folds in
    // any clamp_delta (= clamped_x − word->x) plus the per-glyph
    // font offset (_baseline − origin_y − y_offset).
    if (lfnt_vert_current_anchor >= 0 && lfnt_vert_current_slot_y_key >= 0)
        lfnt_record_vert_slot(lfnt_vert_current_anchor,
                              lfnt_vert_current_slot_y_key,
                              gy - lfnt_vert_current_slot_y_key);
}
