//! waveview brain — pure-logic core of the overview: 3x3 grid layout, tiling,
//! hit-testing, animation. No Hyprland here; the C++ shim feeds geometry and
//! calls back to render/move. Flat C ABI boundary.

use std::os::raw::{c_char, c_int};

/// A rectangle in monitor **pixel** coordinates.
#[repr(C)]
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
    let outer = mh * 0.015; // outer margin (tight — fill the screen)
    let gap = mh * 0.012; // gap between workspaces (the "imaginary lines")
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
