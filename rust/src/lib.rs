//! waveview brain — pure-logic core of the overview: 3x3 grid layout, tiling,
//! hit-testing, animation. No Hyprland here; the C++ shim feeds geometry and
//! calls back to render/move. Flat C ABI boundary.

use std::os::raw::{c_char, c_int};

/// Hyprland's `WORKSPACEID` is `int64_t`.
type WorkspaceId = i64;

/// A rectangle in monitor **pixel** coordinates.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Rect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
}

#[no_mangle]
pub extern "C" fn waveview_hello() -> *const c_char {
    b"brain online\0".as_ptr() as *const c_char
}

/// Compute the 9 workspace tile rects for a monitor of `mw` x `mh` pixels,
/// with the usable area starting at `top` (below the OPTIONS bar plus its
/// breathing gap — the shim passes reserved strip + 3 logical px).
///
/// Number of overview workspaces: a 3x6 grid, the lower rows revealed by
/// scrolling (the shim owns the scroll offset; tiles here are unscrolled).
pub const N_TILES: usize = 18;

/// FULL-BLEED scrollable layout (per Max): 3 columns x 6 rows. The first
/// three rows fill the usable area edge to edge with ONE `gap` everywhere —
/// between tiles and as the left/right/bottom margins; the top margin is
/// `top` itself (bar + 3 px). Rows 4-6 continue below the fold at the same
/// rhythm; the shim scrolls them into view.
///
/// Tiles give up exact monitor aspect (cell width fills the width, cell
/// height makes three rows fill the height — ~2-3% flatter on a barred
/// monitor). Window minis stay correctly placed (the mapper scales x and y
/// independently) and the close-zoom uses separate x/y scales, so the give
/// is invisible in motion.
///
/// Writes [`N_TILES`] `Rect`s to `out`; returns the count.
///
/// # Safety
/// `out` must point to space for at least [`N_TILES`] `Rect`s.
#[no_mangle]
pub unsafe extern "C" fn waveview_workspace_tiles(mw: f64, mh: f64, top: f64, out: *mut Rect) -> c_int {
    if mw <= 0.0 || mh <= 0.0 || !(0.0..mh).contains(&top) || out.is_null() {
        return 0;
    }
    let gap = mh * 0.006; // the one gap (inter-tile AND outer margins)
    let avail_h = mh - top;

    // Fill both axes exactly with the VISIBLE 3 rows: 2 side margins +
    // 2 inner gaps across, 2 inner gaps + 1 bottom margin down (the top
    // margin is `top` itself). Rows 4-6 continue the same rhythm below.
    let cell_w = ((mw - 4.0 * gap) / 3.0).max(1.0);
    let cell_h = ((avail_h - 3.0 * gap) / 3.0).max(1.0);

    let tiles = std::slice::from_raw_parts_mut(out, N_TILES);
    for row in 0..6 {
        for col in 0..3 {
            tiles[row * 3 + col] = Rect {
                x: gap + col as f64 * (cell_w + gap),
                y: top + row as f64 * (cell_h + gap),
                w: cell_w,
                h: cell_h,
            };
        }
    }
    N_TILES as c_int
}

/// Which of the [`N_TILES`] tiles a workspace maps to, or -1 if it's outside
/// the grid. Workspaces 1..=18 map row-major to tiles 0..=17; specials /
/// scratchpads (<= 0) and anything beyond are not shown. This is the one
/// place that owns the workspace→tile policy, so the mapping can grow
/// (paged grids, per-monitor sets) without touching the shim.
#[no_mangle]
pub extern "C" fn waveview_tile_for_workspace(ws_id: WorkspaceId) -> c_int {
    if (1..=N_TILES as WorkspaceId).contains(&ws_id) {
        (ws_id - 1) as c_int
    } else {
        -1
    }
}

/// Map a window into its workspace tile, clipped to the tile's bounds.
///
/// The tile (`t*`) is in monitor **pixel** space; the window (`w*`) and monitor
/// origin/size (`mon_*`) are in **logical** layout coords — the mismatch is
/// deliberate and resolved by the `tile_size / monitor_logical_size` ratio, so
/// a window's placement within its tile is scale-independent. Writes the
/// resulting pixel-space mini-rect to `out`; a fully-clipped window yields a
/// zero-size rect (the shim skips those).
///
/// # Safety
/// `out` must point to a valid `Rect`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn waveview_map_window(
    tx: f64, ty: f64, tw: f64, th: f64,
    mon_x: f64, mon_y: f64, mon_w: f64, mon_h: f64,
    wx: f64, wy: f64, ww: f64, wh: f64,
    out: *mut Rect,
) {
    if out.is_null() {
        return;
    }
    if mon_w <= 0.0 || mon_h <= 0.0 {
        *out = Rect { x: tx, y: ty, w: 0.0, h: 0.0 };
        return;
    }
    let sx = tw / mon_w;
    let sy = th / mon_h;

    // Window corners projected into tile space, then clipped to the tile so a
    // window straddling the monitor edge can't bleed into a neighbour tile.
    let x0 = (tx + (wx - mon_x) * sx).clamp(tx, tx + tw);
    let y0 = (ty + (wy - mon_y) * sy).clamp(ty, ty + th);
    let x1 = (tx + (wx + ww - mon_x) * sx).clamp(tx, tx + tw);
    let y1 = (ty + (wy + wh - mon_y) * sy).clamp(ty, ty + th);

    *out = Rect { x: x0, y: y0, w: (x1 - x0).max(0.0), h: (y1 - y0).max(0.0) };
}

