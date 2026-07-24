//! waveview brain — the pure-logic core of the overview: 3x3 grid layout,
//! per-workspace tiling, drag hit-testing, and the split-reaction animation.
//! No Hyprland/Wayland here — the C++ shim feeds it geometry + input and calls
//! back to render/move. std-only; the boundary is a flat C ABI.

use std::os::raw::{c_char, c_int};

/// Overview grid dimensions (3 columns x 3 rows = 9 workspaces).
#[no_mangle]
pub extern "C" fn waveview_grid_cols() -> c_int {
    3
}
#[no_mangle]
pub extern "C" fn waveview_grid_rows() -> c_int {
    3
}

/// Proof-of-linkage greeting (static NUL-terminated string).
#[no_mangle]
pub extern "C" fn waveview_hello() -> *const c_char {
    b"brain online\0".as_ptr() as *const c_char
}
