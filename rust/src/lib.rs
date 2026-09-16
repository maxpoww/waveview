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

/// Mockup-blessed layout (Max's design, ~/overview-mockup settings
/// 2026-08-30): 3 columns x 6 rows, the visible 3 rows filling the usable
/// area with `outer` side/bottom margins and `gap` between tiles; the top
/// margin is `top` itself (bar + top-gap). Rows 4-6 continue below the
/// fold at the same rhythm; the shim scrolls them in page flips.
///
/// All distances arrive in draw-space px (the shim scales the logical
/// design values: gap 12, outer 35, top-gap 12).
///
/// Writes [`N_TILES`] `Rect`s to `out`; returns the count.
///
/// # Safety
/// `out` must point to space for at least [`N_TILES`] `Rect`s.
#[no_mangle]
pub unsafe extern "C" fn waveview_workspace_tiles(
    mw: f64,
    mh: f64,
    top: f64,
    gap: f64,
    outer: f64,
    out: *mut Rect,
) -> c_int {
    if mw <= 0.0 || mh <= 0.0 || !(0.0..mh).contains(&top) || gap < 0.0 || outer < 0.0 || out.is_null() {
        return 0;
    }
    let avail_h = mh - top;

    // Visible 3 rows: 2 side margins + 2 inner gaps across; 2 inner gaps +
    // 1 bottom `outer` margin down (the top margin is `top` itself).
    let cell_w = ((mw - 2.0 * outer - 2.0 * gap) / 3.0).max(1.0);
    let cell_h = ((avail_h - outer - 2.0 * gap) / 3.0).max(1.0);

    let tiles = std::slice::from_raw_parts_mut(out, N_TILES);
    for row in 0..6 {
        for col in 0..3 {
            tiles[row * 3 + col] = Rect {
                x: outer + col as f64 * (cell_w + gap),
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

// ---- The spread (macOS-style per-workspace exposé) -------------------------
//
// Level 1 of the reveal ladder: when windows on the current workspace hide
// each other, the first swipe spreads THEM apart (this layout); the second
// swipe escalates to the overview. The same layout will later feed the
// overview's tiles (an overlapped workspace renders pre-spread), which is why
// it lives here in logical workspace space: one layout, two projections.

/// A hair of intersection is a windowing accident, not concealment — but any
/// honest overlap means pixels the user cannot see.
const OVERLAP_EPS: f64 = 1.0;

fn pair_overlap(a: &Rect, b: &Rect, pad: f64) -> (f64, f64) {
    let ox = (a.x + a.w).min(b.x + b.w) - a.x.max(b.x) + pad;
    let oy = (a.y + a.h).min(b.y + b.h) - a.y.max(b.y) + pad;
    (ox, oy)
}

fn any_overlap(rects: &[Rect], pad: f64, eps: f64) -> bool {
    for i in 0..rects.len() {
        for j in i + 1..rects.len() {
            let (ox, oy) = pair_overlap(&rects[i], &rects[j], pad);
            if ox > eps && oy > eps {
                return true;
            }
        }
    }
    false
}

/// Does any window on this view meaningfully hide part of another?
/// The spread rung exists exactly when this says so; tiled layouts (edges
/// kiss, never cross) and lone windows answer no.
///
/// # Safety
/// `wins` must point to `n` valid `Rect`s.
#[no_mangle]
pub unsafe extern "C" fn waveview_needs_spread(wins: *const Rect, n: c_int) -> c_int {
    if wins.is_null() || n < 2 {
        return 0;
    }
    let rects = std::slice::from_raw_parts(wins, n as usize);
    any_overlap(rects, 0.0, OVERLAP_EPS) as c_int
}

fn clamp_into(r: &mut Rect, ax: f64, ay: f64, aw: f64, ah: f64) {
    // A rect wider than the area centres in it (the scale loop will shrink it
    // out of everyone's way eventually).
    r.x = if r.w >= aw { ax + (aw - r.w) / 2.0 } else { r.x.clamp(ax, ax + aw - r.w) };
    r.y = if r.h >= ah { ay + (ah - r.h) / 2.0 } else { r.y.clamp(ay, ay + ah - r.h) };
}

/// The flock centres as ONE: after separation the whole group's bounding box
/// is translated to the middle of the area, so relative geometry (the
/// spatial memory) rides along untouched while the composition balances. A
/// bottom-heavy desktop must not spread bottom-heavy (Max, 2026-09-16:
/// "more space being used on bottom than on the top").
fn recentre(rects: &mut [Rect], ax: f64, ay: f64, aw: f64, ah: f64) {
    let (mut x0, mut y0) = (f64::MAX, f64::MAX);
    let (mut x1, mut y1) = (f64::MIN, f64::MIN);
    for r in rects.iter() {
        x0 = x0.min(r.x);
        y0 = y0.min(r.y);
        x1 = x1.max(r.x + r.w);
        y1 = y1.max(r.y + r.h);
    }
    if x1 <= x0 || y1 <= y0 {
        return;
    }
    let dx = (ax + (aw - (x1 - x0)) / 2.0) - x0;
    let dy = (ay + (ah - (y1 - y0)) / 2.0) - y0;
    for r in rects.iter_mut() {
        r.x += dx;
        r.y += dy;
        clamp_into(r, ax, ay, aw, ah); // no-op when the group fits (it does whenever every rect was inside)
    }
}

/// Spread `n` window rects into a JUSTIFIED COLLAGE of the usable area —
/// the photo-wall algorithm, built rather than simulated:
///
/// - already-separate input comes back untouched (identity);
/// - an ALMOST-revealed desk gets a local NUDGE (tier 1): only the windows
///   party to an overlap slide apart, a bounded few dozen px, full size, no
///   recentre — a slight overlap costs a slight move, never a re-layout;
/// - reading order survives (top-to-bottom, left-to-right by centre): what
///   was upper-left lands upper-left;
/// - every window is measured; rows are chosen by balancing aspect sums
///   (linear-partition DP over every row count) and each row is JUSTIFIED —
///   all its cards share the row height, and that height is exactly what
///   fills the full width. Scale is per-window (a small window shrinks less;
///   one huge window no longer starves everyone), capped at real size;
/// - if the stacked rows overflow the area, everything shrinks by one
///   common factor; rows and the whole flock end centred.
///
/// Aligned edges, exact gaps, full-width rows: decent by CONSTRUCTION.
/// Returns 1 with a fully separated layout for any valid input (0 only on
/// degenerate arguments). Deterministic in the input.
///
/// # Safety
/// `wins` must point to `n` valid `Rect`s and `out` to space for `n` more.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn waveview_spread_layout(
    wins: *const Rect,
    n: c_int,
    ax: f64,
    ay: f64,
    aw: f64,
    ah: f64,
    gap: f64,
    out: *mut Rect,
) -> c_int {
    if wins.is_null() || out.is_null() || n <= 0 || aw <= 0.0 || ah <= 0.0 {
        return 0;
    }
    let n = n as usize;
    let input = std::slice::from_raw_parts(wins, n);
    let out = std::slice::from_raw_parts_mut(out, n);

    if !any_overlap(input, 0.0, OVERLAP_EPS) {
        out.copy_from_slice(input); // nothing hidden: move nothing
        return 1;
    }

    // TIER 1 — THE NUDGE (Max, 2026-09-16: "i have one slight piece of a
    // window overlapping another, it will be enough moving one app a few
    // pixels… but our algorithm decides to move all the windows around,
    // reorganize them and then put them back. it makes no sense"). A desk
    // that is ALMOST revealed gets separated IN PLACE: full size, original
    // positions, only overlapping pairs pushed apart along the line between
    // their centres (a clamped axis bleeds into the free one). The result
    // ships only if every window moved less than NUDGE_MAX — a deep pile
    // fails the budget and falls through to the collage. Windows not party
    // to any overlap move EXACTLY zero, and there is no recentre: locality
    // is the whole point. The gap is softened (a nudge separates; it does
    // not compose).
    {
        let ngap = (gap / 3.0).max(6.0);
        let mut work: Vec<Rect> = input.to_vec();
        let mut settled = false;
        'passes: for _ in 0..64 {
            let mut moved = false;
            for i in 0..n {
                for j in i + 1..n {
                    let (ox, oy) = pair_overlap(&work[i], &work[j], ngap);
                    if ox <= 0.0 || oy <= 0.0 {
                        continue;
                    }
                    moved = true;
                    let mut dx = work[i].x + work[i].w / 2.0 - (work[j].x + work[j].w / 2.0);
                    let mut dy = work[i].y + work[i].h / 2.0 - (work[j].y + work[j].h / 2.0);
                    let len = (dx * dx + dy * dy).sqrt();
                    if len < 1.0 {
                        dx = -1.0; // stacked centres: dealt out by index, deterministically
                        dy = 0.0;
                    } else {
                        dx /= len;
                        dy /= len;
                    }
                    let tx = if dx.abs() > 1e-6 { ox / (2.0 * dx.abs()) } else { f64::MAX };
                    let ty = if dy.abs() > 1e-6 { oy / (2.0 * dy.abs()) } else { f64::MAX };
                    let t = tx.min(ty) + 0.5;
                    work[i].x += dx * t;
                    work[i].y += dy * t;
                    work[j].x -= dx * t;
                    work[j].y -= dy * t;
                }
            }
            for r in work.iter_mut() {
                clamp_into(r, ax, ay, aw, ah);
            }
            if !moved {
                settled = true;
                break 'passes;
            }
        }
        const NUDGE_MAX: f64 = 150.0;
        let within_budget = input.iter().zip(work.iter()).all(|(a, b)| {
            let (dx, dy) = (b.x - a.x, b.y - a.y);
            (dx * dx + dy * dy).sqrt() <= NUDGE_MAX
        });
        if (settled || !any_overlap(&work, ngap, -0.5)) && within_budget {
            out.copy_from_slice(&work);
            return 1;
        }
    }

    // TIER 2 — THE COLLAGE (Max, 2026-09-16: "measure them all, and somehow split the
    // workspace and find the best way to spread them… it have to be
    // decent"). Two relaxation-based looks died before this one: pushing
    // windows apart from where they were leaves scatter, because
    // "converged" only ever means NON-OVERLAPPING, never COMPOSED. So the
    // layout is BUILT, not simulated — the justified gallery.
    //
    // 1. Reading order — the windows as the desktop reads them, so what was
    //    upper-left lands upper-left and spatial memory survives.
    let mut order: Vec<usize> = (0..n).collect();
    order.sort_by(|&a, &b| {
        let (ca, cb) = (input[a].y + input[a].h / 2.0, input[b].y + input[b].h / 2.0);
        let (xa, xb) = (input[a].x + input[a].w / 2.0, input[b].x + input[b].w / 2.0);
        ca.partial_cmp(&cb).unwrap_or(std::cmp::Ordering::Equal).then(xa.partial_cmp(&xb).unwrap_or(std::cmp::Ordering::Equal)).then(a.cmp(&b))
    });
    let aspect: Vec<f64> = input.iter().map(|r| r.w.max(1.0) / r.h.max(1.0)).collect();
    // Prefix sums of aspect along the reading order: a row's aspect sum is
    // what sets its justified height, so it is the currency of every split.
    let prefix: Vec<f64> = std::iter::once(0.0)
        .chain(order.iter().scan(0.0, |acc, &i| {
            *acc += aspect[i];
            Some(*acc)
        }))
        .collect();
    let asum = |a: usize, b: usize| prefix[b] - prefix[a];

    // 2. For every row count: split the reading order into consecutive rows
    //    with BALANCED aspect sums (linear-partition DP), then JUSTIFY each
    //    row — all its cards share the row height, and that height is
    //    exactly what fills the full width. Cards never grow past real
    //    size; an overflowing stack of rows shrinks by one common factor.
    //    Highest displayed area wins. Aligned edges, exact gaps, full-width
    //    rows — decent by construction, nothing left to luck.
    let mut best: Option<(f64, Vec<usize>, Vec<f64>)> = None; // (score, row lengths, scale per `order` position)
    for rows in 1..=n {
        let mut dp = vec![vec![f64::MAX; rows + 1]; n + 1];
        let mut cut = vec![vec![0usize; rows + 1]; n + 1];
        dp[0][0] = 0.0;
        for r in 1..=rows {
            for i in r..=n {
                for j in (r - 1)..i {
                    if dp[j][r - 1] == f64::MAX {
                        continue;
                    }
                    let cand = dp[j][r - 1].max(asum(j, i));
                    if cand < dp[i][r] {
                        dp[i][r] = cand;
                        cut[i][r] = j;
                    }
                }
            }
        }
        if dp[n][rows] == f64::MAX {
            continue;
        }
        let mut lens = vec![0usize; rows];
        let mut i = n;
        for r in (1..=rows).rev() {
            let j = cut[i][r];
            lens[r - 1] = i - j;
            i = j;
        }

        let mut scales = vec![0.0f64; n]; // by position in `order`
        let mut bands = vec![0.0f64; rows];
        let mut idx = 0usize;
        for (r, &k) in lens.iter().enumerate() {
            let a = asum(idx, idx + k);
            let hrow = if a > 0.0 { ((aw - (k as f64 - 1.0) * gap) / a).max(0.0) } else { 0.0 };
            for p in idx..idx + k {
                let i = order[p];
                scales[p] = (hrow / input[i].h.max(1.0)).min(1.0);
                bands[r] = bands[r].max(input[i].h * scales[p]);
            }
            idx += k;
        }
        let bsum: f64 = bands.iter().sum();
        if bsum + (rows as f64 - 1.0) * gap > ah && bsum > 0.0 {
            let g = ((ah - (rows as f64 - 1.0) * gap) / bsum).max(0.01);
            for s in scales.iter_mut() {
                *s *= g;
            }
        }
        let score: f64 = order.iter().zip(&scales).map(|(&i, &s)| input[i].w * input[i].h * s * s).sum();
        if best.as_ref().map_or(true, |(bs, _, _)| score > *bs + 1e-6) {
            best = Some((score, lens, scales));
        }
    }
    let Some((_, lens, scales)) = best else {
        out.copy_from_slice(input);
        return 0; // unreachable for sane input; degenerate stays put
    };

    // 3. Build it: rows stacked with the gap, each row centred, each card
    //    vertically centred in its row's band; the flock then centres as one.
    let mut work = vec![Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 }; n];
    let mut idx = 0usize;
    let mut y = ay;
    for &k in &lens {
        let roww: f64 = (idx..idx + k).map(|p| input[order[p]].w * scales[p]).sum::<f64>() + (k as f64 - 1.0) * gap;
        let band: f64 = (idx..idx + k).map(|p| input[order[p]].h * scales[p]).fold(0.0, f64::max);
        let mut x = ax + ((aw - roww) / 2.0).max(0.0);
        for p in idx..idx + k {
            let i = order[p];
            let (w, h) = (input[i].w * scales[p], input[i].h * scales[p]);
            work[i] = Rect { x, y: y + (band - h) / 2.0, w, h };
            x += w + gap;
        }
        idx += k;
        y += band + gap;
    }
    recentre(&mut work, ax, ay, aw, ah);
    out.copy_from_slice(&work);
    1
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

    // ---- spread ------------------------------------------------------------

    const AREA: (f64, f64, f64, f64) = (0.0, 48.0, 1920.0, 1032.0); // usable under the bar
    const GAP: f64 = 24.0;

    fn spread(wins: &[Rect]) -> (Vec<Rect>, i32) {
        let mut out = vec![Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 }; wins.len()];
        let r = unsafe {
            waveview_spread_layout(wins.as_ptr(), wins.len() as c_int, AREA.0, AREA.1, AREA.2, AREA.3, GAP, out.as_mut_ptr())
        };
        (out, r)
    }

    fn needs(wins: &[Rect]) -> bool {
        unsafe { waveview_needs_spread(wins.as_ptr(), wins.len() as c_int) != 0 }
    }

    fn r(x: f64, y: f64, w: f64, h: f64) -> Rect {
        Rect { x, y, w, h }
    }

    fn assert_separated(rects: &[Rect]) {
        for i in 0..rects.len() {
            for j in i + 1..rects.len() {
                let (ox, oy) = pair_overlap(&rects[i], &rects[j], 0.0);
                assert!(ox <= 1.0 || oy <= 1.0, "pair {i}/{j} still overlaps ({ox:.1},{oy:.1})");
            }
        }
    }

    fn assert_inside(rects: &[Rect]) {
        for (i, b) in rects.iter().enumerate() {
            assert!(
                b.x >= AREA.0 - 0.5 && b.y >= AREA.1 - 0.5 && b.x + b.w <= AREA.0 + AREA.2 + 0.5 && b.y + b.h <= AREA.1 + AREA.3 + 0.5,
                "rect {i} escapes the area: ({:.0},{:.0} {:.0}x{:.0})",
                b.x, b.y, b.w, b.h
            );
        }
    }

    #[test]
    fn needs_spread_only_on_true_overlap() {
        assert!(!needs(&[r(0.0, 48.0, 960.0, 1032.0), r(960.0, 48.0, 960.0, 1032.0)])); // tiled: edges kiss
        assert!(!needs(&[r(100.0, 100.0, 400.0, 300.0)])); // lone window hides nothing
        assert!(!needs(&[]));
        assert!(needs(&[r(100.0, 100.0, 400.0, 300.0), r(300.0, 200.0, 400.0, 300.0)]));
        // fullscreen over a small window is the deepest hidden case
        assert!(needs(&[r(0.0, 0.0, 1920.0, 1080.0), r(500.0, 400.0, 600.0, 400.0)]));
    }

    #[test]
    fn no_overlap_is_identity() {
        let wins = [r(10.0, 60.0, 400.0, 300.0), r(1000.0, 500.0, 500.0, 400.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        for (a, b) in wins.iter().zip(out.iter()) {
            assert_eq!((a.x, a.y, a.w, a.h), (b.x, b.y, b.w, b.h));
        }
    }

    #[test]
    fn two_stacked_windows_separate() {
        // Perfectly stacked twins — centres coincide, the index breaks the tie.
        let wins = [r(700.0, 300.0, 500.0, 400.0), r(700.0, 300.0, 500.0, 400.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
    }

    #[test]
    fn fullscreen_over_small_both_fit() {
        let wins = [r(0.0, 0.0, 1920.0, 1080.0), r(600.0, 400.0, 500.0, 350.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
        // Scale is per-window now (the collage measures them all) but size
        // ORDER survives: the fullscreen card stays the bigger one, and the
        // small window comes out LESS shrunk than the giant.
        assert!(out[0].w > out[1].w && out[0].h > out[1].h);
        assert!(out[1].w / wins[1].w >= out[0].w / wins[0].w - 1e-9, "the small window shrank more than the giant");
        // Aspect is sacred: no card is distorted.
        for (a, b) in wins.iter().zip(out.iter()) {
            assert!((a.w / a.h - b.w / b.h).abs() < 1e-6, "aspect distorted");
        }
    }

    #[test]
    fn heterogeneous_collage_is_justified() {
        // Max's 2026-09-16 six-window desktop (big terminal, three 1100x675
        // foots, one narrow-tall, one wide-short): the scatter looked random
        // ("it sucks") — the collage must fill rows edge-to-edge and use a
        // solid share of the estate.
        let wins = [
            r(238.0, 151.0, 1312.0, 826.0),
            r(450.0, 302.0, 1100.0, 675.0),
            r(480.0, 332.0, 1100.0, 675.0),
            r(540.0, 392.0, 1100.0, 675.0),
            r(510.0, 424.0, 338.0, 613.0),
            r(570.0, 766.0, 756.0, 331.0),
        ];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
        let shown: f64 = out.iter().map(|b| b.w * b.h).sum();
        assert!(shown >= 0.45 * AREA.2 * AREA.3, "estate underused: {:.0}%", 100.0 * shown / (AREA.2 * AREA.3));
        // Justified: at least one row spans (nearly) the full width.
        let bx0 = out.iter().map(|b| b.x).fold(f64::MAX, f64::min);
        let bx1 = out.iter().map(|b| b.x + b.w).fold(f64::MIN, f64::max);
        assert!(bx1 - bx0 >= 0.98 * AREA.2, "no full-width row: {:.0} of {:.0}", bx1 - bx0, AREA.2);
    }

    #[test]
    fn displacement_preserving_order_survives() {
        // A left and a right window overlapping in the middle keep their sides.
        let wins = [r(200.0, 300.0, 800.0, 500.0), r(700.0, 350.0, 800.0, 500.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert!(out[0].x + out[0].w / 2.0 < out[1].x + out[1].w / 2.0, "left window ended up right");
    }

    #[test]
    fn nine_piled_windows_converge() {
        let mut wins = Vec::new();
        for i in 0..9 {
            let f = i as f64;
            wins.push(r(400.0 + f * 60.0, 200.0 + f * 40.0, 700.0, 500.0));
        }
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
    }

    #[test]
    fn clustered_windows_fill_the_area() {
        // Three windows piled in one corner must come out USING the estate —
        // full-ish span, no shrink — not separated-in-place around the pile
        // (the 2026-09-16 "a lot of waste space" regression case).
        let wins = [r(1100.0, 100.0, 700.0, 500.0), r(1300.0, 200.0, 700.0, 500.0), r(1500.0, 300.0, 700.0, 500.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
        for (a, b) in wins.iter().zip(out.iter()) {
            assert!((a.w - b.w).abs() < 1e-9, "needless shrink: {} -> {}", a.w, b.w);
        }
    }

    #[test]
    fn slight_overlap_gets_a_nudge_not_a_collage() {
        // Max's 2026-09-16 desk: four placed windows, ONE slight overlap
        // (a terminal's edge over a neighbour). Bystanders must not move AT
        // ALL; the involved pair slides apart a few dozen px; nobody is
        // resized; no rows, no recentre ("it will be enough moving one app
        // a few pixels… it makes no sense").
        let wins = [
            r(60.0, 100.0, 800.0, 350.0),  // top-left, clear of everyone
            r(1300.0, 90.0, 550.0, 380.0), // top-right, clear of everyone
            r(50.0, 520.0, 700.0, 500.0),  // bottom-left …
            r(690.0, 500.0, 900.0, 520.0), // … whose right edge this one overlaps by ~60px
        ];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        for (a, b) in wins.iter().zip(out.iter()) {
            assert!((a.w - b.w).abs() < 1e-9 && (a.h - b.h).abs() < 1e-9, "a nudge must not resize");
        }
        assert_eq!((wins[0].x, wins[0].y), (out[0].x, out[0].y), "bystander 0 moved");
        assert_eq!((wins[1].x, wins[1].y), (out[1].x, out[1].y), "bystander 1 moved");
        for i in [2usize, 3] {
            let d = ((out[i].x - wins[i].x).powi(2) + (out[i].y - wins[i].y).powi(2)).sqrt();
            assert!(d > 0.0 && d <= 150.0, "window {i} moved {d:.0}px");
        }
    }

    #[test]
    fn four_terminals_come_out_at_the_oracle_bound() {
        // Max's 2026-09-16 desktop: four 1100x675 foots in a pile. The old
        // relaxation stalled and shrank them to ~0.6; the oracle (2 rows of
        // 2) admits ~0.75 in this test area — cards must reach it.
        let wins = [
            r(400.0, 200.0, 1100.0, 675.0),
            r(480.0, 260.0, 1100.0, 675.0),
            r(560.0, 320.0, 1100.0, 675.0),
            r(640.0, 380.0, 1100.0, 675.0),
        ];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        assert_inside(&out);
        for b in &out {
            assert!((b.w - out[0].w).abs() < 1e-6, "scale not uniform");
            assert!(b.w >= 0.73 * 1100.0, "below the oracle bound: {:.0}", b.w);
        }
    }

    #[test]
    fn no_needless_shrink_when_the_area_has_room() {
        // Two overlapped windows that plainly fit side by side keep full size.
        let wins = [r(500.0, 300.0, 800.0, 600.0), r(700.0, 400.0, 800.0, 600.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        for (a, b) in wins.iter().zip(out.iter()) {
            assert!((a.w - b.w).abs() < 1e-9 && (a.h - b.h).abs() < 1e-9, "shrunk with room to spare");
        }
    }

    #[test]
    fn spread_centres_the_flock() {
        // A bottom-heavy pile must come out balanced: group air above ≈ below.
        let wins = [r(100.0, 700.0, 900.0, 320.0), r(300.0, 800.0, 900.0, 320.0)];
        let (out, ok) = spread(&wins);
        assert_eq!(ok, 1);
        assert_separated(&out);
        let top = out.iter().map(|b| b.y).fold(f64::MAX, f64::min) - AREA.1;
        let bottom = AREA.1 + AREA.3 - out.iter().map(|b| b.y + b.h).fold(f64::MIN, f64::max);
        assert!((top - bottom).abs() < 1.0, "unbalanced: top {top:.1} vs bottom {bottom:.1}");
        let left = out.iter().map(|b| b.x).fold(f64::MAX, f64::min) - AREA.0;
        let right = AREA.0 + AREA.2 - out.iter().map(|b| b.x + b.w).fold(f64::MIN, f64::max);
        assert!((left - right).abs() < 1.0, "unbalanced: left {left:.1} vs right {right:.1}");
    }

    #[test]
    fn spread_is_deterministic() {
        let wins = [r(100.0, 100.0, 900.0, 700.0), r(400.0, 300.0, 900.0, 700.0), r(700.0, 500.0, 900.0, 700.0)];
        let (a, _) = spread(&wins);
        let (b, _) = spread(&wins);
        for (x, y) in a.iter().zip(b.iter()) {
            assert_eq!((x.x, x.y, x.w, x.h), (y.x, y.y, y.w, y.h));
        }
    }

    #[test]
    fn degenerate_inputs_are_safe() {
        let mut out = [r(0.0, 0.0, 0.0, 0.0)];
        unsafe {
            assert_eq!(waveview_spread_layout(std::ptr::null(), 1, 0.0, 0.0, 100.0, 100.0, 8.0, out.as_mut_ptr()), 0);
            let w = [r(0.0, 0.0, 10.0, 10.0)];
            assert_eq!(waveview_spread_layout(w.as_ptr(), 1, 0.0, 0.0, -5.0, 100.0, 8.0, out.as_mut_ptr()), 0);
            assert_eq!(waveview_needs_spread(std::ptr::null(), 5), 0);
        }
    }

    #[test]
    fn mockup_grid_margins_and_gaps() {
        let mut tiles = [Rect { x: 0.0, y: 0.0, w: 0.0, h: 0.0 }; N_TILES];
        let (mw, mh, top, gap, outer) = (1920.0, 1080.0, 48.0, 12.0, 35.0);
        let n = unsafe { waveview_workspace_tiles(mw, mh, top, gap, outer, tiles.as_mut_ptr()) };
        assert_eq!(n, N_TILES as c_int);
        // Top-anchored exactly at `top` (bar + top-gap).
        assert!((tiles[0].y - top).abs() < 1e-9);
        // Sides and bottom carry the `outer` margin; seams carry `gap`.
        assert!((tiles[0].x - outer).abs() < 1e-9);
        assert!((mw - (tiles[2].x + tiles[2].w) - outer).abs() < 1e-6);
        assert!((mh - (tiles[8].y + tiles[8].h) - outer).abs() < 1e-6);
        let inner_x = tiles[1].x - (tiles[0].x + tiles[0].w);
        let inner_y = tiles[3].y - (tiles[0].y + tiles[0].h);
        assert!((inner_x - gap).abs() < 1e-6 && (inner_y - gap).abs() < 1e-6);
        // Rows 4-6 continue the exact rhythm below the fold.
        let row_step = tiles[3].y - tiles[0].y;
        for r in 1..6 {
            assert!((tiles[r * 3].y - tiles[0].y - r as f64 * row_step).abs() < 1e-6);
        }
        assert!(tiles[17].y + tiles[17].h > mh); // below the fold, page 2 reveals
    }
}
