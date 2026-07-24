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

/// Compute the 9 workspace tile rects for a monitor of `mw` x `mh` pixels.
/// Each tile preserves the monitor aspect ratio; the 3x3 grid is centered and
/// separated by `gap` (the big inter-workspace gaps), with an outer margin.
/// Writes 9 `Rect`s to `out`; returns the count.
///
/// # Safety
/// `out` must point to space for at least 9 `Rect`s.
#[no_mangle]
pub unsafe extern "C" fn waveview_workspace_tiles(mw: f64, mh: f64, out: *mut Rect) -> c_int {
    if mw <= 0.0 || mh <= 0.0 || out.is_null() {
        return 0;
    }
    let outer = mh * 0.006; // outer margin (tight — fill the screen)
    let gap = mh * 0.006; // gap between workspaces (the "imaginary lines")
    let aspect = mh / mw;

    // Cell width limited by both available width and height, so the 3x3 grid of
    // aspect-correct tiles always fits inside the monitor.
    let cw_by_w = (mw - 2.0 * outer - 2.0 * gap) / 3.0;
    let cw_by_h = ((mh - 2.0 * outer - 2.0 * gap) / 3.0) / aspect;
    let cell_w = cw_by_w.min(cw_by_h).max(1.0);
    let cell_h = cell_w * aspect;

    let grid_w = 3.0 * cell_w + 2.0 * gap;
    let grid_h = 3.0 * cell_h + 2.0 * gap;
    let left = (mw - grid_w) / 2.0;
    let top = (mh - grid_h) / 2.0;

    let tiles = std::slice::from_raw_parts_mut(out, 9);
    for row in 0..3 {
        for col in 0..3 {
            tiles[row * 3 + col] = Rect {
                x: left + col as f64 * (cell_w + gap),
                y: top + row as f64 * (cell_h + gap),
                w: cell_w,
                h: cell_h,
            };
        }
    }
    9
}

/// Which of the 9 tiles a workspace maps to, or -1 if it's outside the grid.
/// Workspaces 1..=9 map row-major to tiles 0..=8; specials/scratchpads (<= 0)
/// and anything beyond 9 are not shown. This is the one place that owns the
/// workspace→tile policy, so the mapping can grow (paged grids, per-monitor
/// sets) without touching the shim.
#[no_mangle]
pub extern "C" fn waveview_tile_for_workspace(ws_id: WorkspaceId) -> c_int {
    if (1..=9).contains(&ws_id) {
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
    fn tile_index_only_covers_1_through_9() {
        assert_eq!(waveview_tile_for_workspace(1), 0);
        assert_eq!(waveview_tile_for_workspace(9), 8);
        assert_eq!(waveview_tile_for_workspace(0), -1);
        assert_eq!(waveview_tile_for_workspace(10), -1);
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
    fn nine_aspect_correct_tiles_fit_inside_monitor() {
        let mut tiles = [Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 }; 9];
        let n = unsafe { waveview_workspace_tiles(1920.0, 1080.0, tiles.as_mut_ptr()) };
        assert_eq!(n, 9);
        let aspect = 1080.0 / 1920.0;
        for t in &tiles {
            assert!(t.x >= 0.0 && t.y >= 0.0);
            assert!(t.x + t.w <= 1920.0 + 1e-6 && t.y + t.h <= 1080.0 + 1e-6);
            assert!((t.h / t.w - aspect).abs() < 1e-6); // each tile mirrors monitor aspect
        }
    }
}