#[cfg(test)]
mod tests {
    use super::*;

    fn map(tile: (f64, f64, f64, f64), mon: (f64, f64, f64, f64), win: (f64, f64, f64, f64)) -> Rect {
        let mut out = Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 };
        unsafe {
            waveview_map_window(
                tile.0, tile.1, tile.2, tile.3, mon.0, mon.1, mon.2, mon.3, win.0, win.1, win.2, win.3, &mut out,
            );
        }
        out
    }

    #[test]
    fn tile_index_only_covers_1_through_18() {
        assert_eq!(waveview_tile_for_workspace(1), 0);
        assert_eq!(waveview_tile_for_workspace(9), 8);
        assert_eq!(waveview_tile_for_workspace(18), 17);
        assert_eq!(waveview_tile_for_workspace(0), -1);
        assert_eq!(waveview_tile_for_workspace(19), -1);
        assert_eq!(waveview_tile_for_workspace(-99), -1); // scratchpad/special
    }

    #[test]
    fn window_scales_into_tile_by_size_ratio() {
        // Monitor 1000x500 logical at origin; tile 100x50 → 1/10 scale.
        // A 200x100 window at (300,150) → 20x10 at tile-local (30,15).
        let r = map((10.0, 20.0, 100.0, 50.0), (0.0, 0.0, 1000.0, 500.0), (300.0, 150.0, 200.0, 100.0));
        assert!((r.x - 40.0).abs() < 1e-9); // 10 + 300*0.1
        assert!((r.y - 35.0).abs() < 1e-9); // 20 + 150*0.1
        assert!((r.w - 20.0).abs() < 1e-9);
        assert!((r.h - 10.0).abs() < 1e-9);
    }

    #[test]
    fn monitor_origin_offset_is_subtracted() {
        // Monitor at logical origin (1920,0): a window flush to the monitor's
        // top-left sits at the tile's top-left, not offset by the global origin.
        let r = map((0.0, 0.0, 96.0, 54.0), (1920.0, 0.0, 1920.0, 1080.0), (1920.0, 0.0, 1920.0, 1080.0));
        assert!(r.x.abs() < 1e-9 && r.y.abs() < 1e-9);
        assert!((r.w - 96.0).abs() < 1e-9 && (r.h - 54.0).abs() < 1e-9);
    }

    #[test]
    fn window_past_monitor_edge_is_clipped_to_tile() {
        // Window overhanging the right/bottom edge must not bleed past the tile.
        let r = map((0.0, 0.0, 100.0, 100.0), (0.0, 0.0, 1000.0, 1000.0), (900.0, 900.0, 400.0, 400.0));
        assert!((r.x - 90.0).abs() < 1e-9 && (r.y - 90.0).abs() < 1e-9);
        assert!((r.w - 10.0).abs() < 1e-9 && (r.h - 10.0).abs() < 1e-9); // clamped, not 40
    }

    #[test]
    fn fully_offscreen_window_yields_zero_size() {
        let r = map((0.0, 0.0, 100.0, 100.0), (0.0, 0.0, 1000.0, 1000.0), (2000.0, 2000.0, 100.0, 100.0));
        assert_eq!(r.w, 0.0);
        assert_eq!(r.h, 0.0);
    }

    #[test]
    fn degenerate_monitor_size_is_safe() {
        let r = map((5.0, 6.0, 100.0, 100.0), (0.0, 0.0, 0.0, 0.0), (10.0, 10.0, 10.0, 10.0));
        assert_eq!((r.x, r.y, r.w, r.h), (5.0, 6.0, 0.0, 0.0));
    }

    #[test]
    fn full_bleed_grid_fills_and_gaps_are_uniform() {
        let mut tiles = [Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 }; N_TILES];
        let (mw, mh, top) = (1920.0, 1080.0, 48.0);
        let n = unsafe { waveview_workspace_tiles(mw, mh, top, tiles.as_mut_ptr()) };
        assert_eq!(n, N_TILES as c_int);
        let gap = mh * 0.006;
        // Top-anchored exactly at `top` (the 3 px promise to the bar).
        assert!((tiles[0].y - top).abs() < 1e-9);
        // Side and bottom margins equal the inter-tile gap — full bleed for
        // the visible 3 rows (row 3 ends one gap above the bottom edge).
        assert!((tiles[0].x - gap).abs() < 1e-9);
        assert!((mw - (tiles[2].x + tiles[2].w) - gap).abs() < 1e-6);
        assert!((mh - (tiles[8].y + tiles[8].h) - gap).abs() < 1e-6);
        // Inner seams both equal the same gap, and rows 4-6 continue the
        // exact rhythm below the fold.
        let inner_x = tiles[1].x - (tiles[0].x + tiles[0].w);
        let inner_y = tiles[3].y - (tiles[0].y + tiles[0].h);
        assert!((inner_x - gap).abs() < 1e-6 && (inner_y - gap).abs() < 1e-6);
        let row_step = tiles[3].y - tiles[0].y;
        for r in 1..6 {
            assert!((tiles[r * 3].y - tiles[0].y - r as f64 * row_step).abs() < 1e-6);
        }
        assert!(tiles[17].y + tiles[17].h > mh); // below the fold, scroll reveals
    }
}
