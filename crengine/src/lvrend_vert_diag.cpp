// =============================================================================
// Ruby-table vertical-detection diagnostic counters.
//
// Fork-only file: extracted from m-tky/crengine lvrend.cpp during Phase D
// Step A1 to reduce soft-fork divergence in lvrend.cpp.  No upstream
// counterpart — these counters track a vertical-rl-specific code path.
//
// vert_ok:   ruby table correctly identified as vertical (vert_ruby=true).
// vert_miss: ruby table NOT identified as vertical (vert_ruby=false) yet
//            has cells with non-zero col->x — the bug that causes Y
//            displacement.
// col_x_max: the largest col->x used when vert_miss > 0.
//
// Populated by CCRTable::renderCells() in lvrend.cpp via extern int
// references.  Exposed to cre.cpp (Lua side) through
// lvrend_reset_ruby_diag() / lvrend_get_ruby_diag().
// =============================================================================

int s_ruby_vert_ok   = 0;
int s_ruby_vert_miss = 0;
int s_ruby_col_x_max = 0;
int s_list_marker_vert_ok = 0;
int s_list_marker_vert_miss = 0;

void lvrend_reset_ruby_diag() {
    s_ruby_vert_ok = s_ruby_vert_miss = s_ruby_col_x_max = 0;
}

void lvrend_get_ruby_diag(int *ok_out, int *miss_out, int *col_x_max_out) {
    *ok_out        = s_ruby_vert_ok;
    *miss_out      = s_ruby_vert_miss;
    *col_x_max_out = s_ruby_col_x_max;
}

void lvrend_reset_list_marker_diag() {
    s_list_marker_vert_ok = 0;
    s_list_marker_vert_miss = 0;
}

void lvrend_get_list_marker_diag(int *ok_out, int *miss_out) {
    *ok_out = s_list_marker_vert_ok;
    *miss_out = s_list_marker_vert_miss;
}
