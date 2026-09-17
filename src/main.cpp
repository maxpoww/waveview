// Pull in the standard headers Hyprland includes transitively BEFORE the
// private/public hack below, so their include guards are already set and the
// macro can't rewrite libstdc++ access specifiers.
#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// beginRender()/renderWorkspace() are protected on IHyprRenderer; capturing
// workspace thumbnails needs them. Same hack the official hyprexpo plugin uses —
// must wrap every Hyprland header (they pull Renderer.hpp transitively).
#define private   public
#define protected public
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/XWaylandManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/CursorManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#undef private
#undef protected

// Golem's titlebars for floating windows (a stripped fork of hyprbars).
#include "hyprbars/bars.hpp"
#include "hyprbars/golemBar.hpp" // GOLEM_BAR_HEIGHT: the spread's cards carry their bars

extern "C" {
#include <lua.h>
}

// ---- Rust brain (FFI) --------------------------------------------------------
struct Rect {
    double x, y, w, h;
};
extern "C" {
const char* waveview_hello();
int         waveview_workspace_tiles(double mw, double mh, double top, double gap, double outer, Rect* out);
int         waveview_tile_for_workspace(int64_t ws_id);
void        waveview_map_window(double tx, double ty, double tw, double th, double mon_x, double mon_y, double mon_w,
                                double mon_h, double wx, double wy, double ww, double wh, Rect* out);
int         waveview_needs_spread(const Rect* wins, int n);
int         waveview_spread_layout(const Rect* wins, int n, double ax, double ay, double aw, double ah, double gap,
                                   Rect* out);
}

// 3x6 workspace grid (18 workspaces); rows 4-6 live below the fold and
// scroll into view. Must match the brain's N_TILES.
static constexpr int      N_TILES   = 18;
static constexpr uint32_t ALL_TILES = (1u << N_TILES) - 1;

// The mockup-blessed design (Max, ~/overview-mockup settings 2026-08-30),
// all in LOGICAL px (scaled to draw space at use): bare wallpaper (no tile
// cards / dim / numbers), breathing margins, soft corners, and in-tile
// window gaps ~3x their literal miniature (solo windows stay full-bleed —
// smart gaps).
static constexpr double DSN_GAP        = 20.0; // between tiles
// The SPREAD (level 1 of the reveal ladder — see the spread section below):
// minimum air between spread cards, and the margin the cards keep from the
// usable area's edges. Logical px, like everything designed.
static constexpr double DSN_SPREAD_GAP    = 24.0;
static constexpr double DSN_SPREAD_MARGIN = 16.0;
static constexpr double DSN_OUTER      = 35.0; // side + bottom margins
static constexpr double DSN_TOP_GAP    = 12.0; // below the bar
static constexpr double DSN_TILE_ROUND = 28.0; // hover/drop frame corners
static constexpr double DSN_WIN_ROUND  = 20.0; // window mini corners
static constexpr double DSN_WIN_GAP    = 0.028; // in-tile gap, fraction of tile
// Overview borders mirror the DESKTOP's window borders — same thickness on
// every window regardless of its size, per Max. The desktop's border is no
// longer a fixed amber: waverunner pushes `general:col.active_border` as a
// live gradient of the screen's own sampled colours, so the overview reads
// THAT (see activeBorderGradient) rather than a baked constant, and the two
// can never drift (Max, 2026-09-11: "the overview tiles border ... match
// dynamically too"). The constant stays as the fallback for a compositor
// whose config read fails.
static constexpr double     DSN_BORDER_W = 3.0; // logical, scaled at use
static const CHyprColor     DSN_BORDER_COL{1.0, 0.745, 0.596, 1.0};   // #ffbe98

// The compositor's LIVE active-border gradient — the very object the desktop
// draws its own window borders from (Window.cpp reads it exactly this way),
// so the overview inherits the colours, the stop count AND the angle for
// free, including whatever waverunner pushed a moment ago.
//
// nullptr whenever the value is missing, is not a gradient, or carries no
// colours; callers then fall back to the flat DSN_BORDER_COL. This runs
// inside the render hook, where a wrong colour is cosmetic but a null deref
// ends the session — hence a check at every step rather than a bare cast.
// Not static: Golem's titlebars (src/hyprbars/) read it too, to tint a
// floating window's frame from the same live colours. The `m_p` guard needs
// the privates-opening hack at the top of this file, which is why the helper
// lives here rather than being copied into the bar.
Config::CGradientValueData* borderGradient(bool active) {
    static CConfigValue<Config::IComplexConfigValue> s_active("general:col.active_border");
    static CConfigValue<Config::IComplexConfigValue> s_inactive("general:col.inactive_border");
    auto&                                            slot = active ? s_active : s_inactive;
    if (!slot.good() || !slot.m_p) // ptr() dereferences m_p unconditionally
        return nullptr;
    auto* const v = slot.ptr();
    if (!v || v->getDataType() != Config::CVD_TYPE_GRADIENT)
        return nullptr;
    auto* const grad = static_cast<Config::CGradientValueData*>(v);
    return grad->m_colors.empty() ? nullptr : grad;
}

static Config::CGradientValueData* activeBorderGradient() {
    return borderGradient(true);
}

inline HANDLE              PHANDLE = nullptr;
static bool                g_active = false;
static bool                g_capturing = false; // true while rendering thumbnails, so the render hook doesn't paint the overview into them (grid-in-grid recursion)
static CHyprSignalListener g_renderListener;
static CHyprSignalListener g_keyListener;    // swallows digit/Escape presses while the overview is open (see onKey)
static CHyprSignalListener g_moveListener;   // tracks the cursor for hover/drag (see onMouseMove)
static CHyprSignalListener g_buttonListener; // left-click to pick up / drop a window (see onMouseButton)
static CHyprSignalListener g_swipeBeginListener;  // 3-finger swipe up/down toggles the overview (see onSwipe*)
static CHyprSignalListener g_swipeUpdateListener;
static CHyprSignalListener g_swipeEndListener;
static CHyprSignalListener g_axisListener; // wheel while open scrolls the 3x6 grid (see onMouseAxis)
static CHyprSignalListener g_windowOpenListener; // gives each new window its (floating-only) titlebar — see src/hyprbars/
static CHyprSignalListener g_windowDestroyListener; // a window dying while MINIMIZED must leave the dock (see PLUGIN_INIT)
static SP<CEventLoopTimer> g_liveTimer; // re-arms every REFRESH_MS while open to keep thumbnails live
static SP<CEventLoopTimer> g_dragCheckTimer; // one-shot after a button event: the compositor's drag state settles around our listener
static SP<CEventLoopTimer> g_handTimer;      // one-shot: the open hand shown at press closes shortly after
// A press shows the hand OPEN for a beat, then closes it, so the grab reads
// as a hand taking hold rather than a shape swap. Deliberately longer than
// the 10ms asked for: a frame is ~16ms at 60Hz, so a 10ms open hand would be
// replaced before it was ever scanned out. This is about the shortest that
// actually renders as a beat.
static constexpr int HAND_CLOSE_MS = 90;
static void                checkResizeDrag(); // defined with the waverunner channel below
static void                noteInteraction(bool pointer); // ditto — feeds the daemon's focus-cycle frecency
static void                sendOverviewHover(PHLWINDOW w); // topbar pill follows the overview's pointer
static void                sendOverviewSize(PHLWINDOW w, bool force); // live size while resizing a thumbnail
static void                resetOverviewPill(); // forget what the pill was told (overview opened/closed)

// evdev keycodes as delivered by the input event (xkb code = evdev + 8). Digit
// row is contiguous: KEY_1..KEY_9 = 2..10, so workspace N is keycode N + 1.
static constexpr uint32_t EVDEV_ESC   = 1;
static constexpr uint32_t EVDEV_1     = 2;
static constexpr uint32_t EVDEV_9     = 10;
static constexpr uint32_t EVDEV_Q     = 16;  // KEY_Q — close the hovered window
static constexpr uint32_t EVDEV_LMETA = 125; // Super, tracked so bind combos
static constexpr uint32_t EVDEV_RMETA = 126; // pass through the key swallow
static bool               g_superHeld = false;

static constexpr auto REFRESH_MS = std::chrono::milliseconds(150);

// One captured thumbnail per grid slot (workspaces 1..9), plus the monitor the
// snapshots belong to — thumbnails are only valid on that monitor. These full
// workspace snapshots are no longer drawn directly; they're the SOURCE pixels we
// crop individual windows out of (see captureWindows).
static SP<Render::IFramebuffer> g_fbs[N_TILES];
static PHLMONITORREF            g_captureMon;

// The single wallpaper-only backdrop (no windows, no bars) drawn behind
// everything, replacing the old per-tile repeated wallpapers.
static SP<Render::IFramebuffer> g_bgFB;

// One cropped thumbnail per live window: just that window's pixels, sliced out of
// its workspace snapshot. Drawn as an individually rounded, shrunk rect floating
// over the wallpaper at its slot position.
struct CapWin {
    SP<Render::IFramebuffer> fb;      // the window's own cropped texture
    PHLWINDOWREF             win;     // the live window (for drag → move-to-workspace)
    Rect                     logical; // the window's TRUE box in logical layout coords — always, even on a spread tile
    Rect                     slot;    // its collage seat (spread tiles only) — the TARGET side of the layout blend.
                                      // ⚠ never overwrite `logical` with this: the landing glide interpolates
                                      // true↔slot, and with the slot on both sides it was a no-op — the windows
                                      // JUMPED to their resting point at the overlay drop (Max, 2026-09-16).
    int                      tile;    // grid slot 0..8 (its workspace)
    bool                     active;  // currently focused window
    bool                     spread = false; // its tile shows the collage: no seam-solving, pixels from its solo
    CBox                     screen;  // last-drawn box in draw space (for hit-testing)
    CBox                     drawCur; // eased chase of the mapped slot — re-tiles glide at frame rate
};
// One shared ease rate for every drag chase (slot glides, ghost dims) —
// slowed from the original 14/s snap to sit closer to the desktop's
// window spring, so reactions read as motion, not as teleports.
static constexpr float PREVIEW_RATE = 9.f;
static std::vector<CapWin> g_wins;

// ---- STAGE 2 of the spread: overlapped workspaces render PRE-SPREAD inside
// their overview tiles (Max: "the WS on overview that have overlaped windows
// have to show on the overview all spreaded"). Per tile: whether it hides
// windows, and each window's collage slot. Per window on such a tile: a SOLO
// capture (rendered alone, standalone) — the workspace snapshot holds the
// OCCLUDER's pixels at a buried window's rect, so cropping it can never
// reveal what's hidden.
struct STileSpread {
    bool                                       on = false;
    std::vector<std::pair<PHLWINDOWREF, Rect>> slots; // window → its collage seat (logical, usable-area space)
};
static STileSpread g_tileSpread[N_TILES];

struct SoloCap {
    PHLWINDOWREF             win;
    SP<Render::IFramebuffer> fb; // window-sized; the true pixels, even when buried
};
static std::vector<SoloCap> g_solo;

static const Rect* tileSpreadSlot(int tile, PHLWINDOW w) {
    if (tile < 0 || tile >= N_TILES || !g_tileSpread[tile].on)
        return nullptr;
    for (auto& [ref, slot] : g_tileSpread[tile].slots)
        if (ref.lock() == w)
            return &slot;
    return nullptr;
}

static SoloCap* soloFor(PHLWINDOW w, bool create) {
    for (auto& sc : g_solo)
        if (sc.win.lock() == w)
            return &sc;
    if (!create)
        return nullptr;
    g_solo.push_back({w, nullptr});
    return &g_solo.back();
}

// Windows die; their solo textures must not linger in VRAM.
static void sweepSolos() {
    std::erase_if(g_solo, [](SoloCap& sc) {
        if (sc.win.lock())
            return false;
        if (sc.fb)
            sc.fb->release();
        return true;
    });
}

static void computeTileSpreads(PHLMONITOR m, uint32_t mask, uint32_t occupied); // defined with the spread machinery
static void captureSolosForTile(PHLMONITOR m, int tile, const CBox& monbox);    // ditto

// ---- Open warp ---------------------------------------------------------------
// The overview opens under your hand: the pointer lands on the thumbnail of the
// window you called it FROM, so the first gesture is already aimed at where you
// were. Without it the pointer stays wherever it was parked — usually nowhere
// near the window you just left — and every open starts with a hunt.
//
// It has to wait for the zoom to settle: a window's drawn box is only known
// after a frame is laid out (`CapWin::screen` is filled during the draw), and
// warping mid-zoom would land the pointer where the thumbnail *was* a moment
// before it slid on.
// The pointer is HIDDEN for the length of the zoom and shown again once it has
// been placed, so the move is never seen: the cursor simply is where it should
// be when the overview arrives, instead of visibly jumping across the screen
// after the tiles land (Max, 2026-09-03: "can we hide the pointer during the
// relocation?"). Every exit from the pending state must show it again — a
// hidden pointer is the worst thing to leave behind.
static PHLWINDOWREF g_warpWin;              // the window focused when we opened
static bool         g_warpPending = false;  // still owed a warp this open
static bool         g_warpHidCursor = false; // we are the ones hiding it
static Vector2D     g_warpFromCursor;       // where the pointer sat at open
// Deliberate pointer motion during the zoom cancels the warp: if the hand is
// already driving, yanking the cursor away is worse than not helping. But the
// whole pending window is ANIM_SECONDS (~280 ms) during which the pointer is
// HIDDEN — nobody is aiming a pointer they cannot see. At 8 px, touchpad noise
// (finger-lift, a resting palm, the pad's own settling) cancelled gesture
// opens at random and the feature read as unreliable (Max, 2026-09-03: "it
// works sometimes, sometimes it does not"). Consistency IS the feature; the
// cancel only needs to catch a genuine flick — a hand demonstrably going
// somewhere — so the threshold is a flick, not a jitter.
static constexpr double WARP_CANCEL_SLOP = 150.0;
// …but the 3-finger swipe OPENS the overview mid-gesture, with the fingers
// still on the pad — lifting them delivers residual pointer motion that is
// part of *invoking*, not of aiming, and it was cancelling the warp every
// time (Max, 2026-09-03: "works with supr+r but not so well calling overview
// with gesture" — Super+R produces no pointer motion, the swipe does). While
// the swipe is live, and for a beat after the fingers leave, such motion
// re-baselines the cancel reference instead of cancelling.
static bool            g_swipeLive = false;
static Time::steady_tp g_swipeEndAt{};
static constexpr auto  SWIPE_SETTLE = std::chrono::milliseconds(300);

// Show the pointer again if the open warp hid it. Idempotent, and safe to call
// from any path that ends the pending state (warp done, cancelled, overview
// closed) — whoever gets there first restores it.
static void endWarpHide() {
    if (!g_warpHidCursor)
        return;
    g_warpHidCursor = false;
    g_pHyprRenderer->setCursorHidden(false);
}

// Pointer interaction, all in "draw space" (whole monitor = [0,0,transformedSize]).
// Tracked by window handle, not g_wins index — the vector is rebuilt every capture.
static PHLWINDOWREF g_hoverWin;              // window under the cursor (gets a border)
static PHLWINDOWREF g_dragWin;              // window pressed on; becomes a drag once the cursor moves past CLICK_SLOP
static Vector2D     g_dragCursor;            // current cursor in draw space (while dragging)
static Vector2D     g_pressPos;              // cursor at button-press (draw space) — to tell a click from a drag
static bool         g_dragMoved = false;     // cursor left the CLICK_SLOP radius since press → treat as a drag, not a click
static int          g_pressTile = -1;        // empty tile a press landed on (release within slop → jump), or -1
// A press that releases within this radius (draw-space px) is a click (jump to
// the window), not a drag (move it to another workspace).
static constexpr double CLICK_SLOP = 12.0;
// BTN_LEFT (0x110) comes from linux/input-event-codes.h, pulled in transitively.

// Page-based scroll (per Max): the 3x6 grid is two PAGES of 3x3. One wheel
// notch flips a page (eased, dt-based — target set by input, position
// chases in onRender); digits 1..9 select within the current page.
static double          g_scroll       = 0.0;
static double          g_scrollTarget = 0.0;
static double          g_scrollFrom   = 0.0; // flip start position
static float           g_scrollProg   = 1.0; // 0..1 through the flip (1 = settled)
static int             g_page         = 0; // 0 = workspaces 1-9, 1 = 10-18
static Time::steady_tp g_pageFlipAt{};     // cooldown: one notch = one flip
static constexpr float SCROLL_SECONDS = 0.42f; // page-flip duration
// True once the user has seen the other page this open (wheel or Super+R
// tour) — the next toggle press then closes (see toggle()).
static bool            g_tourDone     = false;
// Wall-clock dt of the current frame (set in onRender), for the preview glide.
static float           g_frameDt      = 0.016f;
// A REAL compositor drag is running for the overview drag (begun at grab so
// the siblings re-tile live — no hole where the window was). Ended at drop,
// or back at home on cancel/close.
static bool            g_dragReal       = false;
static Vector2D        g_dragHomeCenter = {};
// The cursor ghost SHAPESHIFTS to preview its destination: eased dims chase
// the slot it would take (complement half / full empty view / own size),
// anchored by the grab point as a fraction of the box.
static double          g_ghostW = 0.0, g_ghostH = 0.0;
static double          g_ghostWantW = 0.0, g_ghostWantH = 0.0;
static double          g_grabFracX = 0.5, g_grabFracY = 0.5;
// Live-commit state (see the "life reaction" block before updateHoverAt).
static constexpr auto DWELL           = std::chrono::milliseconds(120);
static constexpr auto COMMIT_COOLDOWN = std::chrono::milliseconds(150);
// How long the ~20fps capture boost runs after a commit/regrab/drop: must
// cover the whole "windows" spring + settle tail (spring "easy" ≈ 700ms;
// double this when slow-mo debugging halves the spring stiffness).
static constexpr auto BOOST_MS        = std::chrono::milliseconds(700);
static Time::steady_tp g_lastCommit{};
struct LiveCommit {
    bool         active = false; // false = no target under the cursor
    int          tile   = -1;
    PHLWINDOWREF under;       // split target; empty = whole-view insert
    int          side   = -1; // quadrant side, so crossing a diagonal re-commits
};
static LiveCommit      g_commit;        // what is actually placed right now
static LiveCommit      g_pending;       // last seen hover signature
static Time::steady_tp g_pendingSince{};
static int             g_origWS = -1;   // cancel restores these
static Vector2D        g_origHome{};
static bool            g_origFloating = false; // float state at gesture start
static uint32_t        g_dirtyTiles   = 0;     // tiles touched since the last full capture
// The commit machinery warps the cursor (beginRealDrag / endRealDrag seed
// their drags with warps); each warp fires a SYNTHETIC motion event that
// re-entered updateHoverAt and overwrote g_dragCursor mid-commit — the
// trace showed a commit with desk=(-579,-622), an off-screen drop point,
// which is how dwindle got fed garbage (and windows came out floating).
static bool            g_busy = false;
// Every machinery function guards itself: the 2026-08-30 ws3 float leak was
// the RELEASE path running endRealDrag/placeAt unguarded — their warps fed
// back into updateHoverAt, which re-captured g_origFloating mid-machinery
// (window transiently floating) and re-entered beginRealDrag. Save/restore
// nests, so guarded functions can call each other.
struct SBusyScope {
    bool prev;
    SBusyScope() : prev(g_busy) { g_busy = true; }
    ~SBusyScope() { g_busy = prev; }
};
static Time::steady_tp g_boostUntil{};  // fast recapture while a real re-tile springs

// Mark the workspace's tile for the next partial capture; no-op off-grid.
static void markDirty(int64_t wsId) {
    if (const int t = waveview_tile_for_workspace(wsId); t >= 0)
        g_dirtyTiles |= (1u << t);
}
// Capture at ~20fps for the next BOOST_MS — a real re-tile is springing
// and the tiles should show the actual animation.
static void boostCaptures() {
    g_boostUntil = Time::steadyNow() + BOOST_MS;
    if (g_liveTimer)
        g_liveTimer->updateTimeout(std::chrono::milliseconds(50));
}
// Float-leak watch (diagnosis, v0.21): every gesture end arms this; the live
// timer then compares the window's float state against what the gesture
// promised, for 3s — catching a leak that happens AFTER the synchronous
// nets ran (the only place left: every static path is netted).
static PHLWINDOWREF    g_watchWin;
static bool            g_watchFloatExpect = false;
static Time::steady_tp g_watchSince{};
// Border resize (v0.25): hovering a thumbnail's edge flips the pointer to a
// resize shape; the LEFT press then resizes instead of moving — exactly the
// desktop's resize_on_border, scaled into the tile. No drag-controller
// session and no cursor warps — resizeTarget() applies scaled deltas
// directly (dwindle adjusts ratios, floats change size), so none of the
// warp-feedback hazards of the move machinery exist here. Edges resize one
// axis (the mask zeroes the other), corners both.
static bool                g_resizing     = false;
static PHLWINDOWREF        g_resizeWin;
static Layout::eRectCorner g_resizeCorner = Layout::CORNER_NONE;
static Vector2D            g_resizeMask;  // 1/0 per axis: which deltas this zone applies
static Vector2D            g_resizeLast;  // drawn-space cursor at the last applied step
static Vector2D            g_resizeScale; // drawn px -> desktop logical px for the window's tile
// Hover side of it: the zone under the pointer, live between motions.
static PHLWINDOWREF        g_edgeWin;
static Layout::eRectCorner g_edgeCorner = Layout::CORNER_NONE;
static Vector2D            g_edgeMask;
static std::string         g_edgeShape; // resize cursor we set; empty = not ours

// Zoom animation: 0 = zoomed fully into g_zoomTile (that workspace fills the
// screen), 1 = the whole 3x3 grid at its rest layout. Opening animates 0->1,
// closing 1->0. g_zoomTile is the tile the zoom pivots on.
static float           g_anim       = 0.0f;
static float           g_animTarget = 0.0f;
static int             g_zoomTile   = 0;
static Time::steady_tp g_animLastT;
// A close held over Golem's STAGE, waiting for waverunner to rebuild the stage
// so the zoom can fly into a picture of it rather than of the bare workspace
// (see `startClose`). `g_stageCloseAt` is when the hold began — the map must
// never stay open because an answer never came.
static bool            g_stageClosePending = false;
// While true, `drawOverview` PAINTS the stage's dim over everything outside the
// staged window — the only way it can land as a step, because a capture can
// carry `dim_around` in one tile only. Set by `stage_ready` on the same frame as
// the morph; cleared at the disengage and on every open.
static bool            g_stageDimPaint     = false;
// Ask waverunner for the REAL dim once, from the first linger frame — under the
// overlay, so the desktop is already dark when it lifts and the painted dim it
// replaces is the same shape.
static bool            g_stageDimAsk       = false;
/// Must match waverunner's `hypr::STAGE_DIM` — the stage's `decoration:dim_around`.
static constexpr float GOLEM_STAGE_DIM     = 0.8f;
static Time::steady_tp g_stageCloseAt;
static constexpr auto  STAGE_CLOSE_WAIT    = std::chrono::milliseconds(400);
/// Per-close override for the zoom clock (0 = use `ANIM_SECONDS`). Cleared at
/// the disengage and on every open, so it can never leak into another close.
static float           g_zoomSeconds       = 0.0f;
static constexpr float ANIM_SECONDS = 0.28f;
/// How fast the map OPENS — its own dial, so it can be quicker than the way
/// back without touching the ordinary overview close (Max, 2026-09-17: *"in to
/// overview faster"*).
static constexpr float OPEN_ZOOM_SECONDS  = 0.14f; // 0.28 -> 0.20 -> 0.14
/// How long the tile's windows take to spread apart when the map opens — the
/// reveal, on its OWN clock so it can outlast the (much shorter) opening zoom.
static constexpr float OPEN_SPREAD_SECONDS = 0.19f; // 0.30 -> 0.24 -> 0.19
/// Its progress, 0..1. Reset on every open; advanced per frame in `onRender`.
static float           g_openBlend         = 0.0f;
/// How long the stage's way back takes — **the dial**.
///
/// Deliberately `ANIM_SECONDS`, the OPEN's own clock: the open is the one Max
/// likes, and this is that same movement played the other way. The instant cut
/// was right in substance and wrong in manner (*"it jumps in front… is good, but
/// too fast"*, 2026-09-17); 0 here would be that cut again, larger reads calmer.
static constexpr float STAGE_ZOOM_SECONDS = 0.34f; // 0.28 -> 0.36 -> 0.46 -> 0.40 -> 0.34
// Closing gets its own, longer clock (the spread's glide home): a return is
// a landing, not an invocation — it doesn't answer a keypress, so it may
// take the time it needs to read as calm. Same family as the page flip's
// 0.42s ease-in-out (chosen there for the same reason: max velocity on
// frame one reads as a jerk).
static constexpr float CLOSE_SECONDS = 0.40f;
// The overview close's SECOND ACT (Max, 2026-09-16: "use the animation of
// closing the spread… selecting a window should call that workspace, and
// then do the normal close animation we have for closing the overview 1"):
// the zoom carries the tile to fullscreen with its arrangement INTACT, and
// only then this glide walks every window home — 1:1 screen space, no
// magnification amplifying the motion. Three rounds of curve tuning could
// not fix what was structural: a reorganization riding a zoom is amplified
// by it; a reorganization at flat scale is the spread's close, the one Max
// called smooth.
static float           g_landAnim    = 0.0f; // 1 → 0 through the close's landing act
static constexpr float LAND_SECONDS  = 0.35f;
static int             g_closeLinger = 0;    // desktop-identical overlay frames held over the hand-off
// VRAM is freed on a timer tick ~450ms AFTER a close, never at the hand-off:
// releasing ~0.5GB in one synchronous burst inside the render callback could
// leave the WALLPAPER texture non-resident for one frame — the whole
// background rendered black once (THE BLINK, caught on video 2026-09-16;
// invisible on the old dark wallpaper, a flash on the new light one).
static bool g_freePending = false;
// The landing act starts in the zoom's final quarter (tile ≈ 90% of full
// size — magnification negligible) so the two acts hand over in one breath:
// a full stop between them read as move…stop…move (Max, 2026-09-16 round 6).
static constexpr float LAND_EARLY    = 0.25f;

// Trackpad gesture: a 3-finger vertical swipe toggles the overview (up = open,
// down = close). Deltas accumulate over the gesture; once the dominant axis is
// vertical and past SWIPE_TRIGGER we fire once and latch until the gesture ends.
static uint32_t         g_swipeFingers = 0;
static Vector2D         g_swipeAcc;
static bool             g_swipeFired   = false;
static constexpr double SWIPE_TRIGGER  = 120.0; // accumulated px of vertical travel

// While Golem's STAGE owns the screen the same 3/4-finger swipe walks the
// border along its deck instead of changing workspace, and waverunner stages
// whatever it lands on when the fingers leave. We only report travel: how far a
// tile is, and which tile that makes, is the deck's business.
static bool             g_stageSwipe     = false; // this gesture belongs to the deck
static double           g_stageSwipeDx   = 0.0;   // total travel, positive rightward
static double           g_stageSwipeSent = 0.0;   // travel at the last message
// Travel between messages. Finer than the deck's own per-tile distance so the
// border never lags a tile behind the fingers, coarse enough that a long swipe
// is a dozen messages rather than a hundred.
static constexpr double STAGE_SWIPE_GRAIN = 25.0;

static double mix(double a, double b, double t) {
    return a + (b - a) * t;
}
static float easeOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
// Ease-in-out cubic: gentle start, gentle landing — the page-flip curve,
// and now the curve of every CLOSE (overview zoom, spread): a zoom-out
// concentrates its perceived scale-change at the small end, so a fast-start
// curve kicks twice on frame one. Gentle both ends reads as a landing.
static double easeInOutCubic(double t) {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}
// Its inverse — retargets remap the anim position onto the new curve so a
// direction change turns around exactly where it is.
static float invEaseInOutCubic(float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    return p < 0.5f ? std::cbrt(p / 4.0f) : 1.0f - std::cbrt(2.0f * (1.0f - p)) / 2.0f;
}

// ---- The SPREAD (macOS-style per-workspace exposé) ---------------------------
//
// Level 1 of the reveal ladder. The 3-up swipe means "reveal what's hidden,
// one level at a time": when windows on the current workspace hide each
// other, the first swipe spreads THEM apart (this); the second escalates to
// the overview (level 2). With nothing hidden here, level 1 has nothing to
// show and the swipe goes straight to the overview — today's behaviour,
// untouched. Golem is floating-first, so on most workspaces the spread IS
// the first rung; tiled workspaces (edges kiss, never cross) skip it.
//
// Interactions (Max, 2026-09-16): click a card → home, that window raised +
// focused; click the space between cards → just home; 3-down/Escape → home;
// 3-up again → the overview. Digits mean workspaces here too.
//
// Cards are captured STANDALONE (renderWindow's standalone flag reaches
// buried windows — a crop out of the stacked workspace snapshot would hold
// the occluder's pixels), and drawn over the same wallpaper backdrop the
// overview uses. The layout is the brain's: displacement-preserving, so the
// pile exhales instead of snapping to a grid.
struct SpreadWin {
    PHLWINDOWREF             win;
    SP<Render::IFramebuffer> fb;      // the window's own standalone capture
    Rect                     home;    // real window box (logical) — where the card rests at p=0
    Rect                     spread;  // brain-assigned slot (logical) — where it rests at p=1
    CBox                     drawCur; // eased chase of the slot, once settled (re-layouts glide)
    CBox                     screen;  // last-drawn box in draw space (hit-testing)
};
static std::vector<SpreadWin>   g_spreadWins;
static bool                     g_spreadActive = false;
static float                    g_spreadAnim = 0.0f, g_spreadTarget = 0.0f;
static Time::steady_tp          g_spreadLastT;
static int64_t                  g_spreadWs = -1; // the workspace the spread belongs to
static PHLMONITORREF            g_spreadMon;
static PHLWINDOWREF             g_spreadHover; // card under the pointer (gets the ring)
static PHLWINDOWREF             g_spreadPick;  // clicked card — glides home on top of the rest
static SP<Render::IFramebuffer> g_spreadSrcFB; // scratch: one window rendered alone, then cropped

static bool wsNeedsSpread(PHLMONITOR m);
static void cardExtents(PHLWINDOW w, double& top, double& side); // bar + border past the surface box
static void openSpread(PHLMONITOR m);
static void closeSpread();                            // glide the cards home
static void snapCloseSpread(bool tellDaemon = true);  // instant — the overview or the stage is taking the screen
static void notifyWaverunner(bool on);                // defined with the daemon channel below

// The overview or the spread is on screen: pointer input belongs to their
// CARDS, not to the desktop beneath. Golem's titlebars (src/hyprbars/) stand
// down on this — a press must never take hold of an invisible bar under the
// overlay (focus/raise/drag of a hidden window, and a swallowed release left
// the closed hand stuck; Max, 2026-09-16). Not static: the bars link against
// it.
bool waveviewOwnsScreen() {
    return g_active || g_spreadActive;
}

// ---- MINIMIZE to the dock (the orange button; Max, 2026-09-16: "same as
// macOS. (not to the icon) to the dock") --------------------------------------
// The window's decorated card FLIES into the dock while the window itself
// parks on `special:minimized`; waverunner then shows a per-window thumbnail
// entry (the deck's .rgba flow) that calls `hl.plugin.waveview.restore_min`
// on click — the card flies back out and the window returns to its
// workspace, focused.
struct MinFly {
    SP<Render::IFramebuffer> fb;      // the decorated card
    CBox                     from, to; // draw-space
    float                    t = 0.0f;
    PHLMONITORREF            mon;
    std::string              landMsg; // sent to the daemon when the card lands ("" = nothing)
};
static std::vector<MinFly> g_minFlies;
static Time::steady_tp     g_minFlyLastT;
// A finished fly's texture (window-sized, ~25MB) must NOT be released inside
// the render callback: a big synchronous GL free there dropped the wallpaper
// for one frame — the same blink freeCaptures had, and Max saw it on both the
// minimize and the restore fly (2026-09-16). The fb is parked here and freed
// on a timer between frames.
static std::vector<SP<Render::IFramebuffer>> g_minReap;
static SP<CEventLoopTimer>                   g_minReapTimer;
struct MinRec {
    std::string  addr;
    int64_t      ws = 1; // where it returns to
    PHLWINDOWREF win;
};
static std::vector<MinRec> g_minimized;
static constexpr float     MINFLY_SECONDS = 0.34f;
static constexpr int       MIN_THUMB_SIZE = 256; // == waverunner's ICON_SIZE: the .rgba drops straight into a texture layer

static std::string minThumbDir() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt ? rt : "/tmp") + "/waverunner-min";
}
static std::string windowAddr(PHLWINDOW w) {
    return std::format("0x{:x}", (uintptr_t)w.get());
}
void golemMinimize(PHLWINDOW w); // defined with the deck-capture plumbing it reuses; the bars call it

// Whether Golem's STAGE owns the screen. Defined up here because the spread
// refuses to open over it; the full story lives at its old site above
// toggle(), where waverunner sets it via set_stage.
static bool g_stageMode = false;

// Defined further down; used by the pointer handlers above their definitions.
static void jumpTo(int wsId, bool startTheClose = true);
static void beginCloseZoom(); // put the close on its curve (see `startClose`)
static void snapCloseNow();          // end the map instantly (fallback)
static void beginStageCloseZoom();   // the stage's way back: a short zoom
static void jumpToWindow(PHLWINDOW w);
static void updateHoverAt(PHLMONITOR m, const Vector2D& c);
static void reassertOverviewCursor(); // undo the compositor's border icon (defined with the cursor owner)
static void closeOverview();
static void beginRealDrag(PHLWINDOW dw, bool capture = true);
static void endRealDrag(std::optional<Vector2D> at, PHLWINDOW splitTarget = nullptr);
static void checkFloatWatch();
static void maybeCommit(PHLMONITOR m);
static void restoreFloatState(PHLWINDOW dw);

static void damageAll() {
    for (auto& m : g_pCompositor->m_monitors) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

static void renderRect(const CBox& box, const CHyprColor& color, int round = 0) {
    CRectPassElement::SRectData data;
    data.box           = box;
    data.color         = color;
    data.round         = round;
    data.roundingPower = 2.0f;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

// A rounded ring around a window box — shared by the overview's minis and
// the spread's cards so both wear the SAME paint: the compositor's own
// border pass fed the live `general:col.*_border` gradients (active for the
// hover/focus affordance, inactive for a card at rest — the desktop's own
// language). Falls back to a flat amber halo when a gradient is unreadable.
static void haloAround(PHLMONITOR m, const CBox& box, int round, bool active = true, float roundingPower = 2.0f) {
    auto* const grad = borderGradient(active);
    if (!grad) {
        const double bw = DSN_BORDER_W * m->m_scale;
        renderRect(CBox{box.x - bw, box.y - bw, box.w + 2.0 * bw, box.h + 2.0 * bw}, DSN_BORDER_COL,
                   round + (int)std::lround(bw));
        return;
    }
    CBorderPassElement::SBorderData bd;
    bd.box           = box;
    bd.grad1         = *grad;
    bd.round         = round;
    bd.roundingPower = roundingPower;
    bd.borderSize    = (int)std::lround(DSN_BORDER_W);
    bd.a             = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(std::move(bd)));
}

// Stock CTexPassElement can only stretch its whole texture into `box` — the
// stretch is what made morphing windows look mangled. Its one source-crop
// path (allowCustomUV) reads UVs from the renderer's GLOBAL render data at
// draw time, so these two EK_CUSTOM elements bracket it: the first plants
// the UVs and emits the tex element, the trailing one restores the (-1,-1)
// sentinel so nothing drawn later inherits the crop.
class CUVResetElement : public IPassElement {
  public:
    SP<Render::ITexture> tex; // whose minFilter to restore (may be null)
    CUVResetElement(SP<Render::ITexture> t) : tex(t) {}
    virtual std::vector<UP<IPassElement>> draw() {
        g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
        g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        if (tex)
            tex->minFilter = GL_LINEAR;
        return {};
    }
    virtual bool needsLiveBlur() {
        return false;
    }
    virtual bool needsPrecomputeBlur() {
        return false;
    }
    virtual const char* passName() {
        return "waveviewUVReset";
    }
    virtual ePassElementType type() {
        return EK_CUSTOM;
    }
};

class CUVTexElement : public IPassElement {
  public:
    CTexPassElement::SRenderData data;
    Vector2D                     uvTL, uvBR;
    CUVTexElement(CTexPassElement::SRenderData&& d, const Vector2D& tl, const Vector2D& br) : data(std::move(d)), uvTL(tl), uvBR(br) {}
    virtual std::vector<UP<IPassElement>> draw() {
        g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = uvTL;
        g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = uvBR;
        data.allowCustomUV                                        = true;
        // NOTE: no filter games here. This element only cover-crops small
        // aspect drifts now (seam-snapped minis); the big animated morphs
        // that needed mipmaps are retired — live commits replaced them,
        // and the per-capture glGenerateMipmap cost went with them.
        std::vector<UP<IPassElement>> out;
        out.emplace_back(makeUnique<CTexPassElement>(data));
        out.emplace_back(makeUnique<CUVResetElement>(data.tex));
        return out;
    }
    virtual bool needsLiveBlur() {
        return false;
    }
    virtual bool needsPrecomputeBlur() {
        return false;
    }
    virtual std::optional<CBox> boundingBox() {
        return data.box.copy().scale(1.F / g_pHyprRenderer->m_renderData.pMonitor->m_scale).round();
    }
    virtual const char* passName() {
        return "waveviewUVTex";
    }
    virtual ePassElementType type() {
        return EK_CUSTOM;
    }
};

// Capture just the wallpaper into g_bgFB: the built-in background plus any
// background-layer surfaces (hyprpaper etc.), nothing else. This is the single
// backdrop the whole overview floats over. Must run inside the capture flow
// (EGL current, its own beginRender/endRender), like the workspace snapshots.
static void captureBackdrop(PHLMONITOR m, const CBox& monbox) {
    if (!g_bgFB)
        g_bgFB = g_pHyprRenderer->createFB("waveview-bg");
    if (g_bgFB->m_size != monbox.size()) {
        g_bgFB->release();
        g_bgFB->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, g_bgFB);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto now = Time::steadyNow();
    g_pHyprRenderer->renderBackground(m); // Hyprland's built-in wallpaper (covered by hyprpaper if present)
    for (auto& ref : m->m_layerSurfaceLayers[0]) // background layer: hyprpaper & friends draw here
        if (const auto ls = ref.lock())
            g_pHyprRenderer->renderLayer(ls, m, now);

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
}

// Slice each mapped window out of its workspace snapshot into its own small FB,
// so it can be drawn as an individual floating rect. We crop by custom UV
// (allowCustomUV) into g_fbs[tile] — the window's normalized rect within the full
// monitor-resolution snapshot — rather than re-rendering the window, so this
// reuses the proven workspace capture and needs no standalone-window render.
static void captureWindows(PHLMONITOR m, uint32_t mask = ALL_TILES) {
    // Remember each window's last-drawn hit-box so pointer hit-testing keeps
    // working across the rebuild — otherwise the fresh entries carry a zeroed
    // box until the next drawOverview, and a pointer move in that gap drops the
    // hover (the border blinks ~every REFRESH_MS).
    struct Carry {
        CBox                     screen;
        CBox                     drawCur;
        SP<Render::IFramebuffer> fb; // reused next cycle — see below
    };
    std::vector<std::pair<PHLWINDOWREF, Carry>> prevBoxes;
    prevBoxes.reserve(g_wins.size());
    // Mid-drag, the dragged window is parked offscreen — recropping it
    // would blank the cursor ghost. Stash its whole capture and reuse it.
    std::optional<CapWin> stashDragged;
    for (auto& cw : g_wins) {
        const bool isDragged = g_dragReal && cw.win.lock() && cw.win.lock() == g_dragWin.lock();
        // Each window's FB rides along to the rebuild: creating a fresh GL
        // framebuffer per window on EVERY capture (up to 20/s each during
        // the boost) churned the driver into a progressive mid-drag
        // slowdown ("after a few movements all gets slowed down").
        prevBoxes.emplace_back(cw.win, Carry{cw.screen, cw.drawCur, isDragged ? nullptr : cw.fb});
        if (isDragged)
            stashDragged = cw; // keep its fb alive
    }
    g_wins.clear();

    const double scale = m->m_scale;
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
        if (stashDragged && w == g_dragWin.lock()) {
            g_wins.push_back(*stashDragged); // parked offscreen — reuse its capture
            continue;
        }
        const int tile = waveview_tile_for_workspace(w->workspaceID());
        if (tile < 0 || !g_fbs[tile])
            continue;
        const auto srcTex = g_fbs[tile]->getTexture();
        if (!srcTex)
            continue;

        const CBox wb = w->getWindowMainSurfaceBox(); // logical coords, CURRENT (pixels live here)
        if (wb.w <= 1.0 || wb.h <= 1.0)
            continue;
        // The tile mapping uses the spring GOAL, not the mid-flight value:
        // sampling in-flight geometry at capture cadence made minis flip
        // across snap/seam thresholds between snapshots ("jumps"). Goals
        // move exactly once per commit; the draw-side glide animates the
        // transition at full frame rate. (Delta on top of the surface box,
        // so surface-vs-frame offsets stay whatever they were.)
        const Vector2D gdp = w->m_realPosition->goal() - w->m_realPosition->value();
        const Vector2D gds = w->m_realSize->goal() - w->m_realSize->value();

        // The window's rect as normalized UV within the full-workspace snapshot.
        const double u0 = std::clamp((wb.x - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v0 = std::clamp((wb.y - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        const double u1 = std::clamp((wb.x + wb.w - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v1 = std::clamp((wb.y + wb.h - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        if (u1 - u0 <= 0.0 || v1 - v0 <= 0.0)
            continue; // fully offscreen

        CapWin cw;
        cw.win     = w;
        cw.logical = Rect{wb.x + gdp.x, wb.y + gdp.y, wb.w + gds.x, wb.h + gds.y};
        cw.tile    = tile;
        cw.active  = g_pCompositor->isWindowActive(w);

        // STAGE 2: a tile that hides windows shows its COLLAGE — the mini
        // sits at its spread slot, and its pixels come from its SOLO capture
        // (the snapshot holds the occluder's pixels at a buried rect).
        SP<Render::ITexture> src = srcTex;
        Vector2D             uvTL{u0, v0}, uvBR{u1, v1};
        if (const Rect* slot = tileSpreadSlot(tile, w)) {
            cw.spread = true;
            cw.slot   = *slot; // `logical` keeps the TRUE box — see the field's warning
            if (auto* sc = soloFor(w, false); sc && sc->fb)
                if (const auto st = sc->fb->getTexture()) {
                    // The solo is CARD-sized (bar + border baked, for the
                    // landing act's chrome fade); the mini takes only the
                    // content region, so level 2 stays bare.
                    src = st;
                    double top = 0.0, side = 0.0;
                    cardExtents(w, top, side);
                    const double cbw = wb.w + 2.0 * side, cbh = wb.h + top + side;
                    uvTL = {side / cbw, top / cbh};
                    uvBR = {(side + wb.w) / cbw, (top + wb.h) / cbh};
                }
        }

        const int fbw = std::max(1, (int)std::lround(wb.w * scale));
        const int fbh = std::max(1, (int)std::lround(wb.h * scale));
        for (auto& pb : prevBoxes)
            if (pb.first.lock() == w && pb.second.fb) {
                cw.fb = std::move(pb.second.fb); // last cycle's FB, storage intact
                break;
            }
        // Mid-spring, windows genuinely OVERLAP in the workspace snapshot
        // (the landed window slides over the target's kept half; siblings
        // cross while re-tiling) — a crop taken then contains slivers of
        // the neighbour ("the content gets mixed"). While this window
        // overlaps any sibling, hold its last clean crop; the refresh
        // resumes the moment they separate. A SOLO-sourced window skips all
        // of this: rendered alone, its crop is clean by construction (and a
        // spread tile's windows overlap by definition — the hold would
        // freeze them forever).
        bool overlapped = false;
        if (!cw.spread)
            for (auto& o : g_pCompositor->m_windows) {
            if (!o || o == w || !o->m_isMapped || o->isHidden() || o->workspaceID() != w->workspaceID())
                continue;
            const CBox   ob = o->getWindowMainSurfaceBox();
            const double ix = std::min(wb.x + wb.w, ob.x + ob.w) - std::max(wb.x, ob.x);
            const double iy = std::min(wb.y + wb.h, ob.y + ob.h) - std::max(wb.y, ob.y);
            if (ix > 1.0 && iy > 1.0) {
                overlapped = true;
                break;
            }
        }

        // Off-mask windows keep last cycle's crop untouched (their tile's
        // snapshot wasn't re-rendered either) — unless the FB is missing or
        // wrong-sized, which forces a fresh crop regardless.
        bool fresh = (mask & (1u << tile)) != 0;
        if (!cw.fb) {
            cw.fb = g_pHyprRenderer->createFB("waveview-win");
            cw.fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888);
            fresh      = true;
            overlapped = false; // no clean crop to keep — even a mixed one beats a blank
        } else if (overlapped) {
            fresh = false; // hold the last clean crop (size drift bridged by cover-crop)
        } else if (cw.fb->m_size != Vector2D(fbw, fbh)) {
            cw.fb->release();
            cw.fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888);
            fresh = true;
        }

        if (fresh) {
            CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
            g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, cw.fb);
            glClearColor(0.0F, 0.0F, 0.0F, 0.0F); // transparent: only the window's pixels
            glClear(GL_COLOR_BUFFER_BIT);

            Render::GL::CHyprOpenGLImpl::STextureRenderData td;
            td.allowCustomUV               = true;
            td.primarySurfaceUVTopLeft     = uvTL;
            td.primarySurfaceUVBottomRight = uvBR;
            Render::GL::g_pHyprOpenGL->renderTexture(src, CBox{0.0, 0.0, (double)fbw, (double)fbh}, td);

            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
        }

        for (auto& pb : prevBoxes) // carry hit-box + slot glide forward across the rebuild
            if (pb.first.lock() == w) {
                cw.screen  = pb.second.screen;
                cw.drawCur = pb.second.drawCur;
                break;
            }

        g_wins.push_back(std::move(cw));
    }
    for (auto& pb : prevBoxes)
        if (pb.second.fb)
            pb.second.fb->release(); // window vanished: free its texture storage
}

// Render each of the 9 workspaces into its own framebuffer thumbnail. This does
// its own beginRender/endRender per workspace, so it MUST run outside the
// monitor's render pass (we call it from the toggle handler, never from a render
// stage) — nesting render passes corrupts GL state. Snapshots are captured at
// tile resolution; CTexPassElement rescales them to the tile box on draw, so the
// capture size only affects sharpness, never layout. Mirrors hyprexpo's proven
// flow for the 0.55 render API.
// The monitor's reserved top strip (the OPTIONS topbar's exclusive zone) in
// draw-space px. The overview keeps the bar alive in that strip: tiles are
// laid out below it, so the bar gets its own place instead of overlapping
// the top row. (Draw space = transformed pixels; the reserve is logical.)
static double topInset(PHLMONITOR m) {
    return std::round(m->m_reservedArea.top() * m->m_scale);
}

// Is the pointer over the OPTIONS bar's reserved strip? The overview keeps
// the bar alive up there (tiles are laid out below it), so its pills must
// stay hoverable and clickable — the X is the overview's exit. We swallow
// every pointer event while open, so without this the bar would be visible
// but dead (Max, 2026-08-31: "the whole options click does not work on
// overview"). Logical coords: the cursor and the reserve both are.
static bool inTopbarStrip(PHLMONITOR m) {
    if (!m)
        return false;
    const double inset = m->m_reservedArea.top();
    if (inset <= 0.0)
        return false;
    const auto c = g_pInputManager->getMouseCoordsInternal();
    return c.x >= m->m_position.x && c.x < m->m_position.x + m->m_size.x && c.y >= m->m_position.y && c.y < m->m_position.y + inset;
}

// The monitor's USABLE logical area — position/size minus every reserved
// strip. Windows are mapped into tiles against THIS, not the full monitor:
// mapping against the full monitor bakes the bar strip into every tile as
// a dead band no window can ever occupy (the "out gaps" that survived four
// rounds of seam logic — a maximized window must BE the full tile).
struct SUsable {
    double x, y, w, h;
};
static SUsable usableArea(PHLMONITOR m) {
    const double l = m->m_reservedArea.left(), r = m->m_reservedArea.right();
    const double t = m->m_reservedArea.top(), b = m->m_reservedArea.bottom();
    return {m->m_position.x + l, m->m_position.y + t, std::max(1.0, m->m_size.x - l - r),
            std::max(1.0, m->m_size.y - t - b)};
}

// 3x6 tiles below the reserved strip plus the design's top-gap, shifted up
// by the current grid scroll — the ONE tile source shared by draw, capture,
// hit-testing, and the schematic, so they can't disagree.
static int computeTiles(PHLMONITOR m, Rect* out) {
    const double s   = m->m_scale;
    const double top = topInset(m) + std::round(DSN_TOP_GAP * s);
    const int    n   = waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, top,
                                                DSN_GAP * s, DSN_OUTER * s, out);
    for (int i = 0; i < n && i < N_TILES; ++i)
        out[i].y -= g_scroll;
    return n;
}

// The scroll distance of one page flip: page 2's first row lands exactly
// where page 1's did (row 3's unscrolled y minus row 0's).
static double pageStep(PHLMONITOR m) {
    const double saved = g_scroll;
    g_scroll           = 0.0;
    Rect tiles[N_TILES];
    const int n = computeTiles(m, tiles);
    g_scroll    = saved;
    return n == N_TILES ? tiles[9].y - tiles[0].y : 0.0;
}

// Debug trace for the drag rounds (Max reproduces, we read the log):
// appended to /tmp/waveview-trace.log, ms since plugin load.
static void trace(const char* fmt, ...) {
    static FILE* f = fopen("/tmp/waveview-trace.log", "a");
    if (!f)
        return;
    static const auto t0 = Time::steadyNow();
    fprintf(f, "%9.1f ", std::chrono::duration<double, std::milli>(Time::steadyNow() - t0).count());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

// `mask`: which tiles to re-render. A commit touches at most two workspaces;
// re-rendering all 18 at the 20fps boost (~360 workspace renders/s) is what
// froze the drag after a few movements. Skipped tiles keep their snapshot.
static void captureWorkspaces(PHLMONITOR m, uint32_t mask = ALL_TILES) {
    if (!m)
        return;

    Rect tiles[N_TILES];
    if (computeTiles(m, tiles) < N_TILES)
        return;
    const auto capT0 = Time::steadyNow();

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    // Render each workspace at FULL monitor resolution; CTexPassElement scales the
    // texture down into the tile on draw. Rendering into a tile-sized box instead
    // makes renderWorkspace clip surfaces (only decorations survive) — so don't.
    const CBox monbox{0.0, 0.0, m->m_pixelSize.x, m->m_pixelSize.y};
    const auto startedOn = m->m_activeWorkspace;

    g_pHyprRenderer->m_bBlockSurfaceFeedback = true; // don't send frame callbacks for the fake render
    g_capturing                              = true; // suppress our own render hook while we render into thumbnails
    m->m_solitaryClient.reset(); // clear the "one fullscreen window covers all" optimization, else renderWorkspace draws only that window (Hyprland recomputes it next frame)
    if (startedOn)
        startedOn->m_visible = false; // hide the real active ws; otherwise its on-screen windows bleed into every tile

    // Occupancy: an empty workspace's snapshot is never sampled (the
    // backdrop is g_bgFB; minis crop from their OWN tile's snapshot), so
    // rendering it is pure waste — and each FB is monitor-res ABGR
    // (~25MB at 3200x2000). Empty tiles are skipped AND their FBs freed;
    // an occupied tile with no valid FB is captured even off-mask (a
    // window can land on a previously-empty view mid-boost — without the
    // force it would vanish from the grid until the next full pass).
    uint32_t occupied = 0;
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
        if (const int t = waveview_tile_for_workspace(w->workspaceID()); t >= 0)
            occupied |= (1u << t);
    }

    // STAGE 2: decide which tiles show their collage, and where every window
    // of those tiles sits — before any pixels move, so the snapshot loop can
    // solo-capture exactly the windows the collage needs.
    sweepSolos();
    computeTileSpreads(m, mask, occupied);

    for (int i = 0; i < N_TILES; ++i) {
        auto& fb = g_fbs[i];
        if (!(occupied & (1u << i))) {
            if (fb) {
                fb->release();
                fb.reset();
            }
            continue;
        }
        if (!(mask & (1u << i)) && fb && fb->m_size == monbox.size())
            continue; // not dirty: last snapshot stands
        if (!fb)
            fb = g_pHyprRenderer->createFB("waveview");
        if (fb->m_size != monbox.size()) {
            fb->release();
            fb->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888);
        }

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, fb);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        if (const auto ws = g_pCompositor->getWorkspaceByID(i + 1)) {
            m->m_activeWorkspace = ws; // renderWorkspace draws the monitor's active ws
            // Snap this workspace's windows to their on-screen positions (non-active
            // workspaces are parked offscreen), else renderWorkspace captures nothing
            // of it. instant=true so the real desktop doesn't visibly animate.
            g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
            ws->m_visible = true;
            g_pHyprRenderer->renderWorkspace(m, ws, Time::steadyNow(), monbox);
            ws->m_visible = false;
            g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
        }

        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        // A hiding tile also needs each of its windows ALONE (the snapshot
        // above stacked them). Same on-screen juggling, but OUTSIDE the
        // snapshot's render pass — nesting render passes corrupts GL.
        if (g_tileSpread[i].on)
            if (const auto ws = g_pCompositor->getWorkspaceByID(i + 1)) {
                m->m_activeWorkspace = ws;
                g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
                ws->m_visible = true;
                captureSolosForTile(m, i, monbox);
                ws->m_visible = false;
                g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
            }
    }

    // With the workspace snapshots ready, build the two things we actually
    // draw: the single wallpaper backdrop (skipped on partial captures —
    // wallpaper doesn't change mid-drag), and one cropped texture per window.
    if (mask == ALL_TILES || !g_bgFB)
        captureBackdrop(m, monbox);
    captureWindows(m, mask);

    g_capturing                              = false;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
    m->m_activeWorkspace                     = startedOn;
    if (startedOn) {
        startedOn->m_visible = true; // restore the real active workspace on the live desktop
        g_pDesktopAnimationManager->startAnimation(startedOn, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
    }

    // Send frame events to every workspace's clients so backgrounded apps keep
    // producing frames — otherwise their thumbnails would freeze (Wayland only
    // renders visible surfaces). This is what makes all tiles live, at the cost
    // of keeping background apps awake while the overview is open.
    for (int i = 0; i < N_TILES; ++i)
        if (const auto ws = g_pCompositor->getWorkspaceByID(i + 1))
            g_pHyprRenderer->sendFrameEventsToWorkspace(m, ws, Time::steadyNow());

    g_captureMon = m;
    trace("capture mask=%05x wins=%zu dur=%.1fms", mask, g_wins.size(),
          std::chrono::duration<double, std::milli>(Time::steadyNow() - capT0).count());
}

// Release every capture buffer: the workspace snapshots, the backdrop, and
// the per-window crops — roughly half a GB of VRAM at 3200x2000. Runs at
// full-close (the overview holds GPU memory only while it's on screen; the
// reopen recaptures everything anyway) and at unload. Safe mid-frame: the
// render pass was cleared at this frame's beginRender, so no queued element
// still references these textures.
static void freeCaptures() {
    for (auto& fb : g_fbs) {
        if (fb)
            fb->release();
        fb.reset();
    }
    if (g_bgFB) {
        g_bgFB->release();
        g_bgFB.reset();
    }
    for (auto& cw : g_wins)
        if (cw.fb)
            cw.fb->release();
    g_wins.clear();
    for (auto& sc : g_solo)
        if (sc.fb)
            sc.fb->release();
    g_solo.clear();
    if (g_spreadSrcFB) { // shared with the level-1 spread; both free it guarded
        g_spreadSrcFB->release();
        g_spreadSrcFB.reset();
    }
}

// ---- The spread's machinery ---------------------------------------------------
// (State and the ladder story live with the globals near the top.)

// The spread's cast, in draw order: tiled first, then floating (the desktop's
// own stacking), the fullscreen window last — it covers everything out there,
// so its card rides on top in here. Pinned floats show on every workspace and
// join the cast.
static std::vector<PHLWINDOW> spreadPopulation(PHLMONITOR m) {
    std::vector<PHLWINDOW> tiled, floating;
    PHLWINDOW              fs;
    const auto             wsid = m->activeWorkspaceID();
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->m_fadingOut || w->monitorID() != m->m_id)
            continue;
        if (w->workspaceID() != wsid && !w->m_pinned)
            continue;
        if (w->isFullscreen())
            fs = w;
        else
            (w->m_isFloating ? floating : tiled).push_back(w);
    }
    tiled.insert(tiled.end(), floating.begin(), floating.end());
    if (fs)
        tiled.push_back(fs);
    return tiled;
}

// The window's box in logical coords, aimed at where it is GOING (spring
// goals) — the same trick CapWin::logical uses, so a mid-spring desktop
// spreads from its landed state, not a transient.
static Rect spreadHome(PHLWINDOW w) {
    const CBox     wb  = w->getWindowMainSurfaceBox();
    const Vector2D gdp = w->m_realPosition->goal() - w->m_realPosition->value();
    const Vector2D gds = w->m_realSize->goal() - w->m_realSize->value();
    return Rect{wb.x + gdp.x, wb.y + gdp.y, wb.w + gds.x, wb.h + gds.y};
}

// How far a spread CARD extends past the surface box: Golem's bar above a
// float, the border all around; a fullscreen window is bare. The card is the
// window AS IT LOOKS on the desktop (Max, 2026-09-16: "the windows [should]
// have the bar on [the spread]; the bars should vanish only on the
// overview") — the 3x3's minis stay bare surface crops.
static void cardExtents(PHLWINDOW w, double& top, double& side) {
    if (w->isFullscreen()) {
        top  = 0.0;
        side = 0.0;
        return;
    }
    side = (double)w->getRealBorderSize();
    top  = side + (w->m_isFloating ? (double)GOLEM_BAR_HEIGHT : 0.0);
}

// The level-1 spread's unit of layout, capture and hit-testing: the whole
// card. (Stage 2 keeps using [`spreadHome`] — its tile minis are bar-less, so
// slots from surface boxes keep slot and pixels the same aspect.)
static Rect spreadCard(PHLWINDOW w) {
    Rect   r = spreadHome(w);
    double top = 0.0, side = 0.0;
    cardExtents(w, top, side);
    return Rect{r.x - side, r.y - top, r.w + 2.0 * side, r.h + top + side};
}

// STAGE 2's layout: which tiles hide windows, and where each window sits
// once spread. Slots are computed in the SAME space as the level-1 spread
// (the monitor's usable area, same margins, same gap), so a tile shows the
// very collage the spread shows full-screen — the escalation morph is a pure
// scale. Off-mask tiles keep their collage exactly like they keep their
// snapshot; a mid-drag window is out of its collage like it is out of its
// tile (its parked-offscreen box would poison the layout).
static void computeTileSpreads(PHLMONITOR m, uint32_t mask, uint32_t occupied) {
    for (int i = 0; i < N_TILES; ++i) {
        if (!(mask & (1u << i)) && g_fbs[i])
            continue; // not dirty: last collage stands with the last snapshot
        auto& ts = g_tileSpread[i];
        ts.on    = false;
        ts.slots.clear();
        if (!(occupied & (1u << i)))
            continue;
        std::vector<PHLWINDOW> wins;
        for (auto& w : g_pCompositor->m_windows) {
            if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
                continue;
            if (waveview_tile_for_workspace(w->workspaceID()) != i)
                continue;
            if (g_dragReal && w == g_dragWin.lock())
                continue;
            wins.push_back(w);
        }
        if (wins.size() < 2)
            continue;
        std::vector<Rect> boxes;
        boxes.reserve(wins.size());
        for (const auto& w : wins)
            boxes.push_back(spreadHome(w));
        if (!waveview_needs_spread(boxes.data(), (int)boxes.size()))
            continue;
        std::vector<Rect> slots(wins.size());
        const SUsable     u = usableArea(m);
        waveview_spread_layout(boxes.data(), (int)boxes.size(), u.x + DSN_SPREAD_MARGIN, u.y + DSN_SPREAD_MARGIN,
                               std::max(1.0, u.w - 2.0 * DSN_SPREAD_MARGIN), std::max(1.0, u.h - 2.0 * DSN_SPREAD_MARGIN),
                               DSN_SPREAD_GAP, slots.data());
        ts.on = true;
        for (size_t k = 0; k < wins.size(); ++k)
            ts.slots.emplace_back(wins[k], slots[k]);
    }
}

// Render each window of a hiding tile ALONE (standalone reaches buried
// windows) into the shared monitor scratch, then crop it into its own solo
// texture. The caller has the workspace snapped on-screen and visible, and
// calls from OUTSIDE any render pass.
static void captureSolosForTile(PHLMONITOR m, int tile, const CBox& monbox) {
    if (!g_spreadSrcFB)
        g_spreadSrcFB = g_pHyprRenderer->createFB("waveview-spread-src");
    if (g_spreadSrcFB->m_size != monbox.size()) {
        g_spreadSrcFB->release();
        g_spreadSrcFB->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888);
    }
    const double scale = m->m_scale;
    for (auto& [ref, slot] : g_tileSpread[tile].slots) {
        const auto w = ref.lock();
        if (!w)
            continue;
        const CBox wb = w->getWindowMainSurfaceBox();
        if (wb.w <= 1.0 || wb.h <= 1.0)
            continue;

        // Normally DECORATED and card-sized: the solo carries the window's
        // bar and border, so the close's landing act can fade that chrome in
        // (the "bars coming back" fix). BUT a window fully COVERED by a
        // fullscreen sibling has effectiveAlpha 0, and renderWindow SKIPS an
        // alpha-0 window unless it is `standalone` — so its solo came back
        // blank and the overview tile dropped it entirely (Max, 2026-09-16:
        // "i go from stage to overview and there is no windows"). Render a
        // covered window STANDALONE (bare, but it appears); visible windows
        // keep the decorated path.
        const bool covered = w->effectiveAlpha() <= 0.001F;
        CRegion    fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, g_spreadSrcFB);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprRenderer->renderWindow(w, m, Time::steadyNow(), !covered, Render::RENDER_PASS_ALL, false, covered);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        double top = 0.0, side = 0.0;
        cardExtents(w, top, side);
        const CBox   cb{wb.x - side, wb.y - top, wb.w + 2.0 * side, wb.h + top + side};
        const double u0 = std::clamp((cb.x - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v0 = std::clamp((cb.y - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        const double u1 = std::clamp((cb.x + cb.w - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v1 = std::clamp((cb.y + cb.h - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        if (u1 - u0 <= 0.0 || v1 - v0 <= 0.0)
            continue;
        const auto srcTex = g_spreadSrcFB->getTexture();
        if (!srcTex)
            continue;

        const int fbw = std::max(1, (int)std::lround(cb.w * scale));
        const int fbh = std::max(1, (int)std::lround(cb.h * scale));
        auto*     sc  = soloFor(w, true);
        if (!sc->fb)
            sc->fb = g_pHyprRenderer->createFB("waveview-solo");
        if (sc->fb->m_size != Vector2D(fbw, fbh)) {
            sc->fb->release();
            sc->fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888);
        }

        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, sc->fb);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        Render::GL::CHyprOpenGLImpl::STextureRenderData td;
        td.allowCustomUV               = true;
        td.primarySurfaceUVTopLeft     = Vector2D(u0, v0);
        td.primarySurfaceUVBottomRight = Vector2D(u1, v1);
        Render::GL::g_pHyprOpenGL->renderTexture(srcTex, CBox{0.0, 0.0, (double)fbw, (double)fbh}, td);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
    }
}

// Whether the current workspace hides anything — i.e. whether the spread rung
// exists on this monitor right now. Tiled layouts (edges kiss, never cross)
// and lone windows answer no, and the swipe goes straight to the overview.
static bool wsNeedsSpread(PHLMONITOR m) {
    if (!m)
        return false;
    const auto pop = spreadPopulation(m);
    if (pop.size() < 2)
        return false;
    std::vector<Rect> boxes;
    boxes.reserve(pop.size());
    for (const auto& w : pop)
        boxes.push_back(spreadCard(w)); // a bar hiding something is hiding too
    return waveview_needs_spread(boxes.data(), (int)boxes.size()) != 0;
}

// (Re)build g_spreadWins from a population: fresh homes, a fresh brain
// layout, captures and glide state carried across by window — so a
// mid-spread re-layout (a window opened or closed) glides instead of
// flashing blank.
static void layoutSpread(PHLMONITOR m, const std::vector<PHLWINDOW>& pop) {
    const size_t      n = pop.size();
    std::vector<Rect> homes(n), slots(n);
    for (size_t i = 0; i < n; ++i)
        homes[i] = spreadCard(pop[i]); // the CARD (bar + border) is the layout unit

    const SUsable u = usableArea(m);
    waveview_spread_layout(homes.data(), (int)n, u.x + DSN_SPREAD_MARGIN, u.y + DSN_SPREAD_MARGIN,
                           std::max(1.0, u.w - 2.0 * DSN_SPREAD_MARGIN), std::max(1.0, u.h - 2.0 * DSN_SPREAD_MARGIN),
                           DSN_SPREAD_GAP, slots.data());

    std::vector<SpreadWin> next(n);
    for (size_t i = 0; i < n; ++i) {
        next[i].win    = pop[i];
        next[i].home   = homes[i];
        next[i].spread = slots[i];
        for (auto& old : g_spreadWins)
            if (old.win.lock() == pop[i]) {
                next[i].fb      = std::move(old.fb); // keep the texture storage (churn lesson from captureWindows)
                next[i].drawCur = old.drawCur;
                next[i].screen  = old.screen;
                break;
            }
    }
    for (auto& old : g_spreadWins)
        if (old.fb)
            old.fb->release(); // window left the cast: free its texture
    g_spreadWins = std::move(next);
}

// Capture every card's true pixels. renderWindow with `standalone` reaches a
// BURIED window (alpha forced opaque, corners square — we round at draw), and
// rendering into a monitor-sized scratch then cropping by UV is the proven
// flow from captureWindows — smaller render targets clip surfaces. Runs
// outside the render pass, like every capture here.
static void captureSpreadTextures(PHLMONITOR m, bool alsoBackdrop) {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    const CBox monbox{0.0, 0.0, m->m_pixelSize.x, m->m_pixelSize.y};
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    g_capturing                              = true;
    m->m_solitaryClient.reset(); // else a fullscreen window would be the only thing renderWindow ever draws

    if (!g_spreadSrcFB)
        g_spreadSrcFB = g_pHyprRenderer->createFB("waveview-spread-src");
    if (g_spreadSrcFB->m_size != monbox.size()) {
        g_spreadSrcFB->release();
        g_spreadSrcFB->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888);
    }

    const double scale = m->m_scale;
    for (auto& sw : g_spreadWins) {
        const auto w = sw.win.lock();
        if (!w)
            continue;
        const CBox wb = w->getWindowMainSurfaceBox();
        if (wb.w <= 1.0 || wb.h <= 1.0)
            continue;

        // The window alone, at its real position, into the scratch — with
        // its DECORATIONS: the card is the window as it looks (bar, border,
        // baked corners; Max, 2026-09-16). `standalone` hard-disables
        // decorations, so this is a plain decorated render; it still reaches
        // a buried window (occlusion lives in damage/pass logic, not here) —
        // EXCEPT a window whose alpha is 0 (covered by a fullscreen sibling),
        // which renderWindow skips unless `standalone`; render those bare so
        // they still appear (the overview's covered-window fix, same class).
        const bool covered = w->effectiveAlpha() <= 0.001F;
        CRegion    fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, g_spreadSrcFB);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprRenderer->renderWindow(w, m, Time::steadyNow(), !covered, Render::RENDER_PASS_ALL, false, covered);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        // Crop the CARD box — bar strip and border included — into its own
        // texture (UV within the monitor). The shadow stays outside the
        // crop; what little enters the corner cutouts reads as depth.
        double top = 0.0, side = 0.0;
        cardExtents(w, top, side);
        const CBox   cb{wb.x - side, wb.y - top, wb.w + 2.0 * side, wb.h + top + side};
        const double u0 = std::clamp((cb.x - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v0 = std::clamp((cb.y - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        const double u1 = std::clamp((cb.x + cb.w - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v1 = std::clamp((cb.y + cb.h - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        if (u1 - u0 <= 0.0 || v1 - v0 <= 0.0)
            continue;
        const auto srcTex = g_spreadSrcFB->getTexture();
        if (!srcTex)
            continue;

        const int fbw = std::max(1, (int)std::lround(cb.w * scale));
        const int fbh = std::max(1, (int)std::lround(cb.h * scale));
        if (!sw.fb)
            sw.fb = g_pHyprRenderer->createFB("waveview-spread-win");
        if (sw.fb->m_size != Vector2D(fbw, fbh)) {
            sw.fb->release();
            sw.fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888);
        }

        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, sw.fb);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        Render::GL::CHyprOpenGLImpl::STextureRenderData td;
        td.allowCustomUV               = true;
        td.primarySurfaceUVTopLeft     = Vector2D(u0, v0);
        td.primarySurfaceUVBottomRight = Vector2D(u1, v1);
        Render::GL::g_pHyprOpenGL->renderTexture(srcTex, CBox{0.0, 0.0, (double)fbw, (double)fbh}, td);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
    }

    if (alsoBackdrop || !g_bgFB)
        captureBackdrop(m, monbox);

    g_capturing                              = false;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

    // Buried clients only keep painting if someone asks for frames — the
    // whole point of the spread is watching them come out alive.
    if (const auto ws = g_pCompositor->getWorkspaceByID(g_spreadWs))
        g_pHyprRenderer->sendFrameEventsToWorkspace(m, ws, Time::steadyNow());
}

// Release everything the spread holds. Its VRAM lifetime mirrors the
// overview's: held only while on screen.
static void freeSpread() {
    for (auto& sw : g_spreadWins)
        if (sw.fb)
            sw.fb->release();
    g_spreadWins.clear();
    if (g_spreadSrcFB) {
        g_spreadSrcFB->release();
        g_spreadSrcFB.reset();
    }
    // The backdrop is shared with the overview; only drop it when the
    // overview isn't the one still using it.
    if (!g_active && g_bgFB) {
        g_bgFB->release();
        g_bgFB.reset();
    }
}

// The spread reached fully-closed (or something snapped it away): forget it.
// `tellDaemon = false` is the escalation hand-off: the daemon's conceal
// (dock hidden) carries straight into the overview's own overview-on — an
// off/on pair on two detached socket threads could land out of order.
static void finishSpreadClose(bool tellDaemon = true) {
    if (tellDaemon)
        notifyWaverunner(false); // the dock comes back with the desktop
    g_spreadActive = false;
    g_spreadTarget = 0.0f;
    g_spreadAnim   = 0.0f;
    g_spreadWs     = -1;
    g_spreadHover.reset();
    g_spreadPick.reset();
    g_spreadMon.reset();
    // Same deferral as the overview's close: freeing the cards' VRAM at the
    // hand-off frame is what blinked the wallpaper.
    g_freePending = true;
    if (!g_active && g_liveTimer)
        g_liveTimer->updateTimeout(std::chrono::milliseconds(450));
}

static void openSpread(PHLMONITOR m) {
    if (g_spreadActive || g_active || g_stageMode || !m)
        return;
    const auto pop = spreadPopulation(m);
    if (pop.size() < 2)
        return;
    g_spreadWs  = m->activeWorkspaceID();
    g_spreadMon = m;
    g_spreadWins.clear();
    layoutSpread(m, pop);
    captureSpreadTextures(m, true);
    g_spreadActive = true;
    g_freePending  = false; // reopened before the deferred free: the captures are live again
    g_spreadAnim   = 0.0f;
    g_spreadTarget = 1.0f;
    g_spreadLastT  = Time::steadyNow();
    g_spreadHover.reset();
    g_spreadPick.reset();
    notifyWaverunner(true); // the dock hides while we own the screen, exactly like the overview
    if (g_liveTimer)
        g_liveTimer->updateTimeout(REFRESH_MS);
    trace("spread open ws=%d wins=%zu", (int)g_spreadWs, g_spreadWins.size());
    damageAll();
}

// The spread's progress curve, per DIRECTION. Opening eases OUT of the pile
// (fast leave, gentle arrival at the collage: 1-(1-a)³). Closing must ease
// out INTO the pile (a³: fast leave, gentle landing) — running the opening
// curve backwards is ease-IN: the cards barely move, then cover everything
// in the last frames and hit home at full speed, straight into the overlay
// drop. That was Max's "too aggressive… I guess there is no animation at
// all" (2026-09-16): all the motion lived in three frames before a cut.
static float spreadProgress() {
    return g_spreadTarget >= 0.5f ? easeOutCubic(g_spreadAnim) : (float)easeInOutCubic(g_spreadAnim);
}

static void closeSpread() {
    if (!g_spreadActive || g_spreadTarget <= 0.0f)
        return;
    // Mid-open retarget: the two directions ride different curves, so map
    // the CURRENT progress onto the closing curve — the cards turn around
    // from exactly where they are, no teleport.
    g_spreadAnim   = invEaseInOutCubic(easeOutCubic(g_spreadAnim));
    g_spreadTarget = 0.0f;
    g_spreadLastT  = Time::steadyNow();
    trace("spread close (glide)");
    damageAll();
}

static void snapCloseSpread(bool tellDaemon) {
    if (!g_spreadActive)
        return;
    trace("spread close (snap)");
    finishSpreadClose(tellDaemon);
    damageAll();
}

// A card was clicked: THIS window, on top, focused — the payoff interaction.
// The desktop change happens NOW, under the covering backdrop; the cards then
// glide home over an already-correct desktop, the picked one riding on top.
static void pickSpread(PHLWINDOW w) {
    if (!w) {
        closeSpread();
        return;
    }
    // A fullscreen sibling would keep the pick buried — drop the fullscreen
    // (the window keeps running; picking it itself just closes over it).
    for (auto& sw : g_spreadWins)
        if (const auto o = sw.win.lock(); o && o != w && o->isFullscreen()) {
            g_pCompositor->setWindowFullscreenInternal(o, FSMODE_NONE);
            sw.home = spreadHome(o); // its box just changed; glide home to the new one
        }
    g_pCompositor->changeWindowZOrder(w, true);
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    g_spreadPick = w;
    trace("spread pick ws=%d class=%s", (int)w->workspaceID(), w->fetchClass().c_str());
    closeSpread();
}

// The live-timer's spread tick: the desktop may have moved on (workspace
// switched under us → fold instantly; a window opened/closed → re-layout and
// glide), and the cards' pixels stay fresh.
static void refreshSpread(PHLMONITOR m) {
    if (m->activeWorkspaceID() != g_spreadWs) {
        snapCloseSpread();
        return;
    }
    const auto pop = spreadPopulation(m);
    if (pop.size() < 2) {
        closeSpread(); // a lone window hides nothing; nothing left to hold open
        return;
    }
    bool changed = pop.size() != g_spreadWins.size();
    if (!changed)
        for (size_t i = 0; i < pop.size(); ++i)
            if (g_spreadWins[i].win.lock() != pop[i]) {
                changed = true;
                break;
            }
    if (changed)
        layoutSpread(m, pop);
    captureSpreadTextures(m, false);
}

// Schematic fallback: dark tiles + each live window mapped into its workspace
// tile (focused window highlighted). Used on monitors we haven't captured
// thumbnails for. Tiles are pixel-space; window boxes are logical — the brain
// reconciles the two.
static void drawSchematic(PHLMONITOR m, const Rect tiles[N_TILES]) {
    for (int i = 0; i < N_TILES; ++i)
        renderRect(CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}, CHyprColor(0.0, 0.0, 0.0, 0.35));

    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
        const int ti = waveview_tile_for_workspace(w->workspaceID());
        if (ti < 0)
            continue;

        const CBox    wb = w->getWindowMainSurfaceBox();
        Rect          mini;
        const SUsable u = usableArea(m);
        waveview_map_window(tiles[ti].x, tiles[ti].y, tiles[ti].w, tiles[ti].h, u.x, u.y, u.w, u.h, wb.x, wb.y, wb.w,
                            wb.h, &mini);
        if (mini.w <= 0.0 || mini.h <= 0.0)
            continue;

        const bool active = g_pCompositor->isWindowActive(w);
        renderRect(CBox{mini.x, mini.y, mini.w, mini.h},
                   active ? CHyprColor(0.40, 0.70, 1.0, 0.90) : CHyprColor(0.85, 0.85, 0.90, 0.80));
    }
}

static int  tileAt(PHLMONITOR m, const Vector2D& c);
static bool tileEmpty(int tile);

// Draw the overview onto the current monitor at zoom progress `p` (0 = zoomed
// into `zoomTile`, 1 = full grid), pivoting the zoom on `zoomTile`. The look is
// just the wallpaper with each window floating over it as an individually
// rounded, slightly-shrunk rect — no per-tile backgrounds, borders, or dimming.
static void drawOverview(PHLMONITOR m, float p, float pl, int zoomTile) {
    if (!m)
        return;

    Rect tiles[N_TILES];
    if (computeTiles(m, tiles) < N_TILES)
        return;

    // No captures for this monitor: fall back to the flat schematic.
    if (g_captureMon.lock() != m || !g_bgFB) {
        drawSchematic(m, tiles);
        return;
    }

    // Zoom transform: scale about zoomTile's top-left so at p=0 that tile becomes
    // the full monitor; mix an arbitrary rest-rect toward its zoomed rect by p.
    // Separate x/y scales: inset tiles (see computeTiles) are not quite monitor
    // aspect, and the close must land exactly full-screen (the ~2% stretch while
    // animating is imperceptible; a 2% pop at the hand-off is not).
    const double mw = m->m_transformedSize.x;
    const double mh = m->m_transformedSize.y;
    const Rect&  az = tiles[zoomTile];
    const double sx = mw / az.w, sy = mh / az.h;
    auto         dispRect = [&](const CBox& r) -> CBox {
        const double zx = (r.x - az.x) * sx, zy = (r.y - az.y) * sy;
        const double zw = r.w * sx, zh = r.h * sy;
        return CBox{mix(zx, r.x, p), mix(zy, r.y, p), mix(zw, r.w, p), mix(zh, r.h, p)};
    };

    // The single wallpaper backdrop, filling the whole monitor.
    if (const auto bg = g_bgFB->getTexture()) {
        CTexPassElement::SRenderData td;
        td.tex = bg;
        td.box = CBox{0.0, 0.0, m->m_transformedSize.x, m->m_transformedSize.y};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
    }

    // Windows never draw into the bar strip: during a page flip the outgoing
    // page's tiles slide up and used to peek through the bar's transparent
    // areas (Max saw page 1's bottoms on page 2). The clip relaxes with the
    // zoom (p→0 = a workspace filling the whole monitor, bar strip included).
    const double clipTop = topInset(m) * p;
    const CBox   stripClip{0.0, clipTop, m->m_transformedSize.x, m->m_transformedSize.y - clipTop};
    auto drawTex = [&](SP<Render::ITexture> tex, const CBox& b, int round) {
        CTexPassElement::SRenderData td;
        td.tex           = tex;
        td.box           = b;
        td.round         = round;
        td.roundingPower = 2.0f;
        td.clipBox       = stripClip;
        // COVER, never stretch: when the box's aspect drifts from the
        // capture's (a sibling gliding to its split half, the ghost morphing
        // toward its destination), crop the source centrally in UV space —
        // the window's pixels keep their aspect and the box edges cut into
        // them, reading like a real resize instead of rubber-banding.
        const double ta = tex->m_size.y > 0.0 ? tex->m_size.x / tex->m_size.y : 1.0;
        const double ba = b.h > 0.0 ? b.w / b.h : ta;
        if (std::abs(ba - ta) > 0.01 * ta) {
            Vector2D tl{0.0, 0.0}, br{1.0, 1.0};
            if (ba > ta) { // box relatively wider: crop top/bottom
                const double f = ta / ba;
                tl.y           = (1.0 - f) / 2.0;
                br.y           = 1.0 - tl.y;
            } else { // box relatively taller: crop left/right
                const double f = ba / ta;
                tl.x           = (1.0 - f) / 2.0;
                br.x           = 1.0 - tl.x;
            }
            g_pHyprRenderer->m_renderPass.add(makeUnique<CUVTexElement>(std::move(td), tl, br));
            return;
        }
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
    };
    // A rounded border around the window mini. CONSTANT width matching the
    // desktop's borders — never proportional (small windows used to get thin
    // halos).
    //
    // Drawn with the compositor's OWN border pass, from the live
    // `general:col.active_border` gradient, so an overview ring is the same
    // paint as the ring around the real window — gradient, stops and angle
    // included. `renderBorder` grows the box outward by borderSize (and
    // scales that by the monitor scale itself, so it takes the LOGICAL
    // width), leaving the interior for the thumbnail: same footprint the old
    // filled-rect halo had.
    //
    // Without a readable gradient, fall back to that old halo in flat amber:
    // a filled rounded rect *behind* the window, the texture covering its
    // interior and leaving a ring.
    auto haloBorder = [&](const CBox& box, int round) { haloAround(m, box, round); };

    const auto hoverW = g_hoverWin.lock();
    // Only treat it as a drag once the cursor has left the click slop — before that
    // a press is still a potential click, so the window stays put in its tile.
    // For the WHOLE gesture (held AND committed): the window is hidden from
    // the tiles — a committed slot shows as a live HOLE among the really
    // squeezed siblings — and the ghost stays on the cursor, so the hand
    // never appears to lose it.
    const auto dragW  = g_dragMoved ? g_dragWin.lock() : PHLWINDOW{};

    // NO rest-shrink (per Max, round 3): windows draw at their true mapped
    // size — a maximized/smart-gaps window touches its tile edges exactly
    // like it touches the screen, and every visible gap comes from the real
    // desktop gaps miniaturized. NO tile/space frames either (also per
    // Max): borders live on WINDOWS only — empty views stay bare wallpaper
    // (they're still clickable jump targets; the cursor is the affordance).

    // Window boxes in three passes so identical desktop twins stay
    // identical in the overview (per-window heuristics broke that —
    // Max's ws5 twins rendered unequal):
    //   A) map each window into its tile (usable-area space) and SNAP
    //      edges near the tile bound flush to it (no outer gaps, ever);
    //   B) PAIRWISE SEAM CENTERING: every adjacent pair shares its seam
    //      at the midpoint between them, each side giving exactly half
    //      the design gap — symmetric by construction;
    //   C) lerp real→target by `p` (pixel-exact close) and draw.
    struct MiniBox {
        double x0, y0, x1, y1;
    };
    std::vector<MiniBox> real(g_wins.size()), tgt(g_wins.size());
    std::vector<bool>    ok(g_wins.size(), false);
    const SUsable        u = usableArea(m);
    for (size_t i = 0; i < g_wins.size(); ++i) {
        auto& cw = g_wins[i];
        Rect  mini;
        waveview_map_window(tiles[cw.tile].x, tiles[cw.tile].y, tiles[cw.tile].w, tiles[cw.tile].h, u.x, u.y, u.w, u.h,
                            cw.logical.x, cw.logical.y, cw.logical.w, cw.logical.h, &mini);
        if (mini.w <= 0.0 || mini.h <= 0.0) {
            cw.screen = CBox{};
            continue;
        }
        // The LANDING seat: the same window mapped against the FULL MONITOR.
        // dispRect at p=0 inverts exactly that mapping, so the close's last
        // frame IS the desktop, pixel for pixel. Mapping only against the
        // usable area (the grid's space, which excludes the bar strip)
        // landed every mini ~2% high and small — the windows then JUMPED to
        // their resting point at the overlay drop (Max, 2026-09-16). The
        // grid keeps the usable mapping (tgt); the landing keeps this (real);
        // the layout blend travels between the two spaces continuously.
        Rect land;
        waveview_map_window(tiles[cw.tile].x, tiles[cw.tile].y, tiles[cw.tile].w, tiles[cw.tile].h, m->m_position.x,
                            m->m_position.y, m->m_size.x, m->m_size.y, cw.logical.x, cw.logical.y, cw.logical.w,
                            cw.logical.h, &land);
        ok[i]           = true;
        real[i]         = {land.x, land.y, land.x + land.w, land.y + land.h};
        tgt[i]          = {mini.x, mini.y, mini.x + mini.w, mini.y + mini.h};
        if (cw.spread) {
            // The TARGET is the collage seat; `real` stays the true box, so
            // the layout blend genuinely travels true↔slot: the tile opens
            // as a literal mini-desktop, composes into the collage as pl→1,
            // and the close's landing act walks it home — no jump at the
            // overlay drop. No bound-snapping, no seam-solving here: a
            // collage keeps its own margins and gaps.
            Rect smini;
            waveview_map_window(tiles[cw.tile].x, tiles[cw.tile].y, tiles[cw.tile].w, tiles[cw.tile].h, u.x, u.y, u.w,
                                u.h, cw.slot.x, cw.slot.y, cw.slot.w, cw.slot.h, &smini);
            if (smini.w > 0.0 && smini.h > 0.0)
                tgt[i] = {smini.x, smini.y, smini.x + smini.w, smini.y + smini.h};
        } else {
            const Rect&  t    = tiles[cw.tile];
            const double thrX = t.w * 0.02, thrY = t.h * 0.02;
            tgt[i].x0 = (tgt[i].x0 - t.x < thrX) ? t.x : tgt[i].x0;
            tgt[i].y0 = (tgt[i].y0 - t.y < thrY) ? t.y : tgt[i].y0;
            tgt[i].x1 = (t.x + t.w - tgt[i].x1 < thrX) ? t.x + t.w : tgt[i].x1;
            tgt[i].y1 = (t.y + t.h - tgt[i].y1 < thrY) ? t.y + t.h : tgt[i].y1;
        }
    }
    // Pass B: SEAM LINES. Pairwise mutation was order-dependent (an edge
    // facing two neighbours got re-centred twice, so identical twins
    // diverged — measured 12.7px on Max's ws5). Instead: per tile, per
    // axis, cluster all non-bound edges that fall near a common line; the
    // line sits at the cluster mean and EVERY closing edge becomes
    // line - g/2, every opening edge line + g/2. Deterministic, and
    // columns/rows align by construction.
    {
        struct EdgeRef {
            double coord;
            size_t win;
            bool   closing; // true = x1/y1 (left/top side of the seam)
        };
        auto solveAxis = [&](int tile, bool xAxis) {
            const Rect&          t   = tiles[tile];
            const double         eps = std::max(t.w, t.h) * 0.03;
            const double         g   = DSN_WIN_GAP * (xAxis ? t.w : t.h);
            const double         lo  = xAxis ? t.x : t.y;
            const double         hi  = xAxis ? t.x + t.w : t.y + t.h;
            std::vector<EdgeRef> edges;
            for (size_t i = 0; i < g_wins.size(); ++i) {
                if (!ok[i] || g_wins[i].tile != tile || g_wins[i].spread)
                    continue;
                const double e0 = xAxis ? tgt[i].x0 : tgt[i].y0;
                const double e1 = xAxis ? tgt[i].x1 : tgt[i].y1;
                if (e0 > lo + 0.5) // bound-snapped edges are final
                    edges.push_back({e0, i, false});
                if (e1 < hi - 0.5)
                    edges.push_back({e1, i, true});
            }
            std::sort(edges.begin(), edges.end(), [](const EdgeRef& a, const EdgeRef& b) { return a.coord < b.coord; });
            // Seam lines: cluster means where BOTH sides have windows.
            std::vector<double> lines;
            for (size_t s = 0; s < edges.size();) {
                size_t e   = s + 1;
                double sum = edges[s].coord;
                bool   opn = !edges[s].closing, cls = edges[s].closing;
                while (e < edges.size() && edges[e].coord - edges[s].coord < eps) {
                    sum += edges[e].coord;
                    opn |= !edges[e].closing;
                    cls |= edges[e].closing;
                    ++e;
                }
                if (opn && cls)
                    lines.push_back(sum / (double)(e - s));
                s = e;
            }
            if (lines.empty())
                return;
            // Piecewise redistribution: content between lines compresses by
            // one uniform factor and EXACTLY g is inserted at every line —
            // uniform seams, aligned columns, flush bounds; sizes stay
            // proportional to their between-lines share.
            const size_t        k     = lines.size();
            const double        span  = hi - lo;
            const double        scale = std::max(0.0, span - (double)k * g) / span;
            std::vector<double> is(k + 2), ns(k + 2); // interval starts: old, new
            is[0] = lo;
            ns[0] = lo;
            for (size_t j = 1; j <= k; ++j) {
                is[j] = lines[j - 1];
                ns[j] = ns[j - 1] + (is[j] - is[j - 1]) * scale + g;
            }
            is[k + 1] = hi;
            ns[k + 1] = hi + g; // sentinel; unused beyond interval math
            auto remap = [&](double v, bool closing) -> double {
                if (v <= lo + 0.5)
                    return lo;
                if (v >= hi - 0.5)
                    return hi;
                for (size_t j = 0; j < k; ++j)
                    if (std::abs(v - lines[j]) < eps / 2.0)
                        return closing ? ns[j + 1] - g : ns[j + 1]; // line: -g/2 side handled by ns offset
                // inside an interval: linear map within it
                size_t j = 0;
                while (j < k && v > lines[j])
                    ++j;
                const double a0 = is[j], n0 = ns[j];
                return n0 + (v - a0) * scale;
            };
            for (size_t i = 0; i < g_wins.size(); ++i) {
                if (!ok[i] || g_wins[i].tile != tile)
                    continue;
                if (xAxis) {
                    tgt[i].x0 = remap(tgt[i].x0, false);
                    tgt[i].x1 = remap(tgt[i].x1, true);
                } else {
                    tgt[i].y0 = remap(tgt[i].y0, false);
                    tgt[i].y1 = remap(tgt[i].y1, true);
                }
            }
        };
        bool tileSeen[N_TILES] = {};
        for (size_t i = 0; i < g_wins.size(); ++i) {
            if (!ok[i] || tileSeen[g_wins[i].tile])
                continue;
            tileSeen[g_wins[i].tile] = true;
            solveAxis(g_wins[i].tile, true);
            solveAxis(g_wins[i].tile, false);
        }
    }
    // Resolve every window's drawn box first (hit-testing uses the REAL
    // slots), then apply the live swap preview before drawing. `pl` is the
    // LAYOUT blend, on its own clock (see onRender): opening it lags the
    // zoom; closing it stays 1 through the whole zoom and only then glides
    // home at flat scale — the close's second act.
    std::vector<CBox> boxes(g_wins.size());
    ssize_t           dragIdx = -1, swapIdx = -1;
    for (size_t i = 0; i < g_wins.size(); ++i) {
        if (!ok[i])
            continue;
        const double x0 = mix(real[i].x0, tgt[i].x0, pl), y0 = mix(real[i].y0, tgt[i].y0, pl);
        const double x1 = mix(real[i].x1, tgt[i].x1, pl), y1 = mix(real[i].y1, tgt[i].y1, pl);
        boxes[i]        = dispRect(CBox{x0, y0, std::max(1.0, x1 - x0), std::max(1.0, y1 - y0)});
        g_wins[i].screen = boxes[i]; // hit-testing tracks the real slot
        if (dragW && g_wins[i].win.lock() == dragW)
            dragIdx = (ssize_t)i;
    }
    // Live SPLIT preview (matching the real drop semantics): while the drag
    // hovers any window, that window GLIDES to the half it will keep —
    // vacating the half the dropped window will take (dwindle insert).
    // Glides home when the drag moves off.
    if (dragIdx >= 0) {
        for (size_t i = g_wins.size(); i-- > 0;) {
            if (!ok[i] || (ssize_t)i == dragIdx)
                continue;
            if (boxes[i].w > 0.0 && boxes[i].containsPoint(g_dragCursor)) {
                swapIdx = (ssize_t)i;
                break;
            }
        }
    }
    bool previewMoving = false;
    // The ghost keeps the window's own shape — destination previews are
    // REAL commits now, so it never morphs (and never hits the aspect-
    // mismatch draw path that rendered so badly).
    if (dragIdx >= 0) {
        g_ghostWantW = boxes[dragIdx].w;
        g_ghostWantH = boxes[dragIdx].h;
        const double k = std::min(1.0, (double)g_frameDt * PREVIEW_RATE);
        g_ghostW += (g_ghostWantW - g_ghostW) * k;
        g_ghostH += (g_ghostWantH - g_ghostH) * k;
        if (std::abs(g_ghostW - g_ghostWantW) > 0.5 || std::abs(g_ghostH - g_ghostWantH) > 0.5)
            previewMoving = true;
    }
    // The DRAW-SIDE GLIDE: mapped slots come from spring goals and move
    // exactly once per commit; each drawn box chases its slot at full frame
    // rate. The capture cadence now only refreshes CONTENT — motion no
    // longer samples at 20fps next to a 165Hz ghost. Zoom and page flips
    // own the motion themselves: while they run, the chase snaps.
    {
        const bool settled = p >= 0.999f && g_scrollProg >= 1.0f;
        for (size_t i = 0; i < g_wins.size(); ++i) {
            auto& cw = g_wins[i];
            if (!ok[i])
                continue;
            if (!settled || cw.drawCur.w <= 0.0) {
                cw.drawCur = boxes[i];
                continue;
            }
            const double kc = std::min(1.0, (double)g_frameDt * PREVIEW_RATE);
            cw.drawCur       = CBox{cw.drawCur.x + (boxes[i].x - cw.drawCur.x) * kc,
                                    cw.drawCur.y + (boxes[i].y - cw.drawCur.y) * kc,
                                    cw.drawCur.w + (boxes[i].w - cw.drawCur.w) * kc,
                                    cw.drawCur.h + (boxes[i].h - cw.drawCur.h) * kc};
            if (std::abs(cw.drawCur.x - boxes[i].x) + std::abs(cw.drawCur.y - boxes[i].y) +
                    std::abs(cw.drawCur.w - boxes[i].w) + std::abs(cw.drawCur.h - boxes[i].h) >
                1.0)
                previewMoving = true;
            boxes[i] = cw.drawCur;
        }
    }
    if (previewMoving) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
    const int round = (int)std::lround(DSN_WIN_ROUND * m->m_scale * p); // same corners everywhere, incl. the ghost
    for (size_t i = 0; i < g_wins.size(); ++i) {
        auto& cw = g_wins[i];
        if (!ok[i])
            continue;
        const auto tex = cw.fb ? cw.fb->getTexture() : nullptr;
        if (!tex)
            continue;
        const CBox& box = boxes[i];
        const auto  w   = cw.win.lock();

        if (w && w == dragW)
            continue; // the dragged window is drawn last, under the cursor

        if ((w && w == hoverW) || (ssize_t)i == swapIdx)
            haloBorder(box, round); // ring: hover, or the pending drop target

        // THE CHROME FADES IN (close, act 2): the card solo — bar and
        // border baked — drawn under the content at rising alpha while the
        // window glides home, reaching full strength exactly at the overlay
        // drop. Content stays bar-less through grid and zoom (level 2's
        // rule); the desktop's chrome no longer pops into existence (Max,
        // 2026-09-16: "i think is about the bars coming back").
        if (cw.spread && w && g_animTarget < 0.5f && pl < 1.f)
            if (auto* sc = soloFor(w, false); sc && sc->fb)
                if (const auto ct = sc->fb->getTexture()) {
                    double top = 0.0, side = 0.0;
                    cardExtents(w, top, side);
                    const CBox   swb = w->getWindowMainSurfaceBox();
                    const double sc2 = swb.w > 0.0 ? box.w / swb.w : 0.0; // the mini's current scale
                    CTexPassElement::SRenderData cd;
                    cd.tex = ct;
                    cd.box = CBox{box.x - side * sc2, box.y - top * sc2, box.w + 2.0 * side * sc2,
                                  box.h + (top + side) * sc2};
                    cd.a   = 1.0f - pl;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(cd));
                }

        drawTex(tex, box, round);
    }

    // The dragged window rides on top, following the cursor.
    if (dragW) {
        for (auto& cw : g_wins) {
            if (cw.win.lock() != dragW)
                continue;
            const auto tex = cw.fb ? cw.fb->getTexture() : nullptr;
            if (!tex)
                break;
            // Shapeshifting ghost: eased dims previewing the destination
            // slot, anchored by the grab point as a fraction of the box.
            // The WINDOW ITSELF morphs (per Max — the frosted-plate stand-in
            // read as an abstract colored shape). drawTex's cover-crop keeps
            // its pixels undistorted while the box changes shape — same
            // treatment as the sibling halves gliding beneath it.
            CBox b{g_dragCursor.x - g_grabFracX * g_ghostW, g_dragCursor.y - g_grabFracY * g_ghostH, g_ghostW,
                   g_ghostH};
            haloBorder(b, round);
            drawTex(tex, b, round);
            break;
        }
    }

    // ── THE STAGE'S DIM, PAINTED OVER THE MAP ────────────────────────────────
    //
    // Everything OUTSIDE the tile we are flying into goes dark the instant the
    // stage is back, so the dim lands as one step together with the picture —
    // Max, 2026-09-17: *"the dimming should happen at the same time that the
    // windows start coming, it comes too late."*
    //
    // ⭐ Why the plugin has to paint it at all: `dim_around` is drawn around the
    // staged window ON ITS OWN WORKSPACE, so a capture can only ever carry it in
    // ONE tile. Re-photographing every tile does not help — the other tiles and
    // the wallpaper backdrop have no window to dim around, so they stay bright
    // and the darkness still has to GROW with the zoom tile. That growth is the
    // "200ms to dim" and the "BG becomes dimmed after i land". Only something
    // covering the whole overlay can make it a step.
    //
    // A hole is left for the zoom tile because its own picture already carries
    // the real dim (verified: a staged capture is 27% darker in RGB than the same
    // capture with dim off). At p=0 that hole IS the whole monitor, so this draws
    // nothing at the hand-off and the last frame stays pixel-identical to the
    // desktop — the property the linger depends on.
    if (g_stageDimPaint) {
        // ⭐ The hole is the staged WINDOW's rect, never the tile's. The desktop
        // dims everything AROUND the window, so at p=0 this has to be exactly
        // that shape or the hand-off steps. Holing the whole tile instead meant
        // the painted dim vanished as the tile filled the screen, leaving the
        // last frames bright before the dimmed desktop appeared — the
        // *"dimm well, then it goes bright, and dimm again"* (Max, 2026-09-17).
        CBox hole{0.0, 0.0, mw, mh};
        if (const auto w = Desktop::focusState()->window(); w && w->m_isMapped) {
            const auto   wb = w->getWindowMainSurfaceBox();
            const double sc = m->m_scale;
            const double wx = (wb.x - m->m_position.x) * sc, wy = (wb.y - m->m_position.y) * sc;
            // Into the tile's rest space, then through the very same zoom
            // transform every tile uses — so the hole tracks the window as it
            // flies, and lands on its real box.
            hole = dispRect(CBox{az.x + wx / mw * az.w, az.y + wy / mh * az.h, wb.w * sc / mw * az.w,
                                 wb.h * sc / mh * az.h});
        }
        const double x0 = hole.x, y0 = hole.y, x1 = hole.x + hole.w, y1 = hole.y + hole.h;
        const CHyprColor dim{0.0, 0.0, 0.0, GOLEM_STAGE_DIM};
        auto             band = [&](double bx, double by, double bw, double bh) {
            if (bw > 0.5 && bh > 0.5)
                renderRect(CBox{bx, by, bw, bh}, dim);
        };
        band(0.0, 0.0, mw, std::max(0.0, y0));                      // above
        band(0.0, std::min(mh, y1), mw, std::max(0.0, mh - y1));    // below
        band(0.0, std::max(0.0, y0), std::max(0.0, x0), std::max(0.0, std::min(mh, y1) - std::max(0.0, y0)));
        band(std::min(mw, x1), std::max(0.0, y0), std::max(0.0, mw - x1),
             std::max(0.0, std::min(mh, y1) - std::max(0.0, y0)));  // right
    }
}

// While the overview is open, re-capture thumbnails on a timer so they stay live.
// Fires outside the render pass (so beginRender is safe) and re-arms itself.
static void onLiveTimer(SP<CEventLoopTimer> self, void*) {
    checkFloatWatch(); // float-leak watch outlives the overview (leaks show on the desktop)
    // The deferred VRAM free: the desktop has been settled for ~450ms, the
    // driver can absorb the release without a texture dropping out of a
    // frame (the blink). A reopen in the meantime cleared the flag.
    if (g_freePending && !g_active && !g_spreadActive) {
        g_freePending = false;
        freeCaptures();
        freeSpread();
    }
    if (g_spreadActive && !g_active) {
        // Never recapture mid-glide — a capture stalls a frame (the
        // overview's own hitch lesson). Poll quickly until it settles.
        if (g_spreadAnim != g_spreadTarget) {
            self->updateTimeout(std::chrono::milliseconds(50));
            return;
        }
        if (const auto m = g_spreadMon.lock()) {
            refreshSpread(m);
            damageAll();
        } else
            snapCloseSpread();
        if (!g_spreadActive)
            return; // the refresh folded it; finishSpreadClose disarmed the timer
        self->updateTimeout(REFRESH_MS);
        return;
    }
    if (!g_active) {
        if (g_watchWin.lock())
            self->updateTimeout(std::chrono::milliseconds(100)); // keep watching ≤3s past close
        return; // disarmed on close; don't re-arm
    }
    // Never recapture mid-animation: snapshotting 18 workspaces stalls a
    // frame, which reads as a hitch in the page-flip / zoom / landing glide.
    // Poll quickly until the motion settles, then catch up. A CLOSING
    // overview never captures at all — the tiles are about to be freed, and
    // a capture firing in the tick between touchdown and disengage blocked
    // the loop at the exact frame of the hand-off (the blink race).
    if (g_scrollProg < 1.0f || g_anim != g_animTarget || g_animTarget < 0.5f) {
        self->updateTimeout(std::chrono::milliseconds(50));
        return;
    }
    if (const auto m = g_captureMon.lock()) {
        // Boost ticks refresh only the tiles a commit touched; the regular
        // cadence does a full pass and resets the dirty set.
        const bool boosting = Time::steadyNow() < g_boostUntil;
        captureWorkspaces(m, boosting && g_dirtyTiles ? g_dirtyTiles : ALL_TILES);
        if (!boosting)
            g_dirtyTiles = 0;
        maybeCommit(m); // the dwell can expire with the cursor at rest
        damageAll();
    }
    // While a live commit's real re-tile is springing, capture at ~20fps so
    // the tiles show the actual animation — that's the point of committing.
    self->updateTimeout(Time::steadyNow() < g_boostUntil ? std::chrono::milliseconds(50) : REFRESH_MS);
}

// Global cursor position expressed in draw space (whole monitor = [0,0,transformedSize]).
static Vector2D cursorDrawSpace(PHLMONITOR m) {
    const Vector2D g     = g_pInputManager->getMouseCoordsInternal();
    const Vector2D local = g - m->m_position;
    return Vector2D(local.x * m->m_transformedSize.x / m->m_size.x, local.y * m->m_transformedSize.y / m->m_size.y);
}

// The inverse: a draw-space point back to a global cursor position, so we can
// put the pointer somewhere the overview drew (see the open warp).
static Vector2D drawSpaceToGlobal(PHLMONITOR m, const Vector2D& d) {
    return m->m_position + Vector2D(d.x * m->m_size.x / m->m_transformedSize.x, d.y * m->m_size.y / m->m_transformedSize.y);
}

// Topmost captured window whose last-drawn box contains `c`, or empty.
static PHLWINDOWREF winAt(const Vector2D& c) {
    for (auto it = g_wins.rbegin(); it != g_wins.rend(); ++it)
        if (it->screen.w > 0.0 && it->screen.containsPoint(c))
            return it->win;
    return {};
}

// Grid slot (0..8) whose rest rect contains draw-space point `c`, or -1. Uses the
// rest layout (matches interaction at p≈1, same hit test as the drag drop logic).
static int tileAt(PHLMONITOR m, const Vector2D& c) {
    Rect tiles[N_TILES];
    if (computeTiles(m, tiles) != N_TILES)
        return -1;
    for (int i = 0; i < N_TILES; ++i)
        if (CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}.containsPoint(c))
            return i;
    return -1;
}

// True if no captured window sits in `tile` — i.e. that workspace is empty.
static bool tileEmpty(int tile) {
    for (auto& cw : g_wins)
        if (cw.tile == tile)
            return false;
    return true;
}

// Whether the window's layout target sits in a space. The fork's
// LayoutManager::resizeTarget AND setTargetGeom dereference target->space()
// unchecked, and a spaceless target (group-membership transitions leave
// them behind) is a null CSpace — the 2026-08-30 18:46 SIGSEGV. The core's
// own DragController refuses these ("no workspace"); every direct layout
// call of ours must match it.
static bool inLayoutSpace(const PHLWINDOW& w) {
    const auto t = w ? w->layoutTarget() : nullptr;
    return t && t->space();
}

// The resize gesture is stateless beyond its grab: sizes applied live are
// final, so ending is just bookkeeping + a settle capture.
static void endRealResize() {
    if (!g_resizing)
        return;
    g_resizing   = false;
    sendOverviewSize(nullptr, true); // clear the topbar's live readout
    const auto w = g_resizeWin.lock();
    g_resizeWin.reset();
    if (!w)
        return;
    trace("resize end ws=%d", (int)w->workspaceID());
    markDirty(w->workspaceID());
    boostCaptures();
    if (const auto m = g_captureMon.lock())
        captureWorkspaces(m, g_dirtyTiles);
    damageAll();
}

// Cursor motion while open: update the hovered window, or the drag/resize —
// then SWALLOW the event. Cancelling is safe in this fork: PointerManager::move
// runs before the hook fires (verified in InputManager.cpp::onMouseMoved), so
// the sprite keeps moving; what cancelling stops is focus-follows-mouse and
// surface motion reaching the desktop underneath — which used to leak (windows
// refocused, the dock revealed, topbar pills lit while the overview was open).
// The first uncancelled motion after close re-focuses under the cursor.
static void onMouseMove(Vector2D, Event::SCallbackInfo& info) {
    checkResizeDrag(); // resize-drag watch runs desktop-side too (cheap)
    if (g_spreadActive && !g_active) {
        const auto m = g_spreadMon.lock();
        if (!m)
            return;
        // The OPTIONS strip stays the bar's, exactly like the overview.
        if (inTopbarStrip(m)) {
            if (g_spreadHover.lock()) {
                g_spreadHover.reset();
                damageAll();
            }
            return;
        }
        info.cancelled   = true; // motion must not refocus the desktop underneath
        const Vector2D c = cursorDrawSpace(m);
        PHLWINDOWREF   hov;
        for (auto it = g_spreadWins.rbegin(); it != g_spreadWins.rend(); ++it)
            if (it->screen.w > 0.0 && it->screen.containsPoint(c)) {
                hov = it->win;
                break;
            }
        if (hov.lock() != g_spreadHover.lock()) {
            g_spreadHover = hov;
            damageAll();
        }
        return;
    }
    if (!g_active || g_animTarget < 0.5f)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
    // The compositor's motion handling already ran (it precedes this hook) and
    // may have re-applied its border icon from the invisible desktop windows
    // underneath — undo that before any shape decision below.
    reassertOverviewCursor();
    // Hand the OPTIONS strip back to the bar (mid-gesture events stay ours,
    // so a drag or resize that wanders under the bar isn't interrupted).
    if (!g_busy && !g_resizing && !g_dragWin.lock() && inTopbarStrip(m)) {
        if (g_hoverWin.lock()) {
            g_hoverWin.reset();
            sendOverviewHover(nullptr); // left the grid: pill drops the title
            damageAll();
        }
        return; // NOT cancelled — the bar's pills get hover + clicks
    }
    info.cancelled   = true;
    if (g_busy)
        return; // the machinery's own cursor warps must not feed back into it
    // The hand is already driving: drop the open warp rather than yank the
    // pointer out from under a gesture the user has started.
    if (g_warpPending) {
        const double moved = (g_pInputManager->getMouseCoordsInternal() - g_warpFromCursor).size();
        if (moved > WARP_CANCEL_SLOP) {
            if (g_swipeLive || Time::steadyNow() - g_swipeEndAt < SWIPE_SETTLE) {
                // Finger-lift jitter from the swipe that opened us: not aiming.
                trace("open warp: rebaseline (moved=%.0f, swipe)", moved);
                g_warpFromCursor = g_pInputManager->getMouseCoordsInternal();
            } else {
                trace("open warp: CANCELLED (moved=%.0f, flick)", moved);
                g_warpPending = false;
                g_warpWin.reset();
                endWarpHide(); // a moving pointer must be visible, immediately
            }
        }
    }
    if (g_resizing) {
        const auto rw = g_resizeWin.lock();
        if (!rw || rw->isFullscreen() || !inLayoutSpace(rw)) {
            if (rw && !inLayoutSpace(rw))
                trace("resize DROP mid-gesture: ws=%d target lost its space", (int)rw->workspaceID());
            endRealResize(); // window died / went fullscreen / left the layout mid-gesture
            return;
        }
        const Vector2D c = cursorDrawSpace(m);
        const Vector2D d{(c.x - g_resizeLast.x) * g_resizeScale.x * g_resizeMask.x, (c.y - g_resizeLast.y) * g_resizeScale.y * g_resizeMask.y};
        g_resizeLast     = c;
        if (d.x != 0.0 || d.y != 0.0) {
            g_layoutManager->resizeTarget(d, rw->layoutTarget(), g_resizeCorner);
            markDirty(rw->workspaceID());
            g_boostUntil = Time::steadyNow() + BOOST_MS; // captures track the live resize
            sendOverviewSize(rw, false); // live numbers on the topbar pill
            damageAll();
        }
        return;
    }
    updateHoverAt(m, cursorDrawSpace(m));
}

// Focus hops with every commit (dwindle needs the split target focused), and
// the dim / border-fade / alpha transitions it triggers keep animating across
// several captures — reading as glows ("lightnings") on OTHER tiles while a
// window is being moved somewhere else. All of these are compositor-side
// cosmetics with no client-redraw dependency (unlike geometry — the
// freeze-frame lesson), so snapping them everywhere is safe.
static void warpFocusFx() {
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped)
            continue;
        w->m_alpha.warp();
        w->m_dimPercent->warp();
        w->m_borderFadeAnimationProgress->warp();
        w->m_realShadowColor->warp();
        w->m_realGlowColor->warp();
    }
}

// Begin the compositor's own drag for `dw` at GRAB time: the layout floats
// the window out and re-tiles the siblings immediately (the live thumbnails
// show it — no hole). The cursor warps to the window's desktop centre to
// seed the drag, then returns; our motion-swallow mutes the side effects.
static void beginRealDrag(PHLWINDOW dw, bool capture) {
    if (g_dragReal || !dw || dw->isFullscreen())
        return;
    const SBusyScope busy;
    const CBox     wb = dw->getWindowMainSurfaceBox();
    const Vector2D home{wb.x + wb.w / 2.0, wb.y + wb.h / 2.0};
    const Vector2D saved = g_pInputManager->getMouseCoordsInternal();
    g_dragHomeCenter     = home;
    g_pCompositor->warpCursorTo(home, true);
    g_layoutManager->beginDragTarget(dw->layoutTarget(), MBIND_MOVE);
    // The controller can reject the grab (no target kept). Claiming
    // g_dragReal anyway means the release later ends a drag the
    // compositor never held — dragEnd() derefs null and crashes. The
    // begin may still have floated the window before rejecting: undo it.
    if (!g_layoutManager->dragController()->target()) {
        trace("grab REJECT ws=%d float=%d", (int)dw->workspaceID(), (int)dw->m_isFloating);
        restoreFloatState(dw);
        g_pCompositor->warpCursorTo(saved, true);
        return;
    }
    trace("grab ws=%d float=%d dragTiled=%d", (int)dw->workspaceID(), (int)dw->m_isFloating,
          (int)g_layoutManager->dragController()->draggingTiled());
    g_layoutManager->moveMouse(home + Vector2D(3, 3)); // trip the drag threshold
    g_layoutManager->moveMouse(home);
    // Park the float far offscreen for the drag's duration: it floats at
    // its old spot on the REAL workspace, so the live captures were baking
    // its pixels into the re-tiled siblings' textures ("the grabbed
    // window's image gets printed on the reacting one"). The end-drag
    // recomputes position from the begin anchor, so parking is invisible
    // to the drop math; our cursor ghost is the only visual.
    if (inLayoutSpace(dw)) // setTargetGeom derefs target->space() unchecked
        g_layoutManager->setTargetGeom(CBox{-20000.0, -20000.0, wb.w, wb.h}, dw->layoutTarget());
    g_pCompositor->warpCursorTo(saved, true);
    g_dragReal = true;
    warpFocusFx(); // grab/regrab focus churn must not glow through captures
    markDirty(dw->workspaceID()); // the pull-out re-tiled this view
    // A re-commit passes capture=false: it re-inserts immediately after and
    // captures THEN — snapshotting the pulled-out intermediate made every
    // re-placement read as a double bounce. (NO geometry warping here: a
    // warped capture freezes client buffers that haven't redrawn at their
    // new sizes — that was v0.9's "content all mixed between windows".)
    if (capture)
        if (const auto m = g_captureMon.lock())
            captureWorkspaces(m, g_dirtyTiles); // show the re-tile right away
}

// The grab floats the window out of the layout; endDragTarget() re-tiles
// it. Every guarded path that SKIPS the end (dead drag target — the crash
// guards) leaks that float ("i grabbed a window and it became floating").
// Called after every end attempt: restores whatever the window was when
// the gesture started, and no-ops when the end completed normally.
static void restoreFloatState(PHLWINDOW dw) {
    if (dw && dw->m_isMapped && !dw->isFullscreen() && dw->m_isFloating != g_origFloating) {
        trace("floatnet TOGGLE ws=%d float %d->%d", (int)dw->workspaceID(), (int)dw->m_isFloating,
              (int)g_origFloating);
        g_layoutManager->changeFloatingMode(dw->layoutTarget());
    }
}

// Arm the deferred float watch for the window a gesture just released.
static void armFloatWatch(PHLWINDOW dw) {
    if (!dw)
        return;
    g_watchWin         = dw;
    g_watchFloatExpect = g_origFloating;
    g_watchSince       = Time::steadyNow();
}

// One-shot leak report: fires if the watched window's float state diverges
// from the gesture's promise any time within 3s of the release.
static void checkFloatWatch() {
    const auto w = g_watchWin.lock();
    if (!w)
        return;
    const double ms = std::chrono::duration<double, std::milli>(Time::steadyNow() - g_watchSince).count();
    if (w->m_isMapped && !w->isFullscreen() && w->m_isFloating != g_watchFloatExpect) {
        trace("FLOAT-LEAK +%.0fms ws=%d float=%d expected=%d", ms, (int)w->workspaceID(), (int)w->m_isFloating,
              (int)g_watchFloatExpect);
        g_watchWin.reset(); // log once per gesture
        return;
    }
    if (ms > 3000.0)
        g_watchWin.reset(); // clean for 3s = no leak this gesture
}

// A clean synthetic re-place: begin+end a whole drag around a KNOWN static
// window state, splitting `under` at `desk` (dwindle insert). Used after a
// cross-workspace move — running the positional insert while the original
// grab-drag was still alive made moves and inserts fight over the space.
static void placeAt(PHLWINDOW dw, const Vector2D& desk, PHLWINDOW under) {
    if (!dw || dw->isFullscreen())
        return;
    const SBusyScope busy;
    const Vector2D saved = g_pInputManager->getMouseCoordsInternal();
    g_pCompositor->warpCursorTo(desk, true);
    if (under)
        Desktop::focusState()->fullWindowFocus(under, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    g_layoutManager->beginDragTarget(dw->layoutTarget(), MBIND_MOVE);
    // The begin can silently reject (the controller keeps no target); ending
    // a targetless drag segfaults inside dragEnd() — compositor down.
    if (g_layoutManager->dragController()->target()) {
        g_layoutManager->moveMouse(desk + Vector2D(3, 3));
        g_layoutManager->moveMouse(desk);
        // The drag motion itself can drop the target (the controller aborts
        // mid-drag); the earlier check is stale by now. Re-check or crash.
        if (g_layoutManager->dragController()->target())
            g_layoutManager->endDragTarget();
        else
            trace("placeAt DEAD-TARGET(moved)");
    } else
        trace("placeAt REJECT");
    trace("placeAt at=(%.0f,%.0f) ws=%d float=%d", desk.x, desk.y, (int)dw->workspaceID(),
          (int)dw->m_isFloating);
    restoreFloatState(dw);
    g_pCompositor->warpCursorTo(saved, true);
}

// End the running real drag at `at` (desktop coords) — the dwindle insert
// splits whatever is under that point — or back at home when cancelled.
// `splitTarget`: dwindle's use_active_for_splits keys the insert off the
// FOCUSED window; on the desktop focus-follows-mouse focuses the drop
// target during the drag, but the overview swallows motion, leaving focus
// stale (the insert then fell to a default slot — "it goes back home").
// Focusing the target first restores the invariant the machinery expects.
static void endRealDrag(std::optional<Vector2D> at, PHLWINDOW splitTarget) {
    if (!g_dragReal)
        return;
    const SBusyScope busy;
    g_dragReal = false;
    // The compositor's drag target can die while we hold the grab (the
    // window closes mid-drag); dragEnd() dereferences it without a check
    // and takes Hyprland down. No live target → nothing to end — but the
    // grab already floated the window out, so undo that much.
    if (!g_layoutManager->dragController()->target()) {
        trace("end DEAD-TARGET(top)");
        restoreFloatState(g_dragWin.lock());
        return;
    }

    const Vector2D dest  = at.value_or(g_dragHomeCenter);
    const Vector2D saved = g_pInputManager->getMouseCoordsInternal();
    g_pCompositor->warpCursorTo(dest, true);
    if (splitTarget)
        Desktop::focusState()->fullWindowFocus(splitTarget, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    // Un-park BEFORE ending: the float sat at -20000 for the drag's
    // duration, and now that captures run live through the settle, the
    // landing spring would visibly fly it in from offscreen ("laggy").
    // Re-seated at the drop point, the spring is short and reads as a drop.
    if (const auto dw = g_dragWin.lock(); dw && inLayoutSpace(dw)) { // setTargetGeom derefs target->space() unchecked
        const CBox wb = dw->getWindowMainSurfaceBox();
        g_layoutManager->setTargetGeom(CBox{dest.x - wb.w / 2.0, dest.y - wb.h / 2.0, wb.w, wb.h}, dw->layoutTarget());
    }
    g_layoutManager->moveMouse(dest);
    // The check at the top is stale by now: the focus change and the drag
    // motion at the drop point both run through the controller and can drop
    // the target themselves (this exact gap took the compositor down twice
    // on 2026-08-30 — dragEnd() on a target freed during moveMouse).
    if (g_layoutManager->dragController()->target())
        g_layoutManager->endDragTarget();
    else
        trace("end DEAD-TARGET(moved)");
    if (const auto dw2 = g_dragWin.lock())
        trace("end at=(%.0f,%.0f) ws=%d float=%d", dest.x, dest.y, (int)dw2->workspaceID(),
              (int)dw2->m_isFloating);
    restoreFloatState(g_dragWin.lock());
    g_pCompositor->warpCursorTo(saved, true);
}

// ---- Live commit ("the life reaction") -------------------------------------
// The drag previews nothing with stale textures any more: after a short dwell
// on a target the window is REALLY inserted there (actual dwindle split,
// actual cross-view move), the affected workspaces re-tile for real, and the
// live captures show true re-rendered content — the tiles AND the grabbed
// window. Hovering elsewhere pulls it back out (re-grab) and the next dwell
// commits anew. Release on the committed target keeps it; anywhere else falls
// back to the classic drop; outside every view — and on cancel/close — the
// window returns to its ORIGINAL workspace and spot.
// The discrete side of a would-be insert (0 L, 1 R, 2 T, 3 B), mirroring
// dwindle's precise_mouse_move: the cursor's slope from the target's center
// picks the seam — flat side triangles (|dy|/|dx| < h/w) cut side-by-side,
// the steep top/bottom triangles stack.
static int quadrantSide(const CBox& b, const Vector2D& c) {
    const double dx = c.x - (b.x + b.w / 2.0);
    const double dy = c.y - (b.y + b.h / 2.0);
    if (std::abs(dy) * b.w < std::abs(dx) * b.h)
        return dx > 0.0 ? 1 : 0;
    return dy > 0.0 ? 3 : 2;
}

// Draw-space point c mapped to desktop logical coords within tile t
// (inverse of the tile mapping; same math as the drop handler).
static Vector2D deskAt(PHLMONITOR m, const Rect& t, const Vector2D& c) {
    const SUsable u = usableArea(m);
    return {u.x + (c.x - t.x) * u.w / t.w, u.y + (c.y - t.y) * u.h / t.h};
}

// Translate the INTENT — "split `under` on `side`, as seen in the tiles" —
// into a desktop point that makes dwindle's precise_mouse_move pick exactly
// that side against the window's REAL layout geometry. The displayed boxes
// are the committed preview (squeezed halves); mapping the raw cursor onto
// the pulled-out real layout picked the wrong axis ("the axis reordering is
// not working well"). Quarter-points sit safely inside the flat/steep
// triangles for any box aspect, so the choice is deterministic. Goal-based,
// like the mapping: the layout tree moves instantly, pixels lag.
static Vector2D dropPointFor(PHLWINDOW under, int side) {
    const CBox     wb = under->getWindowMainSurfaceBox();
    const Vector2D dp = under->m_realPosition->goal() - under->m_realPosition->value();
    const Vector2D ds = under->m_realSize->goal() - under->m_realSize->value();
    const CBox     b{wb.x + dp.x, wb.y + dp.y, wb.w + ds.x, wb.h + ds.y};
    switch (side) {
        case 0: return {b.x + b.w * 0.25, b.y + b.h * 0.50}; // left of it
        case 1: return {b.x + b.w * 0.75, b.y + b.h * 0.50}; // right of it
        case 2: return {b.x + b.w * 0.50, b.y + b.h * 0.25}; // above it
        default: return {b.x + b.w * 0.50, b.y + b.h * 0.75}; // below it
    }
}

// What a commit at cursor `c` would be: the tile, the window under the
// cursor (if any), and the quadrant side of the would-be insert.
static LiveCommit signatureAt(PHLMONITOR m, const Vector2D& c, PHLWINDOW dw) {
    LiveCommit s;
    Rect       tiles[N_TILES];
    if (computeTiles(m, tiles) != N_TILES)
        return s;
    for (int i = 0; i < N_TILES; ++i)
        if (CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}.containsPoint(c)) {
            s.active = true;
            s.tile   = i;
            break;
        }
    if (!s.active)
        return s;
    for (auto it = g_wins.rbegin(); it != g_wins.rend(); ++it) {
        if (it->screen.w <= 0.0 || !it->screen.containsPoint(c))
            continue;
        if (auto w2 = it->win.lock(); w2 && w2 != dw) {
            s.under = it->win;
            s.side  = quadrantSide(it->screen, c);
            break;
        }
    }
    return s;
}

static bool sameCommit(const LiveCommit& a, const LiveCommit& b) {
    return a.active == b.active && a.tile == b.tile && a.under.lock() == b.under.lock() && a.side == b.side;
}

// Land `dw` at draw-space `c` per signature `sig` — the ONE placement
// machinery, shared by the live commit and the classic release-drop (they
// used to be clones and drifted). Same view: end the grab-drag right at the
// mapped point (the dwindle insert splits `under` exactly like the
// desktop). Cross-view: NEVER move a workspace while the drag is alive
// (they fight over the space — empty-view drops silently failed); close
// the drag, move, then a clean synthetic re-place (an empty view needs no
// re-place: sole window = full view). Returns false if nothing was placed.
static bool landAt(PHLMONITOR m, const Vector2D& c, PHLWINDOW dw, const LiveCommit& sig) {
    if (!sig.active)
        return false;
    const SBusyScope busy;
    Rect             tiles[N_TILES];
    if (computeTiles(m, tiles) != N_TILES)
        return false;
    // Stale-signature net: if the aim point no longer sits in the signature's
    // tile, the mapping would extrapolate to nonsense coordinates — abort and
    // let the caller re-read the world.
    if (!CBox{tiles[sig.tile].x, tiles[sig.tile].y, tiles[sig.tile].w, tiles[sig.tile].h}.containsPoint(c))
        return false;
    const auto     under = sig.under.lock();
    const Vector2D desk  = under ? dropPointFor(under, sig.side) : deskAt(m, tiles[sig.tile], c);
    trace("land tile=%d side=%d under=%p ws=%d desk=(%.0f,%.0f)", sig.tile, sig.side, (void*)under.get(),
          (int)dw->workspaceID(), desk.x, desk.y);
    markDirty(dw->workspaceID());
    g_dirtyTiles |= (1u << sig.tile);
    if (dw->workspaceID() == sig.tile + 1)
        endRealDrag(desk, under);
    else {
        endRealDrag(std::nullopt);
        auto ws = g_pCompositor->getWorkspaceByID(sig.tile + 1);
        if (!ws)
            ws = g_pCompositor->createNewWorkspace(sig.tile + 1, m->m_id);
        if (!ws)
            return false;
        g_pCompositor->moveWindowToWorkspaceSafe(dw, ws);
        // The arrival fade would blink through the live captures (the
        // window shows half-transparent for several snapshots — "traces").
        // Alpha warps clean: no client redraw depends on it.
        dw->m_alpha.warp();
        if (under)
            placeAt(dw, desk, under);
    }
    return true;
}

// Really place the held window at `c` — the same machinery as the drop —
// then capture fast so the tiles show the actual re-tile springing with
// clients re-rendering at their true new sizes. The landed window itself is
// HIDDEN from the tile draws for the whole gesture (a live HOLE marks the
// spot) and the ghost stays on the cursor: the workspaces react to the
// intention, the hand never loses the window.
static void commitAt(PHLMONITOR m, const Vector2D& c, PHLWINDOW dw, const LiveCommit& sig) {
    if (!g_dragReal)
        return;
    const SBusyScope busy;
    if (!landAt(m, c, dw, sig))
        return;
    g_commit     = sig;
    g_lastCommit = Time::steadyNow();
    boostCaptures();
    warpFocusFx();
    captureWorkspaces(m, g_dirtyTiles);
    damageAll();
}

// The hover left the committed spot: pull the window back out of its slot.
// beginRealDrag floats it out and the siblings re-tile back. No synchronous
// capture — a re-commit inserts (and captures) right behind this, and the
// boosted timer covers the pulled-out case within 50ms.
static void regrab(PHLWINDOW dw) {
    if (!g_commit.active)
        return;
    const SBusyScope busy;
    trace("regrab ws=%d", (int)dw->workspaceID());
    g_commit = {};
    beginRealDrag(dw, false);
    boostCaptures();
    damageAll();
}

// Dwell logic, fed by every drag motion AND the live timer (the dwell can
// expire with the cursor at rest). Commit when the signature has been
// stable for DWELL and differs from what's already placed.
static void maybeCommit(PHLMONITOR m) {
    const auto dw = g_dragWin.lock();
    if (!dw || !g_dragMoved)
        return;
    // Hovering the committed window's OWN slot means "stay put" — the
    // insert it would preview is the one already made (signatureAt skips
    // dw, so this reads as an empty-area signature and would re-commit).
    if (g_commit.active && winAt(g_dragCursor).lock() == dw)
        return;
    const auto sig = signatureAt(m, g_dragCursor, dw);
    // Empty space in the view it already occupies isn't a new placement
    // either — re-committing there just churned the layout (and a null
    // split target falls to dwindle's stale-focus default slot).
    if (g_commit.active && sig.active && !sig.under.lock() && sig.tile + 1 == dw->workspaceID())
        return;
    // Only a change of TARGET (tile / under-window) restarts the dwell
    // clock. The quadrant side wobbles with the cursor's slope — and with
    // the hitboxes drifting while post-commit springs settle — and
    // restarting on every wobble kept the clock from ever expiring ("it
    // gets stuck"). A side flip on the same window is deliberate (crossing
    // the diagonal) and commits at once; the cooldown absorbs jitter from
    // a cursor sitting right on the diagonal.
    if (sig.active != g_pending.active || sig.tile != g_pending.tile || sig.under.lock() != g_pending.under.lock()) {
        g_pending      = sig;
        g_pendingSince = Time::steadyNow();
        return;
    }
    g_pending.side = sig.side;
    if (Time::steadyNow() - g_pendingSince < DWELL || sameCommit(sig, g_commit))
        return;
    if (Time::steadyNow() - g_lastCommit < COMMIT_COOLDOWN)
        return;
    // Freeze the aim before the machinery runs — its warps must not move it.
    const Vector2D   at = g_dragCursor;
    const SBusyScope busy;
    if (g_commit.active)
        regrab(dw);
    if (sig.active)
        commitAt(m, at, dw, sig);
}

// Cancel from any state: the window goes back to the workspace and spot the
// gesture started from.
static void restoreOriginal() {
    const SBusyScope busy;
    const auto       dw = g_dragWin.lock();
    if (dw) {
        trace("restore ws=%d -> %d", (int)dw->workspaceID(), g_origWS);
        markDirty(dw->workspaceID());
        markDirty(g_origWS);
    }
    if (dw && g_commit.active)
        regrab(dw); // one mechanism: a held drag we end at the original spot
    g_commit  = {};
    g_pending = {};
    if (!g_dragReal)
        return;
    if (!dw) {
        endRealDrag(std::nullopt);
        return;
    }
    if (g_origWS >= 1 && dw->workspaceID() != g_origWS) {
        endRealDrag(std::nullopt);
        if (const auto ws = g_pCompositor->getWorkspaceByID(g_origWS)) {
            g_pCompositor->moveWindowToWorkspaceSafe(dw, ws);
            dw->m_alpha.warp(); // no arrival-fade blink in the captures
            placeAt(dw, g_origHome, nullptr);
        }
    } else
        endRealDrag(g_origHome, nullptr);
    armFloatWatch(dw); // cancels are gesture ends too
}

// Which border zone of drawn rect `r` is `c` in? Fills the corner Hyprland's
// resize should push, the per-axis mask (edges move one axis, corners both)
// and the cursor-spec shape name. The zone hugs the INSIDE of the rect —
// outside it is the seam, or a sibling. Capped so tiny thumbnails keep a
// grabbable interior for the move gesture.
static constexpr double EDGE_ZONE   = 8.0;  // drawn px, thin band per edge
static constexpr double CORNER_ZONE = 22.0; // corners reach much further along
                                            // both edges — an 8x8 corner was
                                            // unhittable at thumbnail scale
static const char*      edgeZoneAt(const CBox& r, const Vector2D& c, Layout::eRectCorner& corner, Vector2D& mask) {
    const double dl = c.x - r.x, dr = r.x + r.w - c.x;
    const double dt = c.y - r.y, db = r.y + r.h - c.y;
    const double cx = std::min(CORNER_ZONE, r.w / 3.0), cy = std::min(CORNER_ZONE, r.h / 3.0);
    corner          = Layout::CORNER_NONE;
    // Corners first: near two edges at once (generously) is a diagonal.
    if ((dl < cx || dr < cx) && (dt < cy || db < cy)) {
        mask   = {1.0, 1.0};
        corner = dt < cy ? (dl < cx ? Layout::CORNER_TOPLEFT : Layout::CORNER_TOPRIGHT) //
                         : (dl < cx ? Layout::CORNER_BOTTOMLEFT : Layout::CORNER_BOTTOMRIGHT);
        return (corner == Layout::CORNER_TOPLEFT || corner == Layout::CORNER_BOTTOMRIGHT) ? "nwse-resize" : "nesw-resize";
    }
    const double zx = std::min(EDGE_ZONE, r.w / 4.0), zy = std::min(EDGE_ZONE, r.h / 4.0);
    const bool   L = dl < zx, R = dr < zx;
    const bool   T = dt < zy, B = db < zy;
    if (!L && !R && !T && !B)
        return nullptr;
    mask = {L || R ? 1.0 : 0.0, T || B ? 1.0 : 0.0};
    if (T)
        corner = L ? Layout::CORNER_TOPLEFT : Layout::CORNER_TOPRIGHT;
    else if (B)
        corner = R ? Layout::CORNER_BOTTOMRIGHT : Layout::CORNER_BOTTOMLEFT;
    else
        corner = L ? Layout::CORNER_BOTTOMLEFT : Layout::CORNER_BOTTOMRIGHT;
    return mask.x == 0.0 ? "ns-resize" : "ew-resize";
}

// Leave the zone: drop hover state and give the pointer back its default.
static void resetEdgeCursor() {
    g_edgeWin.reset();
    g_edgeCorner = Layout::CORNER_NONE;
    if (!g_edgeShape.empty()) {
        g_pCursorManager->setCursorFromName("left_ptr");
        g_edgeShape.clear();
    }
}

// Apply an overview cursor shape (nullptr = back to the plain arrow). All
// overview cursor changes go through here so exactly one place owns the
// override and the "did it change" check — the desktop must never be left
// wearing one of ours (see the resets on close/exit).
//
// `force` re-applies even when we believe the shape is already set. Needed
// during a real drag: the compositor's own drag machinery sets the cursor
// too, so our cached name goes stale and the shape silently reverts to the
// arrow while the cache still says "grabbing" (Max, 2026-09-01).
// Hyprland has its OWN cursor state for "the pointer is on a window border":
// `CInputManager::m_borderIconDirection`, applied by setCursorIconOnBorder and
// re-asserted from the compositor's own motion handling. It is not our cache
// and not the theme name we set, so setting a shape over it does nothing — the
// resize arrow simply comes back, which is why the pointer kept arriving in the
// overview still wearing it (Max, 2026-09-03, twice). Clearing the direction is
// the only thing that actually releases it.
// setBorderCursorIcon() itself is not exported from the Hyprland binary (the
// plugin failed to load with an undefined symbol), so the direction is cleared
// by writing the member — reachable through the private/public hack at the top
// of this file, and no symbol needed. Clearing it does not repaint on its own;
// it stops the compositor re-asserting, and the shape we set next is what stays.
static void clearBorderResizeIcon() {
    if (g_pInputManager)
        g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
}

// The inconsistency that survived v0.43: the compositor's own motion
// processing runs BEFORE our hook (see the note on onMouseMove), and it
// computes its border icon against the REAL windows still lying under the
// overlay — invisible, but very much there. So a pointer crossing a hidden
// desktop window's border mid-overview got flipped back to ew-resize right
// after we set our shape, depending purely on what happened to be underneath
// (Max, 2026-09-03: "almost solved, with inconsistencies"). Detect exactly
// that — the direction went non-NONE while the overview owns the pointer —
// release it, and re-assert the shape the overview meant to show. A no-op
// branch when the compositor hasn't interfered, so it's safe on every
// motion and every frame.
static void reassertOverviewCursor() {
    if (!g_pInputManager || g_pInputManager->m_borderIconDirection == BORDERICON_NONE)
        return;
    trace("reassert: compositor set borderIcon=%d, reapplying '%s'", (int)g_pInputManager->m_borderIconDirection,
          g_edgeShape.empty() ? "left_ptr" : g_edgeShape.c_str());
    g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
    g_pCursorManager->setCursorFromName(g_edgeShape.empty() ? "left_ptr" : g_edgeShape);
}

static void setOverviewCursor(const char* shape, bool force = false) {
    // Whatever we are about to put on the pointer, the compositor's border
    // icon outranks it — drop that first or it wins the next frame.
    clearBorderResizeIcon();
    if (!shape) {
        // `force` matters here too: the cache only knows about shapes WE set,
        // so an empty cache does not mean the pointer is wearing the arrow —
        // it may still be wearing whatever the desktop left on it. Forcing is
        // how a caller says "I don't know what's on the pointer, make it the
        // arrow" (see the overview taking the pointer over).
        if (force || !g_edgeShape.empty()) {
            if (!g_edgeShape.empty())
                trace("shape '%s' -> left_ptr%s", g_edgeShape.c_str(), force ? " (forced)" : "");
            g_pCursorManager->setCursorFromName("left_ptr");
            g_edgeShape.clear();
        }
        return;
    }
    if (force || g_edgeShape != shape) {
        if (g_edgeShape != shape)
            trace("shape '%s' -> '%s'", g_edgeShape.empty() ? "left_ptr" : g_edgeShape.c_str(), shape);
        g_pCursorManager->setCursorFromName(shape);
        g_edgeShape = shape;
    }
}

// Hover shapes change only once the pointer has MEANT the new zone for a
// beat. The trace from a real session (2026-09-03) showed a card crossing
// flick grab → ew-resize → pointer → ew-resize → grab within 35 ms — four
// cursor repaints nobody asked for, reading as the pointer glitching
// ("keeps failing every once in a while"). A 60 ms dwell swallows transit
// flicker entirely, while a genuine rest in any zone still gets its shape
// faster than a human notices the wait. The closed hand (drag feedback) and
// the open-warp landing bypass the dwell — those must be instant.
static std::string     g_shapeWant;           // desired shape; "" = the arrow
static bool            g_shapeWantSet = false;
static Time::steady_tp g_shapeWantAt{};
static constexpr auto  SHAPE_DWELL = std::chrono::milliseconds(60);

static void settleCursor(const char* shape) {
    const std::string want = shape ? shape : "";
    if (want == g_edgeShape) { // already worn — nothing pending
        g_shapeWantSet = false;
        return;
    }
    if (!g_shapeWantSet || g_shapeWant != want) {
        g_shapeWant    = want;
        g_shapeWantSet = true;
        g_shapeWantAt  = Time::steadyNow();
        return; // keep the current shape until the new one persists
    }
    if (Time::steadyNow() - g_shapeWantAt >= SHAPE_DWELL) {
        g_shapeWantSet = false;
        setOverviewCursor(want.empty() ? nullptr : want.c_str());
    }
}

// Motion stops mid-dwell → no more events to confirm the want; the render
// loop ticks it so a rest always gets its shape.
static void tickCursorSettle() {
    if (g_shapeWantSet && Time::steadyNow() - g_shapeWantAt >= SHAPE_DWELL) {
        g_shapeWantSet = false;
        setOverviewCursor(g_shapeWant.empty() ? nullptr : g_shapeWant.c_str());
    }
}

// While dragging, re-assert the closed hand on a throttle rather than on
// every motion event (a fast mouse delivers hundreds a second, and each
// re-assert re-renders the cursor).
static std::chrono::steady_clock::time_point g_handReassert{};
static void                                  holdGrabbingCursor() {
    g_shapeWantSet = false; // a pending hover want must not overwrite the drag hand
    const auto now = std::chrono::steady_clock::now();
    if (now - g_handReassert < std::chrono::milliseconds(60)) {
        setOverviewCursor("grabbing");
        return;
    }
    g_handReassert = now;
    setOverviewCursor("grabbing", /*force=*/true);
}

// Shared by motion and grid-scroll: recompute hover/drag targets at cursor `c`
// (a scroll moves the tiles under a stationary cursor, so hover must follow).
static void updateHoverAt(PHLMONITOR m, const Vector2D& c) {
    // Any pending press (window or empty tile) that leaves the slop is a drag.
    // !g_dragReal: a live real drag means the origin was already captured —
    // this block must never refire mid-gesture (it would poison g_origFloating
    // and nest a second controller drag; the busy scopes make that impossible,
    // this is the structural belt on top).
    if ((g_dragWin.lock() || g_pressTile >= 0) && !g_dragMoved && !g_dragReal && (c - g_pressPos).size() > CLICK_SLOP) {
        g_dragMoved = true;
        // The grab is real from frame one: the compositor pulls the window
        // out of the layout NOW, so the siblings re-tile live (the
        // thumbnails show it — no hole where the window was).
        if (const auto dw = g_dragWin.lock()) {
            // The gesture's true origin, for cancel — every later re-grab
            // overwrites g_dragHomeCenter with the last committed slot.
            g_origWS         = dw->workspaceID();
            g_origFloating   = dw->m_isFloating;
            const CBox wb    = dw->getWindowMainSurfaceBox();
            g_origHome       = {wb.x + wb.w / 2.0, wb.y + wb.h / 2.0};
            g_pending        = {};
            g_pendingSince   = Time::steadyNow();
            beginRealDrag(dw);
        }
    }

    if (g_dragWin.lock()) {
        g_dragCursor = c;
        maybeCommit(m);
        // AFTER the commit, and every motion: each commit ends and re-begins
        // the compositor's drag, and its dragEnd unsets the "grabbing"
        // override it set — leaving the plain arrow with nothing to restore
        // it. This branch returns before the hover cursor logic below, so
        // the closed hand has to be asserted here or not at all (Max,
        // 2026-09-01: "when i start dragging, it becomes plain arrow").
        holdGrabbingCursor();
        damageAll();
        return;
    }

    // Hover: the window under the cursor gets a border. (Empty tiles are
    // still clickable jump targets — the cursor is the affordance, nothing
    // to draw or track for them.)
    PHLWINDOWREF hov = winAt(c);
    if (hov.lock() != g_hoverWin.lock()) {
        g_hoverWin = hov;
        sendOverviewHover(hov.lock()); // the topbar pill follows the pointer
        damageAll();
    }

    // Border zones: near a thumbnail's edge the pointer flips to a resize
    // shape and the next press resizes instead of moving.
    const char* shape = nullptr;
    if (const auto hw = hov.lock(); hw && !hw->isFullscreen() && inLayoutSpace(hw))
        for (auto it = g_wins.rbegin(); it != g_wins.rend(); ++it)
            if (it->win.lock() == hw && it->screen.w > 0.0) {
                shape = edgeZoneAt(it->screen, c, g_edgeCorner, g_edgeMask);
                break;
            }
    if (shape) {
        g_edgeWin = hov;
        settleCursor(shape);
        return;
    }
    g_edgeWin.reset();
    g_edgeCorner = Layout::CORNER_NONE;

    // Everywhere else the cursor states what the gesture under it WOULD do,
    // in the order the gestures themselves resolve:
    //   dragging now       → closed hand (re-asserted; see holdGrabbingCursor)
    //   over a thumbnail   → open hand: it can be picked up
    //   over an empty tile → finger: a click jumps to that workspace
    //   over the void      → plain arrow
    // (A live resize never reaches here — onMouseMove handles and returns —
    // so its edge shape is left standing rather than overwritten.)
    // (A window drag returned above; this reaches an empty-tile drag.)
    if (g_dragMoved && g_pressTile >= 0)
        holdGrabbingCursor(); // immediate: drag feedback never waits
    else if (hov.lock())
        settleCursor("grab");
    else if (tileAt(m, c) >= 0)
        settleCursor("pointer");
    else
        settleCursor(nullptr);
}

// Scroll while open flips between the two pages, clamped — never a loop.
// A mouse wheel flips per notch; a 2-finger touchpad scroll streams tiny
// deltas, so it accumulates to a travel threshold first. Swallowed either
// way so the desktop underneath never scrolls; the eased glide runs in
// onRender (dt-based).
static double           g_fingerAcc    = 0.0;
static constexpr double FINGER_FLIP_AT = 140.0; // accumulated px per page flip
static void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    // Scrolling inside the focused window is use (desktop-side only).
    if (!g_active && !g_spreadActive)
        noteInteraction(true);
    if (g_spreadActive && !g_active) {
        const auto m = g_spreadMon.lock();
        if (m && inTopbarStrip(m))
            return; // scrolls over the bar are the bar's
        info.cancelled = true; // nothing scrolls in the spread; the desktop must not scroll beneath it
        return;
    }
    if (!g_active || g_animTarget < 0.5f)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
    if (inTopbarStrip(m))
        return; // scrolls over the bar are the bar's (its boxes scroll)
    info.cancelled = true;
    if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;
    const auto now = Time::steadyNow();
    if (std::chrono::duration<double>(now - g_pageFlipAt).count() < 0.3)
        return;
    int dir = 0;
    if (e.source == WL_POINTER_AXIS_SOURCE_FINGER) {
        // Direction change discards stale travel (momentum can't fight you).
        if (e.delta * g_fingerAcc < 0.0)
            g_fingerAcc = 0.0;
        g_fingerAcc += e.delta;
        if (std::abs(g_fingerAcc) < FINGER_FLIP_AT)
            return;
        dir         = g_fingerAcc > 0.0 ? 1 : -1;
        g_fingerAcc = 0.0;
    } else {
        dir = e.delta > 0.0 ? 1 : e.delta < 0.0 ? -1 : 0;
    }
    const int next = std::clamp(g_page + dir, 0, 1);
    if (next == g_page)
        return;
    g_page         = next;
    g_pageFlipAt   = now;
    g_tourDone     = true; // wheel counts as touring — next Super+R closes
    g_scrollFrom   = g_scroll; // retarget-safe: a mid-flight flip re-eases
    g_scrollProg   = 0.0f;
    g_scrollTarget = g_page * pageStep(m);
    damageAll();
}

// Left-click while open: press over a window picks it up; release drops it onto the
// tile under the cursor, moving it to that workspace. All left-clicks are swallowed
// so nothing leaks through to the desktop underneath.
static void onMouseButton(IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
    // Resize-drag watch: the press that starts a border resize is processed
    // by the compositor around our listener — check now AND shortly after,
    // so a click-and-hold shows the size before any movement.
    checkResizeDrag();
    if (g_dragCheckTimer)
        g_dragCheckTimer->updateTimeout(std::chrono::milliseconds(30));
    // Clicking INTO the focused window is use (desktop-side only).
    if (!g_active && !g_spreadActive && e.state == WL_POINTER_BUTTON_STATE_PRESSED)
        noteInteraction(true);
    if (g_spreadActive && !g_active) {
        if (e.button != BTN_LEFT)
            return;
        const auto m = g_spreadMon.lock();
        if (!m)
            return;
        if (inTopbarStrip(m))
            return; // NOT cancelled — the bar handles the click
        info.cancelled = true;
        if (e.state != WL_POINTER_BUTTON_STATE_RELEASED)
            return; // the release decides — there are no drags in the spread
        const Vector2D c = cursorDrawSpace(m);
        PHLWINDOW      w;
        for (auto it = g_spreadWins.rbegin(); it != g_spreadWins.rend(); ++it)
            if (it->screen.w > 0.0 && it->screen.containsPoint(c)) {
                w = it->win.lock();
                break;
            }
        if (w)
            pickSpread(w); // this window, on top, focused
        else
            closeSpread(); // the space itself: home, untouched
        return;
    }
    if (!g_active || g_animTarget < 0.5f || e.button != BTN_LEFT)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
    // A press on the OPTIONS strip belongs to the bar (its X closes us); a
    // release mid-gesture stays ours so a drag that ended up under the bar
    // still lands.
    if (!g_resizing && !g_dragWin.lock() && inTopbarStrip(m))
        return; // NOT cancelled — the bar handles the click
    info.cancelled = true;

    const Vector2D c = cursorDrawSpace(m);
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (g_resizing)
            return; // already mid-resize (stray second press)
        // Border press: the pointer sits on a resize zone — this gesture
        // resizes. The interior grab below moves.
        if (const auto ew = g_edgeWin.lock(); ew && !ew->isFullscreen() && inLayoutSpace(ew) && !g_dragWin.lock()) {
            Rect      tiles[N_TILES];
            const int t = waveview_tile_for_workspace(ew->workspaceID());
            if (t >= 0 && computeTiles(m, tiles) == N_TILES && tiles[t].w > 0.0 && tiles[t].h > 0.0) {
                const SUsable u = usableArea(m);
                g_resizing     = true;
                g_resizeWin    = ew;
                g_resizeLast   = c;
                g_resizeCorner = g_edgeCorner;
                g_resizeMask   = g_edgeMask;
                g_resizeScale  = {u.w / tiles[t].w, u.h / tiles[t].h};
                g_watchWin.reset(); // dwindle ratio pushes are not float leaks
                trace("resize grab ws=%d corner=%d mask=(%.0f,%.0f) class=%s", (int)ew->workspaceID(), (int)g_resizeCorner, g_resizeMask.x, g_resizeMask.y, ew->fetchClass().c_str());
                boostCaptures();
                return;
            }
        }
        g_pressPos  = c;
        g_dragMoved = false;
        g_pressTile = -1;
        g_watchWin.reset(); // a fresh gesture takes over: the grab's own
                            // float-out must not read as a leak
        if (const auto w = winAt(c).lock()) {
            g_dragWin    = w;
            g_dragCursor = c;
            // grab offset from the window's drawn top-left, so it tracks naturally
            for (auto& cw : g_wins)
                if (cw.win.lock() == w) {
                    const Vector2D grab{c.x - cw.screen.x, c.y - cw.screen.y};
                    g_grabFracX = cw.screen.w > 0.0 ? std::clamp(grab.x / cw.screen.w, 0.0, 1.0) : 0.5;
                    g_grabFracY = cw.screen.h > 0.0 ? std::clamp(grab.y / cw.screen.h, 0.0, 1.0) : 0.5;
                    g_ghostW = g_ghostWantW = cw.screen.w;
                    g_ghostH = g_ghostWantH = cw.screen.h;
                    break;
                }
            // The hand takes hold on the PRESS, not on the first movement:
            // open for a beat, then closed (see onHandTimer).
            setOverviewCursor("grab", /*force=*/true);
            if (g_handTimer)
                g_handTimer->updateTimeout(std::chrono::milliseconds(HAND_CLOSE_MS));
            damageAll();
        } else {
            const int t = tileAt(m, c);
            if (t >= 0 && tileEmpty(t))
                g_pressTile = t; // press landed on an empty workspace → candidate jump
        }
        return;
    }

    // Released mid-resize: sizes applied live are final — settle and re-read
    // the zone (the border may have moved out from under the pointer).
    if (g_resizing) {
        endRealResize();
        updateHoverAt(m, c);
        return;
    }
    // Released: a click (never left the slop) jumps — to the window, or to an empty
    // workspace; a real drag drops the window onto the tile under the cursor.
    const auto dw        = g_dragWin.lock();
    const bool moved     = g_dragMoved;
    const int  pressTile = g_pressTile;
    // g_dragWin stays set through the drop logic below: endRealDrag's
    // un-park and restoreOriginal's workspace-restore both read it.
    g_dragMoved = false;
    g_pressTile = -1;
    // Hand opens again on release; the hover pass below re-decides the shape
    // from whatever the pointer now sits on.
    setOverviewCursor(nullptr);
    if (dw && !moved) {
        g_dragWin.reset();
        jumpToWindow(dw); // click → switch to & focus that window, closing the overview
        return;
    }
    if (!dw && !moved && pressTile >= 0) {
        g_dragWin.reset();
        jumpTo(pressTile + 1); // click on an empty workspace → jump there, closing the overview
        return;
    }
    if (!dw && moved) {
        g_dragWin.reset();
        endRealDrag(std::nullopt); // grabbed window died mid-drag: fold the grab, keep no state
        g_commit  = {};
        g_pending = {};
        return;
    }
    if (dw) {
        // Released while the intention is REALLY placed under the cursor
        // (the cursor sits on the live hole, or on the committed target):
        // reveal the window in place — nothing to end, move, or fake.
        if (g_commit.active && (winAt(c).lock() == dw || sameCommit(signatureAt(m, c, dw), g_commit))) {
            // The one gesture end with NO net of its own (the commit's
            // endRealDrag already netted) — watch it extra closely.
            trace("release REVEAL ws=%d float=%d", (int)dw->workspaceID(), (int)dw->m_isFloating);
            armFloatWatch(dw);
            g_dragWin.reset();
            g_commit  = {};
            g_pending = {};
            g_origWS  = -1;
            damageAll();
            return;
        }
        if (g_commit.active)
            regrab(dw); // released elsewhere before the dwell: pull out, classic drop below
        g_pending = {};
        // The classic drop is a fresh signature landed via the SAME machinery
        // as a live commit (they used to be hand-rolled clones of each other).
        if (!landAt(m, c, dw, signatureAt(m, c, dw)))
            restoreOriginal(); // dropped outside every view: back to where it came from
        // The gesture is over: the final landing is LIVE — capture now and
        // keep capturing fast while the real windows spring into place
        // (the un-park in endRealDrag keeps the spring short).
        g_dragWin.reset();
        g_commit  = {};
        g_origWS  = -1;
        restoreFloatState(dw); // last-resort net: no gesture may leak a float
        trace("release settled ws=%d float=%d", (int)dw->workspaceID(), (int)dw->m_isFloating);
        armFloatWatch(dw);
        boostCaptures();
        warpFocusFx(); // the drop's focus churn must not glow through the settle captures
        captureWorkspaces(m, g_dirtyTiles ? g_dirtyTiles : ALL_TILES);
    }
    // Re-decide the cursor for wherever the drop left the pointer (the tiles
    // just re-laid out under it).
    updateHoverAt(m, c);
    damageAll();
}

// Put the pointer on the thumbnail of the window the overview was called from
// (see the open-warp note by `g_warpWin`). Called on the first settled frame,
// when `CapWin::screen` holds the final boxes. One shot per open, whether or
// not a target is found — a warp owed forever would fire on some later frame
// after the user had moved on.
static void warpToOpeningWindow() {
    g_warpPending = false;
    const auto w  = g_warpWin.lock();
    g_warpWin.reset();
    // ALWAYS the capture monitor — never the monitor whose render pass we
    // happen to be in. onRender fires once per monitor, and on a multi-head
    // desk another head's pass usually reaches the settle first; converting
    // the thumbnail centre through THAT head's position and scale put the
    // pointer somewhere arbitrary, and the follow-up hover then dressed it
    // as resize / hand / arrow depending on what it found (Max, 2026-09-03:
    // "at any shape and supr+r will change the shape … resize/hand/plain
    // arrow"). Single-monitor test rigs never see this; his desk always did.
    const auto m = g_captureMon.lock();
    if (!w || !m)
        return;
    for (const auto& cw : g_wins) {
        if (cw.win.lock() != w || cw.screen.w <= 0.0)
            continue;
        const Vector2D centre{cw.screen.x + cw.screen.w / 2.0, cw.screen.y + cw.screen.h / 2.0};
        {
            const SBusyScope busy; // our own warp must not feed the hover machinery
            g_pCompositor->warpCursorTo(drawSpaceToGlobal(m, centre), true);
        }
        // The pointer arrives wearing whatever the DESKTOP last put on it — an
        // ew-resize from a window edge, a text caret, a client's own cursor
        // (Max, 2026-09-03: "if im resizing and i call overview, the pointer
        // goes to the window but on resize shape"). Wipe it to the arrow and
        // drop the cache with it, so the hover logic below cannot look at a
        // cache that says "nothing of ours is set" and conclude there is
        // nothing to change.
        const int borderIconWas = g_pInputManager ? (int)g_pInputManager->m_borderIconDirection : -1;
        setOverviewCursor(nullptr, /*force=*/true);
        // Land fully arrived: the hover border, the topbar pill and the cursor
        // shape all describe where the pointer now is, not where it came from.
        // Hover from the point we just warped TO, not from a re-read of the
        // mouse coords — the warp may not have propagated back through the
        // input stack yet, and hovering the OLD point put the plain arrow on a
        // pointer sitting squarely on a thumbnail (should be the open hand).
        updateHoverAt(m, centre);
        tickCursorSettle(); // the landing shape is applied NOW, not a dwell later
        if (g_shapeWantSet) {
            g_shapeWantSet = false;
            setOverviewCursor(g_shapeWant.empty() ? nullptr : g_shapeWant.c_str());
        }
        endWarpHide(); // shown only now — with the right shape, in the right place
        damageAll();
        // Traced together because they are one story: what the desktop had put
        // on the pointer, and what the overview decided instead.
        trace("open warp -> ws=%d borderIconWas=%d shape='%s' hover=%d", (int)w->workspaceID(), borderIconWas,
              g_edgeShape.empty() ? "left_ptr" : g_edgeShape.c_str(), (int)(g_hoverWin.lock() != nullptr));
        return;
    }
    // No thumbnail for it (fullscreen, a special workspace, off-grid): leave
    // the pointer where the user put it rather than guess — but show it again.
    endWarpHide();
    trace("open warp: focused window has no tile; pointer left alone");
}

// Logical layout coords → this monitor's draw space (whole monitor =
// [0,0,transformedSize]) — where the spread's cards are drawn and hit-test.
static CBox logicalToDraw(PHLMONITOR m, const Rect& r) {
    const double sx = m->m_transformedSize.x / m->m_size.x;
    const double sy = m->m_transformedSize.y / m->m_size.y;
    return CBox{(r.x - m->m_position.x) * sx, (r.y - m->m_position.y) * sy, r.w * sx, r.h * sy};
}

// Draw the spread: the wallpaper backdrop covering the real desktop, then
// every card mixed home→slot by `p`. At p=0 each card sits EXACTLY over its
// real window — same box, same corner radius and power, scaled with the card
// — so the open is seamless and the pile visibly exhales from where it stood.
static void drawSpread(PHLMONITOR m, float p) {
    if (const auto bg = g_bgFB ? g_bgFB->getTexture() : nullptr) {
        CTexPassElement::SRenderData td;
        td.tex = bg;
        td.box = CBox{0.0, 0.0, m->m_transformedSize.x, m->m_transformedSize.y};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
    }

    const auto hoverW  = g_spreadHover.lock();
    const auto pickW   = g_spreadPick.lock();
    const bool settled = p >= 0.999f && g_spreadTarget >= 0.5f;
    bool       moving  = false;

    // Two passes: everyone, then the picked card — it ends on top of the real
    // stack, so it glides home on top of the cards too.
    for (int pass = 0; pass < 2; ++pass) {
        for (auto& sw : g_spreadWins) {
            const auto w = sw.win.lock();
            if (!w)
                continue;
            if ((pass == 1) != (pickW && w == pickW))
                continue;
            const auto tex = sw.fb ? sw.fb->getTexture() : nullptr;
            if (!tex)
                continue;

            const CBox home = logicalToDraw(m, sw.home);
            const CBox goal = logicalToDraw(m, sw.spread);
            CBox box{mix(home.x, goal.x, p), mix(home.y, goal.y, p), std::max(1.0, mix(home.w, goal.w, p)),
                     std::max(1.0, mix(home.h, goal.h, p))};

            // Settled: chase re-layouts at frame rate (a window joining or
            // leaving mid-spread glides, the overview's draw-side lesson).
            // In motion: the mix owns the box outright.
            if (!settled || sw.drawCur.w <= 0.0) {
                sw.drawCur = box;
            } else {
                const double kc = std::min(1.0, (double)g_frameDt * PREVIEW_RATE);
                sw.drawCur      = CBox{sw.drawCur.x + (box.x - sw.drawCur.x) * kc, sw.drawCur.y + (box.y - sw.drawCur.y) * kc,
                                       sw.drawCur.w + (box.w - sw.drawCur.w) * kc, sw.drawCur.h + (box.h - sw.drawCur.h) * kc};
                if (std::abs(sw.drawCur.x - box.x) + std::abs(sw.drawCur.y - box.y) + std::abs(sw.drawCur.w - box.w) +
                        std::abs(sw.drawCur.h - box.h) >
                    1.0)
                    moving = true;
                box = sw.drawCur;
            }
            sw.screen = box;

            // The card IS the window as it looks — bar, border, corners and
            // even the corner shadow are baked in the capture (Max: bars
            // stay on the spread, vanish only in the overview) — so the
            // texture draws plain, pixel-identical to the desktop at p=0.
            // The only ring WE add is the affordance: the hover, and the
            // picked card riding home (its real window lands focused).
            if ((settled && hoverW && w == hoverW) || (pickW && w == pickW)) {
                const double cs = home.w > 0.0 ? box.w / home.w : 1.0;
                haloAround(m, box, (int)std::lround(w->rounding() * m->m_scale * std::clamp(cs, 0.0, 1.0)), true,
                           (float)w->roundingPower());
            }

            CTexPassElement::SRenderData td;
            td.tex = tex;
            td.box = box;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
        }
    }
    if (moving) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

// The spread's slice of the render hook: advance its own clock, draw, and
// disengage at fully-closed. Mirrors the overview's flow exactly.
static void onRenderSpread(PHLMONITOR m) {
    const auto now = Time::steadyNow();
    float      dt  = std::chrono::duration<float>(now - g_spreadLastT).count();
    g_spreadLastT  = now;
    if (dt <= 0.f || dt > 0.1f)
        dt = 0.016f;
    g_frameDt        = dt;
    const float step = dt / (g_spreadTarget >= 0.5f ? ANIM_SECONDS : CLOSE_SECONDS);
    if (g_spreadAnim < g_spreadTarget)
        g_spreadAnim = std::min(g_spreadTarget, g_spreadAnim + step);
    else if (g_spreadAnim > g_spreadTarget)
        g_spreadAnim = std::max(g_spreadTarget, g_spreadAnim - step);

    if (g_spreadTarget <= 0.f && g_spreadAnim <= 0.f) {
        finishSpreadClose(); // cards are home; the real desktop beneath is already correct
        damageAll();
        return;
    }

    drawSpread(m, spreadProgress());

    if (g_spreadAnim != g_spreadTarget) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

// The zoom's progress curve, per DIRECTION — the same law the spread's
// spreadProgress() follows, for the same reason (Max, 2026-09-16, both rungs:
// "too aggressive"): each leg leaves fast and LANDS gently. Reusing the
// opening curve backwards is ease-in — the close crept across the grid, then
// slammed into the workspace at max velocity, straight into the hand-off
// cut. Opening: 1-(1-a)³ (fast off the workspace, soft dock onto the grid).
// Closing: a³ (fast off the grid, soft landing on the workspace). Retargets
// remap `g_anim` onto the new curve so the zoom turns around from exactly
// where it is.
static float zoomProgress() {
    return g_animTarget >= 0.5f ? easeOutCubic(g_anim) : (float)easeInOutCubic(g_anim);
}

// The minimize FLY: decorated cards sailing into (or out of) the dock. Own
// clock, per-monitor, runs with or without any overlay up; the land message
// tells the daemon exactly when to show the dock entry, so the thumbnail
// appears as the card arrives.
static void drawMinFlies(PHLMONITOR m) {
    if (g_minFlies.empty())
        return;
    const auto now = Time::steadyNow();
    float      dt  = std::chrono::duration<float>(now - g_minFlyLastT).count();
    g_minFlyLastT  = now;
    if (dt <= 0.f || dt > 0.1f)
        dt = 0.016f;
    bool mine = false;
    for (auto& f : g_minFlies) {
        if (f.mon.lock() != m)
            continue;
        mine = true;
        f.t  = std::min(1.0f, f.t + dt / MINFLY_SECONDS);
        const float p = (float)easeInOutCubic(f.t);
        const CBox  box{mix(f.from.x, f.to.x, p), mix(f.from.y, f.to.y, p), std::max(1.0, mix(f.from.w, f.to.w, p)),
                        std::max(1.0, mix(f.from.h, f.to.h, p))};
        if (const auto tex = f.fb ? f.fb->getTexture() : nullptr) {
            CTexPassElement::SRenderData td;
            td.tex   = tex;
            td.box   = box;
            td.round = (int)std::lround(DSN_WIN_ROUND * m->m_scale * p); // corners grow as it becomes a "tile"
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
        }
    }
    bool reaped = false;
    std::erase_if(g_minFlies, [&](MinFly& f) {
        if (f.t < 1.0f)
            return false;
        if (!f.landMsg.empty())
            sendWaverunner(f.landMsg);
        if (f.fb) {
            g_minReap.push_back(f.fb); // freed off-frame — never in the render callback
            reaped = true;
        }
        return true;
    });
    if (reaped && g_minReapTimer)
        g_minReapTimer->updateTimeout(std::chrono::milliseconds(250));
    if (mine) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

static void onRender(eRenderStage stage) {
    if (g_capturing || stage != eRenderStage::RENDER_POST_WINDOWS)
        return;
    if (const auto fm = g_pHyprRenderer->m_renderData.pMonitor.lock())
        drawMinFlies(fm); // under any overlay, above the desktop
    if (g_spreadActive && !g_active) {
        if (const auto m = g_pHyprRenderer->m_renderData.pMonitor.lock(); m && m == g_spreadMon.lock())
            onRenderSpread(m);
        return;
    }
    if (!g_active)
        return;
    const auto m = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!m)
        return;

    // The answer never came (waverunner gone, a daemon too old to know
    // `stage-resume`): the close was never waiting on it, so this only drops the
    // expectation — a late answer must not re-photograph into an unrelated close.
    if (g_stageClosePending && Time::steadyNow() - g_stageCloseAt > STAGE_CLOSE_WAIT)
        g_stageClosePending = false;

    // Per-frame belt to the motion-hook fix: anything that re-applied the
    // compositor's border icon between events (a refocus, a warp, a layout
    // change — not everything arrives as pointer motion) is undone within a
    // frame. Only while we own the pointer; the close animation hands it back.
    if (g_animTarget > 0.5f) {
        reassertOverviewCursor();
        tickCursorSettle(); // a rest mid-dwell still gets its shape
    }

    // Advance the zoom animation by wall-clock dt (guard first-frame / stalls).
    const auto now = Time::steadyNow();
    float      dt  = std::chrono::duration<float>(now - g_animLastT).count();
    g_animLastT    = now;
    if (dt <= 0.f || dt > 0.1f)
        dt = 0.016f;
    g_frameDt        = dt;
    // Both zoom legs: the close's calm comes from the landing act, not a slow
    // zoom. The stage's way back has no landing act, so it sets its own clock
    // here instead (`beginStageCloseZoom`).
    const float step = dt / (g_zoomSeconds > 0.f ? g_zoomSeconds : ANIM_SECONDS);
    if (g_anim < g_animTarget)
        g_anim = std::min(g_animTarget, g_anim + step);
    else if (g_anim > g_animTarget)
        g_anim = std::max(g_animTarget, g_anim - step);

    // The close's SECOND ACT: the zoom is (nearly) landed with the tile's
    // arrangement intact; the spread-style glide walks the windows home.
    if (g_animTarget < 0.5f && g_anim <= LAND_EARLY && g_landAnim > 0.f)
        g_landAnim = std::max(0.0f, g_landAnim - dt / LAND_SECONDS);

    // Page-flip scroll: a fixed-duration ease-in-out glide (dt-based) —
    // gentle start, gentle landing (the old exponential chase hit max
    // velocity on frame one and read as a jerk). While it moves, hover
    // retargets under the stationary cursor and frames keep coming.
    if (g_scrollProg < 1.0f) {
        g_scrollProg = std::min(1.0f, g_scrollProg + dt / SCROLL_SECONDS);
        g_scroll     = mix(g_scrollFrom, g_scrollTarget, easeInOutCubic(g_scrollProg));
        updateHoverAt(m, cursorDrawSpace(m));
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    } else if (g_scroll != g_scrollTarget) {
        g_scroll = g_scrollTarget;
        g_pHyprRenderer->damageMonitor(m);
    }

    // Fully closed: LINGER two frames, then disengage. The overlay at rest
    // (pl = 0, chrome at full) is pixel-identical to the desktop, so holding
    // it while the desktop beneath composes a couple of full frames makes
    // the hand-off unable to catch a half-painted frame — the BLINK was a
    // racy one-frame black gap where the wallpaper region hadn't repainted
    // at the swap (caught on video, 2026-09-16; absent on clean runs).
    if (g_animTarget <= 0.f && g_anim <= 0.f && g_landAnim <= 0.f) {
        if (g_closeLinger > 0) {
            if (g_stageDimAsk) {
                g_stageDimAsk = false;
                sendWaverunner("stage-dim\n");
            }
            // The stage's dim goes back on HERE — under the overlay, which is
            // still covering the whole screen for these linger frames, so the
            // change itself is never seen. Told at the disengage instead, the
            // daemon's round trip (plus its own work on that path) left two or
            // three frames of UNDIMMED desktop on screen: Max, 2026-09-17,
            // *"i see the brighter bg settle on the stage"*. The linger is
            // lengthened for a stage close precisely to cover this round trip.
            --g_closeLinger;
            drawOverview(m, 0.0f, 0.0f, g_zoomTile);
            g_pHyprRenderer->damageMonitor(m);
            g_pCompositor->scheduleFrameForMonitor(m);
            return;
        }
        trace("close DISENGAGE");
        notifyWaverunner(false);
        g_active            = false;
        g_zoomSeconds       = 0.0f;  // the stage's clock dies with its close
        g_stageClosePending = false; // and a late answer must not fire into the next one
        g_stageDimPaint     = false;
        g_freePending = true; // captures outlive the hand-off; the timer frees them once the desktop has settled
        g_hoverWin.reset();
        g_dragWin.reset();
        g_resizing = false;
        g_resizeWin.reset();
        resetEdgeCursor();
        g_pressTile = -1;
        if (g_liveTimer)
            g_liveTimer->updateTimeout(std::chrono::milliseconds(450)); // one last tick: the deferred VRAM free
        damageAll();
        return;
    }

    // The layout blend rides its own clock: opening, the windows drift into
    // their seats while shrinking; closing, it holds the arrangement through the
    // zoom and only then glides home, flat.
    //
    // ⭐ OPENING, the spread runs on a clock of its OWN (`OPEN_SPREAD_SECONDS`),
    // not on the zoom's. Tied to `zoomProgress` it could only ever finish when
    // the zoom did — and with the open down to 0.14s that crammed the whole
    // reveal into 140ms whatever curve it was given. Its own clock is the only
    // way it can be asked to go SLOWER than the map opens (Max, 2026-09-17,
    // after asking twice for faster and once for slower — the two requests are
    // only compatible if these are separate clocks). It keeps drifting after the
    // map has settled, which is what reads as the windows easing apart rather
    // than snapping. **The dial.**
    float pl;
    if (g_animTarget >= 0.5f) {
        g_openBlend = std::min(1.0f, g_openBlend + dt / OPEN_SPREAD_SECONDS);
        pl          = (float)easeInOutCubic(g_openBlend);
    } else if (g_anim > LAND_EARLY) {
        pl = 1.0f;
    } else {
        pl = (float)easeInOutCubic(g_landAnim);
    }
    drawOverview(m, zoomProgress(), pl, g_zoomTile);

    // The zoom has landed and every thumbnail now has its final box: put the
    // pointer on the one we came from. (The warp resolves the CAPTURE monitor
    // itself — `m` here is whichever head is currently rendering.)
    if (g_warpPending && g_anim >= g_animTarget)
        warpToOpeningWindow();

    // Keep frames coming while the zoom — or the landing glide — still moves.
    if (g_anim != g_animTarget || (g_animTarget < 0.5f && g_landAnim > 0.f)) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

// Fire-and-forget a control verb to waverunner (the dock/topbar daemon)
// over its socket on a detached thread; a dead daemon = nothing to tell.
// Takes the message BY VALUE and moves it into the thread: callers build
// verbs with payloads (titles, sizes) in temporaries, and a captured
// `const char*` into one of those would dangle before the write.
// Not static: Golem's titlebars (src/hyprbars/) send through it too — a button
// there is a Golem action, so it goes to the daemon rather than to the
// compositor.
void sendWaverunner(std::string msg) {
    std::thread([msg = std::move(msg)] {
        const char* rt = getenv("XDG_RUNTIME_DIR");
        if (!rt)
            return;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/waverunner.sock", rt);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            (void)!write(fd, msg.data(), msg.size());
        close(fd);
    }).detach();
}

// Tell waverunner the overview state, so it conceals its surfaces while we
// own the screen. Every open/close path funnels through here, so this is
// also where the topbar-pill overrides are forgotten (the daemon drops them
// on overview-off; we drop our "already sent" memory to match).
static void notifyWaverunner(bool on) {
    resetOverviewPill();
    sendWaverunner(on ? "overview-on\n" : "overview-off\n");
}

// Tell waverunner's STAGE it may put its shape back — sent the moment a close
// STARTS, not at the touchdown `notifyWaverunner(false)` above.
//
// The map lifts the stage's concealment while it is up (un-maximized, floats
// unparked) so it can show the whole workspace to pick from. Told only at the
// disengage, waverunner rebuilt the stage AFTER the hand-off: the close landed
// on the bare desktop and the stage snapped in a beat later — "it goes to
// normal, and then focus the window and go to stage" (Max, 2026-09-17). Sent
// here, the rebuild happens behind the still-covering overlay and the close
// lands on the stage.
//
// Deliberately NOT an earlier `overview-off`: that one waits for touchdown on
// purpose, because the dock popping back mid-animation reads as aggressive.
// This is one extra word, for the stage alone, and the daemon ignores it unless
// the map actually suspended the stage.
//
// Only while the stage owns the screen, so an ordinary overview close says
// nothing new. Call it once the pick's focus is applied — waverunner stages
// whatever the map left focused.
static void tellStageResume() {
    if (g_stageMode)
        sendWaverunner("stage-resume\n");
}

// Tell waverunner when a window RESIZE drag begins/ends (a border click, or
// Super+RMB), so the topbar can show the live size from the CLICK onward.
// The compositor emits no drag events, but the drag controller's state is
// public — this is a cheap read + bool compare, called from mouse events.
// The overview's own grid drags run MBIND_MOVE (and its live resize calls
// resizeTarget directly, no drag state), so neither false-triggers.
static bool g_resizeDragSent = false;
static void checkResizeDrag() {
    const auto& dc   = g_layoutManager->dragController();
    const auto  mode = dc->mode();
    const bool on   = dc->target() &&
        (mode == MBIND_RESIZE || mode == MBIND_RESIZE_BLOCK_RATIO || mode == MBIND_RESIZE_FORCE_RATIO);
    if (on == g_resizeDragSent)
        return;
    g_resizeDragSent = on;
    sendWaverunner(on ? "resize-drag-on\n" : "resize-drag-off\n");
}

static void onDragCheckTimer(SP<CEventLoopTimer> self, void*) {
    checkResizeDrag();
}

static void onHandTimer(SP<CEventLoopTimer> self, void*) {
    if (g_active && (g_dragWin.lock() || g_pressTile >= 0))
        setOverviewCursor("grabbing", /*force=*/true);
}

// Free finished fly textures between frames (see g_minReap): a big GL free
// inside the render callback blinks the wallpaper.
static void onMinReapTimer(SP<CEventLoopTimer>, void*) {
    for (auto& fb : g_minReap)
        if (fb)
            fb->release();
    g_minReap.clear();
}

// --- Overview → topbar (waverunner draws the bar over the overview) --------
// The current-task pill follows the POINTER while we own the screen: it
// shows the hovered thumbnail's title, plus the live size while a thumbnail
// is being resized. Both are fire-and-forget verbs; the resize stream is
// throttled because it rides pointer motion (one socket write per motion
// event would be one detached thread per event).
static std::string                                 g_sentHoverLine;
static bool                                        g_sentHoverValid = false;
static std::string                                 g_sentSize;
static std::chrono::steady_clock::time_point       g_sizeSentAt{};
static constexpr std::chrono::milliseconds         SIZE_SEND_EVERY{50};

static void sendOverviewHover(PHLWINDOW w) {
    // The ADDRESS rides in front of the title. The pill only ever needed the
    // words, but the daemon needs to know which window they belong to: entering
    // the stage from the map stages the one under the pointer, and by then the
    // map is closing and there is nothing left to ask.
    const std::string payload =
        w ? std::format("0x{:x} {}", (uintptr_t)w.get(), w->m_title) : std::string{};
    if (g_sentHoverValid && payload == g_sentHoverLine)
        return;
    g_sentHoverLine  = payload;
    g_sentHoverValid = true;
    // Titles can hold anything except our line terminator; strip newlines.
    std::string line = "overview-hover " + payload;
    for (auto& c : line)
        if (c == '\n' || c == '\r')
            c = ' ';
    line += '\n';
    sendWaverunner(std::move(line));
}

// `w == nullptr` ends the readout.
static void sendOverviewSize(PHLWINDOW w, bool force) {
    std::string size;
    if (w) {
        const auto s = w->m_realSize->goal();
        size = std::format("{}x{}", (int)s.x, (int)s.y);
    }
    if (size == g_sentSize)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && !size.empty() && now - g_sizeSentAt < SIZE_SEND_EVERY)
        return; // throttle the motion-driven stream
    g_sentSize   = size;
    g_sizeSentAt = now;
    sendWaverunner(size.empty() ? std::string("overview-resize\n") : "overview-resize " + size + "\n");
}

// Opening/closing forgets what the pill was last told, so the next hover
// always re-sends (the daemon drops both overrides when the overview ends).
static void resetOverviewPill() {
    g_sentHoverValid = false;
    g_sentHoverLine.clear();
    g_sentSize.clear();
}

// --- Interaction watch (feeds waverunner's focus-cycle frecency) ------------
// One "interacted" per window-visit: the FIRST key/click/scroll the user aims
// at the focused window tells the daemon "this window is being used" — it
// commits an in-flight focus walk and earns the window its usage point.
// Super-chords never count (they're binds — the Super+Tab that drives the
// cycle must not commit it), and clicks/scrolls only count with the cursor
// inside the focused window's box (a click on the topbar pill is the cycle
// itself, not window use). Keyboard counts wherever the pointer rests —
// typing with the mouse parked on the pill is still working in the window.
static std::string g_interactEpisode; // focused window the last notify was for
static bool        g_interactSent = false;

static void noteInteraction(bool pointer) {
    const auto w = Desktop::focusState()->window();
    if (!w)
        return;
    if (pointer) {
        const auto c = g_pInputManager->getMouseCoordsInternal();
        const auto p = w->m_realPosition->value();
        const auto s = w->m_realSize->value();
        if (c.x < p.x || c.y < p.y || c.x > p.x + s.x || c.y > p.y + s.y)
            return; // aimed at a layer or another window, not this one
    }
    auto addr = std::format("0x{:x}", reinterpret_cast<uintptr_t>(w.get()));
    if (addr != g_interactEpisode) {
        g_interactEpisode = addr;
        g_interactSent    = false;
    }
    if (g_interactSent)
        return;
    g_interactSent = true;
    sendWaverunner("interacted\n");
}

// Whether any window lives on `page` (0 = workspaces 1-9, 1 = 10-18),
// judged from the open capture set.
static bool pageHasWindows(int page) {
    for (auto& cw : g_wins)
        if (cw.tile / 9 == page)
            return true;
    return false;
}

// Eased flip to `page` (shared by the wheel and the Super+R tour).
static void flipToPage(int page) {
    const auto m = g_captureMon.lock();
    if (!m || page == g_page)
        return;
    g_page         = page;
    g_scrollFrom   = g_scroll;
    g_scrollProg   = 0.0f;
    g_scrollTarget = g_page * pageStep(m);
    damageAll();
}

// Close unconditionally (Escape's path — no touring).
// Put the close on its curve. The zoom flies into `g_zoomTile`, so whatever that
// tile's snapshot holds is what the user watches grow to fill the screen.
static void beginCloseZoom() {
    g_zoomSeconds = 0.0f; // an ordinary close is the ordinary clock, never the open's
    // Onto the closing curve from exactly the current progress; the landing
    // glide (the close's second act) is armed behind the zoom. The daemon is
    // told OFF when the close has LANDED (the disengage in onRender), never
    // now — the dock popping back mid-animation was part of the "still
    // aggressive" (the spread always had this right: off on touchdown).
    g_anim        = invEaseInOutCubic(easeOutCubic(g_anim));
    g_landAnim    = 1.0f;
    g_closeLinger = 2;
    g_animTarget  = 0.0f;
    g_animLastT   = Time::steadyNow();
    damageAll();
}

// End the map INSTANTLY — no zoom, no landing glide.
//
// Coming back to the stage is a task switch, and inside the stage a task switch
// is a CUT: it is why the deck's tiles never fly, and why `stage-show` puts the
// window there rather than animating it (Max's standing call). The way IN is a
// movement — the open stays exactly as it is, he likes it — but the way back is
// not a journey, it is arriving.
//
// A cut also has nothing left to mismatch, which is the point after four rounds
// of trying to make a zoom land cleanly on a desktop that changes underneath it:
// no picture to fly into, no landing glide to line up, no hand-off frame where
// the overlay and the desktop can disagree.
//
// `g_closeLinger = 2` still applies, and the caller must have re-photographed the
// zoom tile first: those two frames draw the overlay AT REST, which is fullscreen
// `g_zoomTile`, and they must already show the stage or the cut would flash the
// old workspace on its way out.
static void snapCloseNow() {
    g_anim        = 0.0f;
    g_animTarget  = 0.0f;
    g_landAnim    = 0.0f;
    g_closeLinger = 2;
    g_animLastT   = Time::steadyNow();
    damageAll();
}

// The stage's way back: a SHORT zoom, and nothing else.
//
// The cut above was right in substance — no flicker, no mismatch, no workspace
// step — and wrong in manner: *"too aggressive… too snappy… i click a window, on
// overview, it jumps in front… is good, but too fast"* (Max, 2026-09-17). This is
// that same arrival, given time to be seen.
//
// Motion is only safe here because of what now happens FIRST: `luaStageReady`
// re-photographs the zoom tile, so this flies into a picture of the STAGE. Every
// earlier animated attempt flew out of the bare workspace, which is what made
// them all read the same — the journey itself was the workspace step.
//
// No landing glide (`g_landAnim = 0`): that second act walks a tile's windows
// home from their collage arrangement, and the stage is one window already at
// home, so it would add a third of a second of nothing.
static void beginStageCloseZoom() {
    g_anim        = invEaseInOutCubic(easeOutCubic(g_anim));
    g_landAnim    = 0.0f;
    g_closeLinger = 5; // must outlast one round trip: the real dim lands under these
    g_animTarget  = 0.0f;
    g_zoomSeconds = STAGE_ZOOM_SECONDS;
    g_animLastT   = Time::steadyNow();
    damageAll();
}

// Begin closing — and over Golem's STAGE, hold, then arrive (see `beginStageCloseZoom`).
//
// Max, 2026-09-17: *"i want it to go from overview direct to stage. no workspace
// step."* The map lifts the stage's concealment while it is up, so any zoom home
// flies into a picture of the BARE WORKSPACE and the stage can only arrive at the
// end of it. Three shapes of that were tried and all three were the same to him —
// rebuild behind the animation, hold-then-fly, morph mid-flight — because they
// all kept a journey whose start is the workspace. *"the coming back to stage is
// so bad that i dont care about the overview open."*
//
// So over the stage there is no journey: the map holds, unchanged, while
// waverunner rebuilds the stage (`stage-resume`), and its answer (`stage_ready`,
// ~60ms) CUTS to it. One frame the grid, the next the stage.
//
// Nothing animates here, so nothing can disagree at the hand-off, and nothing
// changes under the pointer before it (the held map keeps the picture it had —
// the re-photograph happens in the same breath as the cut).
//
// Call it with the pick's focus already applied: waverunner stages whatever the
// map left focused.
static void startClose() {
    if (g_stageMode) {
        g_stageClosePending = true;
        g_stageCloseAt      = Time::steadyNow();
        tellStageResume();
        // NOT held. Holding let the map sit at full grid while waverunner worked,
        // and both of the things that then changed the picture were visible as a
        // rebuild: the live timer re-capturing every 150ms, and our own
        // re-photograph popping the tile from six windows to one BEFORE any
        // motion. Flying immediately means the picture only ever changes while
        // it is already moving — and while it moves, the live timer holds off by
        // itself (it never captures mid-animation).
        beginStageCloseZoom();
        return;
    }
    beginCloseZoom();
}

static void closeOverview() {
    if (g_animTarget < 0.5f)
        return;
    // Closed before the zoom ever settled (Escape on the way in): drop the
    // warp we still owe and give the pointer back. Nothing may leave this
    // function with a hidden cursor.
    g_warpPending  = false;
    g_shapeWantSet = false; // no dwelled shape may land after the close
    g_warpWin.reset();
    endWarpHide();
    endRealResize();   // sizes applied live are final; just settle
    resetEdgeCursor(); // the desktop must not inherit a resize pointer
    restoreOriginal(); // never leave a real drag dangling; a commit is undone
    g_dragWin.reset();
    g_dragMoved  = false;
    // Escape / gesture / toggle: nothing was picked, so the stage comes back on
    // whatever is focused — which is where it already was.
    startClose();
}

// Whether Golem's STAGE owns the screen — waverunner sets it over
// `hl.plugin.waveview.set_stage(...)` as it enters and leaves.
//
// It no longer bars the overview: Max asked for the map to be reachable from
// inside the stage (2026-09-12), so `toggle()` is open to every route again and
// this flag only says who owns the TRACKPAD — the deck's border takes the
// horizontal swipes while the stage is up, and the workspace swipe must not run
// underneath it. (Defined with the spread globals near the top — the spread
// refuses to open over the stage.)

static void toggle() {
    // The key (and the second swipe) names its destination: the overview.
    // A spread on screen folds instantly underneath it — the overview's
    // open-zoom starts from the current workspace anyway, so the hand-off
    // reads as one continued zoom-out. The daemon is NOT told off here: the
    // overview's own overview-on keeps the conceal seamless.
    if (g_spreadActive)
        snapCloseSpread(false);
    const bool opening = g_animTarget < 0.5f; // currently closed/closing -> open
    // The Super+R tour: pressed while open, and the other page holds
    // windows we haven't visited → flip there instead of closing. A third
    // press (or a second when the other page is empty) closes.
    if (!opening && !g_tourDone) {
        const int other = 1 - g_page;
        if (pageHasWindows(other)) {
            g_tourDone = true;
            flipToPage(other);
            return;
        }
    }
    if (!opening) {
        closeOverview(); // one close path: drag/resize/cursor cleanup included
        return;
    }
    // Onto the opening curve from exactly the current progress (a reopen
    // mid-close turns around in place; from fully closed this is a no-op).
    g_anim       = 1.0f - std::cbrt(std::clamp(1.0f - (float)easeInOutCubic(g_anim), 0.0f, 1.0f));
    g_landAnim    = 0.0f; // an open never lands; a stale glide must not hold the close condition
    g_zoomSeconds = OPEN_ZOOM_SECONDS; // the open runs on its own, quicker clock
    g_openBlend   = 0.0f;                // and the reveal starts from bunched, every time
    g_stageDimPaint = false;
    g_stageDimAsk   = false;
    g_closeLinger = 0;
    g_freePending = false; // reopened before the deferred free: the captures are live again
    g_animTarget  = 1.0f;
    notifyWaverunner(true);
    g_animLastT  = Time::steadyNow();
    {
        g_active         = true;
        const auto m     = g_pCompositor->getMonitorFromCursor();
        const int  at    = waveview_tile_for_workspace(m ? m->activeWorkspaceID() : -1);
        g_zoomTile       = at >= 0 ? at : 0;
        // Open on the page holding the active workspace, already settled
        // (no flip animation on open — the zoom pivots on-screen).
        g_page       = g_zoomTile / 9;
        g_scroll     = g_scrollTarget = m ? g_page * pageStep(m) : 0.0;
        g_scrollFrom = g_scroll;
        g_scrollProg = 1.0f; // open lands settled — no flip animation
        g_tourDone   = false;
        // Remember where we were called from; the warp itself happens on the
        // first settled frame, once that window has a drawn box.
        // "The window I'm calling it from" is the one under the POINTER, not
        // the one holding keyboard focus. This desktop runs follow_mouse = 2,
        // where focus moves only on CLICK — so the focused window is merely
        // the last one clicked, often a different window, sometimes a
        // different workspace, and the warp dutifully went there. Every
        // landing in the trace was mechanically perfect and humanly wrong
        // (Max, 2026-09-03: "well.. its not working"). Keyboard focus is only
        // the fallback for a pointer over nothing (the void, a layer).
        const auto underPtr = g_pCompositor->vectorToWindowUnified(
            g_pInputManager->getMouseCoordsInternal(),
            Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
        g_warpWin        = underPtr ? underPtr : Desktop::focusState()->window();
        g_warpPending    = g_warpWin.lock() != nullptr;
        g_warpFromCursor = g_pInputManager->getMouseCoordsInternal();
        // The one silent way to get no warp at all is opening with nothing
        // under the pointer AND nothing focused — name it in the log so "it
        // didn't move" is always attributable.
        trace("open: warp %s via %s (swipeLive=%d)",
              g_warpPending ? "armed" : "SKIPPED - no window",
              underPtr ? "pointer" : "focus fallback", (int)g_swipeLive);
        captureWorkspaces(m); // snapshot on open, outside the render pass
        // The pointer must exist over the overview: the capture's workspace
        // juggling can leave the cursor surfaceless (shouldRenderCursor
        // needs one), and the motion-cancel delays the natural re-arm —
        // "overview opens sometimes with no pointer". Unhide and pin the
        // default arrow for the overview's lifetime.
        g_pHyprRenderer->setCursorHidden(false);
        // Through the one owner, forced: the overview is taking the pointer
        // over, and whatever shape the desktop had on it (a resize arrow from
        // a window edge, a client's caret) is not ours and must not survive
        // into the grid.
        setOverviewCursor(nullptr, /*force=*/true);
        // …and then hide it again for exactly as long as it is being moved, so
        // the relocation is never seen. Ordered after the unhide above on
        // purpose: that call is what guarantees there IS a cursor to show when
        // `endWarpHide` runs.
        if (g_warpPending) {
            g_warpHidCursor = true;
            g_pHyprRenderer->setCursorHidden(true);
        }
        if (g_liveTimer)
            g_liveTimer->updateTimeout(REFRESH_MS);
    }
    damageAll();
}

// A trackpad swipe begins: remember the finger count and reset the accumulator.
// While the overview is open the whole gesture stream is consumed so the
// compositor's workspace-swipe never engages underneath (a sideways 3/4-finger
// swipe would drag the real workspaces behind the overview).
static void onSwipeBegin(IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) {
    g_swipeFingers = e.fingers;
    g_swipeAcc     = Vector2D(0.0, 0.0);
    g_swipeFired   = false;
    g_swipeLive    = true; // motion until the fingers leave belongs to the gesture
    // STAGE mode eats every swipe, in BOTH axes. Vertical would open the
    // overview from here; horizontal is passed through untouched below and the
    // compositor's own `hl.gesture` turns it into a workspace change — and that
    // gesture cannot be unregistered from Lua at all (no unregister exists,
    // re-registering is refused, `action = "none"` is rejected). Consuming it
    // here is the only place either can actually be stopped.
    // The stage's own gesture: 3 or 4 fingers, the same counts the config gives
    // the workspace swipe, so the hand does what it already knew — it just
    // travels the deck instead of the workspaces while the stage is up.
    g_stageSwipe     = g_stageMode && (e.fingers == 3 || e.fingers == 4);
    g_stageSwipeDx   = 0.0;
    g_stageSwipeSent = 0.0;
    if (g_active || g_stageMode || g_spreadActive)
        info.cancelled = true;
}

// Whether a swipe has travelled far enough, vertically enough, to mean it. Every
// vertical gesture asks the same question, so they ask it here.
static bool decisiveVertical() {
    return std::abs(g_swipeAcc.y) >= SWIPE_TRIGGER && std::abs(g_swipeAcc.y) > std::abs(g_swipeAcc.x);
}

// Sideways travel that hands the whole gesture to whatever owns horizontal —
// the compositor's workspace swipe out on the desktop, the deck's border while
// staged.
//
// Well under SWIPE_TRIGGER, because the compositor starts sliding the
// workspaces on the first few millimetres: by the time this much has gone by
// the user is watching a slide, and lifting the fingers afterwards is the hand
// coming off the pad, not a new instruction. Without it a swipe that went
// sideways and then up opened the stage *mid-slide*, halfway between two
// workspaces (Max, 2026-09-12).
static constexpr double SWIPE_CLAIM_X = 40.0;

// Whether this gesture has already been claimed by its horizontal owner.
static bool claimedSideways() {
    return std::abs(g_swipeAcc.x) >= SWIPE_CLAIM_X && std::abs(g_swipeAcc.x) > std::abs(g_swipeAcc.y);
}

// Accumulate the swipe; act once per gesture on a decisive vertical move.
// libinput reports fingers-up as negative dy.
//
// It is the FINGER COUNT that says which room you are asking for, and UP that
// asks for it (Max, 2026-09-12 — the direction carried both at first, and the
// count reads better: two places, two hands, one motion):
//
//   3 UP    — the overview. Same ladder as Super+R: open, tour the other
//             inhabited page, close.
//   3 DOWN  — out of the overview (its long-standing Escape). Nothing when it
//             is not up.
//   4 UP    — Golem's STAGE. Also the way back out of it, and it works from
//             inside the overview too — the daemon shuts that on its way in.
//   4 DOWN  — out of the stage. Nothing when it is not up.
//
// So each room is opened by a count and left by the same count, with down as
// the extra "out" for whichever one you are in.
//
// A gesture that went SIDEWAYS first belongs to whoever owns that axis and is
// latched away from all of this — see [`claimedSideways`].
static void onSwipeUpdate(IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
    if (g_active || g_spreadActive)
        info.cancelled = true; // overview/spread owns the trackpad while open
    // The stage owns the trackpad — but only while it is the thing on screen.
    // With the overview open OVER it, the map owns the gestures and they run the
    // ordinary path below (which is where its own ladder and Escape live).
    if (g_stageMode && !g_active) {
        info.cancelled = true; // no workspace swipe underneath the stage
        g_swipeAcc += e.delta;
        // A gesture is one axis or the other, and the first one to be decisive
        // takes it. Sideways first means the deck has it: the border is already
        // walking, and lifting the fingers away at the end must not also throw
        // you out of the mode.
        if (!g_swipeFired && claimedSideways())
            g_swipeFired = true; // the deck's, for the rest of this gesture
        // Vertical first wins outright and takes the gesture OFF the deck, so a
        // swipe out of the mode cannot leave a half-finished tile selection
        // behind it.
        if (!g_swipeFired && decisiveVertical()) {
            g_swipeFired = true;
            g_stageSwipe = false;
            // FOUR fingers leave, either direction: the count that opened the
            // stage is the count that closes it, and down is the extra way out.
            // THREE still means the overview, which now opens over the stage
            // rather than being refused — the map is how you find the task you
            // want to put on it. Down does nothing here: the overview is not up
            // (this branch only runs when it isn't), so there is nothing to
            // escape from.
            if (g_swipeFingers == 4)
                sendWaverunner("stage-toggle\n");
            else if (g_swipeAcc.y < 0.0)
                toggle(); // 3 up: the overview, over the stage
            return;
        }
        if (g_stageSwipe) {
            g_stageSwipeDx += e.delta.x;
            // The TOTAL is sent, never the step: messages go out on their own
            // threads, so one arriving late (or not at all) must not be able to
            // leave the border out of step with the fingers.
            if (std::abs(g_stageSwipeDx - g_stageSwipeSent) >= STAGE_SWIPE_GRAIN) {
                g_stageSwipeSent = g_stageSwipeDx;
                sendWaverunner(std::format("stage-swipe {:.1f}\n", g_stageSwipeDx));
            }
        }
        return;
    }
    if ((g_swipeFingers != 3 && g_swipeFingers != 4) || g_swipeFired)
        return;
    g_swipeAcc += e.delta;
    // The workspace swipe has it: the compositor is already sliding, and this
    // gesture is theirs to finish. Latched — NOT consumed, since they still need
    // the events — so a lift at the end cannot open the stage halfway between
    // two workspaces.
    if (claimedSideways()) {
        g_swipeFired = true;
        return;
    }
    if (!decisiveVertical())
        return; // not yet decisive, or dominantly horizontal

    const bool up    = g_swipeAcc.y < 0.0;
    bool       acted = true;
    if (g_swipeFingers == 3) {
        if (up) {
            // The reveal ladder, bottom rung first: on a workspace hiding
            // windows the first UP spreads THEM (level 1); the next UP —
            // toggle() folds the spread on its way — escalates to the
            // overview (level 2). Nothing hidden here → straight to the
            // overview, exactly as before the spread existed.
            if (!g_active && !g_spreadActive && g_animTarget < 0.5f) {
                const auto mm = g_pCompositor->getMonitorFromCursor();
                if (wsNeedsSpread(mm)) {
                    openSpread(mm);
                } else
                    toggle();
            } else
                toggle(); // the ladder above the rung (spread → overview → tour → close)
        } else if (g_spreadActive && !g_active)
            closeSpread(); // down in the spread: home, untouched
        else if (g_animTarget >= 0.5f)
            closeOverview(); // down, with the overview showing: Escape out of it
        else
            acted = false; // down on a bare desktop: nothing to leave
    } else if (up) {
        // Four up is the stage wherever you are. From inside the overview it
        // still means the stage — the daemon closes that on its way in.
        sendWaverunner("stage-toggle\n");
    } else {
        acted = false; // four down with no stage up: nothing to leave
    }
    // Latched either way: one gesture makes one decision, so a hand that drifts
    // back the other way mid-swipe cannot fire the opposite one behind it.
    g_swipeFired = true;
    if (acted)
        info.cancelled = true; // consume so no built-in gesture also reacts
}

static void onSwipeEnd(IPointer::SSwipeEndEvent, Event::SCallbackInfo& info) {
    if (g_active || g_stageMode || g_spreadActive)
        info.cancelled = true;
    // The fingers left: waverunner stages whatever the border reached. The final
    // travel rides along, so this message alone decides where that is.
    if (g_stageSwipe) {
        sendWaverunner(std::format("stage-swipe-end {:.1f}\n", g_stageSwipeDx));
        g_stageSwipe = false;
    }
    g_swipeFingers = 0;
    g_swipeFired   = false;
    g_swipeLive    = false;
    g_swipeEndAt   = Time::steadyNow(); // the settle grace runs from here
}

// Jump to workspace `wsId` (1..9) and close the overview by zooming into that
// workspace's tile. Switching workspace happens under the still-covering
// overview; the close animation then flies into the chosen tile, so releasing
// the overview reveals the workspace we just switched to — seamless.
static void jumpTo(int wsId, bool startTheClose) {
    if (!g_active)
        return;
    const int t = waveview_tile_for_workspace(wsId);
    if (t < 0)
        return;

    g_zoomTile = t; // close animation pivots on (zooms into) the chosen tile

    // The workspace switch happens FIRST now, still under the covering map, so
    // that the close we start below is decided against the workspace we are
    // actually landing on — over the stage, `startClose` hands this moment to
    // waverunner, which stages whatever is focused by then.
    if (g_pKeybindManager) {
        const auto it = g_pKeybindManager->m_dispatchers.find("workspace");
        if (it != g_pKeybindManager->m_dispatchers.end())
            it->second(std::to_string(wsId));
    }
    // `jumpToWindow` passes false and closes itself, once it has focused the
    // exact window that was clicked — from here the focus is still the old one,
    // and over the stage that would stage the window we are leaving.
    if (startTheClose)
        startClose();
    damageAll();
}

// Clicking a window: jump to its workspace (same seamless zoom-into-tile close as
// digit-jump) and focus that specific window, so releasing the overview lands on
// exactly the window that was clicked — not just its workspace's last focus.
static void jumpToWindow(PHLWINDOW w) {
    if (!w)
        return;
    jumpTo(static_cast<int>(w->workspaceID()), false); // switch workspace, hold the close
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
    // Only NOW: the stage rebuilds itself on whatever the map left focused, and
    // that is only this window once the focus above has landed.
    startClose();
}

// While the overview is open, a digit 1..9 jumps to that workspace ON THE
// CURRENT PAGE (page 2 → 10..18), Escape closes, Q closes the hovered
// window — and EVERY other key is swallowed too: keyboard focus is still on
// the last window underneath, and typing used to leak straight into it
// (Max: "focus is on the last window still"). While the overview owns the
// screen it owns the keyboard. Closed: we're transparent.
static void onKey(IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
    // Track Super ALWAYS (even while closed — the press that precedes an
    // opening Super+R happens before we're active).
    if (e.keycode == EVDEV_LMETA || e.keycode == EVDEV_RMETA) {
        g_superHeld = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        return;
    }
    // Typing into the focused window is USE (desktop-side only; keys while
    // the overview is open are ours). Super-chords are binds, not use — the
    // Super+Tab that drives the focus cycle must never commit it.
    if (!g_active && !g_spreadActive && !g_superHeld && e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
        noteInteraction(false);
    // The spread owns the keyboard exactly as the overview does — focus is
    // still on a window underneath, and typing must not leak into it.
    if (g_spreadActive && !g_active) {
        // Super-chords are binds and pass through — Super+G's toggle() is the
        // keyboard's way up the ladder (it folds the spread on its way).
        // Digits are ours even with Super held, same as in the overview.
        if (g_superHeld) {
            if (e.keycode >= EVDEV_1 && e.keycode <= EVDEV_9) {
                info.cancelled = true;
                if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                    const int ws = (int)(e.keycode - EVDEV_1 + 1);
                    snapCloseSpread();
                    if (g_pKeybindManager) {
                        const auto it = g_pKeybindManager->m_dispatchers.find("workspace");
                        if (it != g_pKeybindManager->m_dispatchers.end())
                            it->second(std::to_string(ws));
                    }
                }
            }
            return;
        }
        info.cancelled = true;
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        if (e.keycode == EVDEV_ESC) {
            closeSpread(); // Escape is the back gesture's keyboard twin
        } else if (e.keycode >= EVDEV_1 && e.keycode <= EVDEV_9) {
            // Digits mean workspaces everywhere in waveview: leave for N.
            const int ws = (int)(e.keycode - EVDEV_1 + 1);
            snapCloseSpread();
            if (g_pKeybindManager) {
                const auto it = g_pKeybindManager->m_dispatchers.find("workspace");
                if (it != g_pKeybindManager->m_dispatchers.end())
                    it->second(std::to_string(ws));
            }
        } else if (e.keycode == EVDEV_Q) {
            if (const auto w = g_spreadHover.lock())
                g_pXWaylandManager->sendCloseWindow(w); // the refresh re-spreads without it
        }
        return;
    }
    if (!g_active || g_animTarget < 0.5f) // only intercept while open (not mid-close)
        return;
    // Super held: digits are OURS — page-relative jump, swallowed so the
    // compositor's absolute workspace bind can't fight it. This makes the
    // one-hand chord work: Super+R, Super+R (tour), Super+3 → workspace 12.
    // Every other bind combo passes through (Super+R's toggle above all).
    if (g_superHeld) {
        if (e.keycode >= EVDEV_1 && e.keycode <= EVDEV_9) {
            if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
                jumpTo(g_page * 9 + static_cast<int>(e.keycode - EVDEV_1 + 1));
            info.cancelled = true;
        }
        return;
    }
    info.cancelled = true; // plain typing never reaches the desktop while open

    if (e.keycode >= EVDEV_1 && e.keycode <= EVDEV_9) {
        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            jumpTo(g_page * 9 + static_cast<int>(e.keycode - EVDEV_1 + 1));
        return;
    }

    if (e.keycode == EVDEV_Q) {
        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            if (const auto w = g_hoverWin.lock())
                g_pXWaylandManager->sendCloseWindow(w); // live timer re-captures, so it vanishes from the grid
        return;
    }

    if (e.keycode == EVDEV_ESC && e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
        closeOverview(); // Escape means ESCAPE — never the tour
}

static int luaToggle(lua_State*) {
    toggle();
    return 0;
}

// ---- Deck thumbnails for Golem's STAGE mode --------------------------------
//
// waverunner draws the stage's deck in its own process, so it cannot reach
// these textures — this is the one place a capture leaves the compositor.
//
// It exists because the client-side alternatives are all worse for the job.
// `grim` can only photograph what is on screen, so a tile stayed blank until
// its task had been staged at least once, and every capture cost a subprocess
// plus a PNG encode and decode. The capture protocols
// (`hyprland_toplevel_export`, `ext_image_copy_capture`) can reach an unmapped
// window but hand back a full-resolution frame, ~95% of which is thrown away to
// fill a 256px tile. Here the GPU does the downscale as part of the blit and
// only the tile comes back — a couple of hundred KB instead of tens of MB.
//
// Called twice per visit (arrival and departure), never on a timer: the deck is
// a working surface, not a live view. That is also what makes the whole thing
// affordable — the overview, which *is* live, had to be throttled to
// REFRESH_MS with dirty-tile masking to stay smooth.
static PHLWINDOW windowByAddr(const std::string& addrStr) {
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        if (std::format("0x{:x}", (uintptr_t)w.get()) == addrStr)
            return w;
    }
    return {};
}

/// Blit one already-cropped window texture down into `out` and read it back.
///
/// `out` is reused across a batch: it is the same size for every tile, so
/// allocating it per window would be pure churn — the same churn that, in the
/// overview's own capture loop, "churned the driver into a progressive mid-drag
/// slowdown".
static bool blitAndRead(PHLMONITOR m, SP<Render::IFramebuffer> src, SP<Render::IFramebuffer> out, int size,
                        double tileAspect, std::vector<uint8_t>& px) {
    const auto srcTex = src ? src->getTexture() : nullptr;
    if (!srcTex)
        return false;

    // Mipmap the source before the blit. This is a ~12x reduction (a
    // monitor-resolution crop into a 256px tile) and plain GL_LINEAR
    // minification samples only 2x2 texels, which turns text into aliased mush —
    // that was the whole of the "quality sucks" problem, not the resolution.
    // Restored afterwards: the texture belongs to the overview, which draws it
    // at a gentler scale and wants its own filtering back.
    const GLenum prevMin = srcTex->minFilter;
    bool         mipped  = false;
    glBindTexture(GL_TEXTURE_2D, srcTex->m_texID);
    while (glGetError() != GL_NO_ERROR) {}
    glGenerateMipmap(GL_TEXTURE_2D);
    if (glGetError() == GL_NO_ERROR) {
        mipped            = true;
        srcTex->minFilter = GL_LINEAR_MIPMAP_LINEAR;
        srcTex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // The atlas layer is square and the tile stretches it back out to the tile's
    // aspect. Filling the whole square therefore makes EVERY window come out
    // looking like the tile's shape, whatever shape it actually is: a staged
    // window (1.84) came out 15% too wide, and a tall half-screen window (0.82)
    // came out nearly 2:1 wrong. That is the "broken thumbnail".
    //
    // So the window is drawn into a sub-rect of the square whose aspect is
    // `source / tile` — after the tile's stretch that lands back at the source's
    // true aspect. What is left over stays transparent and the tile's own dark
    // body shows through, which is the right backdrop anyway.
    const double srcA  = srcTex->m_size.y > 0 ? (double)srcTex->m_size.x / (double)srcTex->m_size.y : 1.0;
    const double want  = tileAspect > 0.0 ? srcA / tileAspect : 1.0;
    double       destW = size, destH = size;
    if (want >= 1.0)
        destH = (double)size / want;
    else
        destW = (double)size * want;
    const CBox dest{(size - destW) / 2.0, (size - destH) / 2.0, destW, destH};

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, out);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    Render::GL::CHyprOpenGLImpl::STextureRenderData td;
    Render::GL::g_pHyprOpenGL->renderTexture(srcTex, dest, td);
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    // endRender first: the render pass is not submitted until it runs, so
    // anything read before it is the cleared framebuffer, not the blit.
    g_pHyprRenderer->endRender();

    if (mipped) {
        srcTex->minFilter = prevMin;
        glBindTexture(GL_TEXTURE_2D, srcTex->m_texID);
        srcTex->setTexParameter(GL_TEXTURE_MIN_FILTER, prevMin);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Then read through an FBO of our own, wrapping the result texture. Going
    // via the framebuffer's own binding after endRender fails with
    // GL_INVALID_FRAMEBUFFER_OPERATION — by then Hyprland has torn its binding
    // down — and this owes nothing to that lifecycle.
    px.assign((size_t)size * size * 4, 0);
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    const auto tex = out->getTexture();
    GLuint     fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex ? tex->m_texID : 0, 0);
    while (glGetError() != GL_NO_ERROR) {} // drop errors left by earlier passes, so the one below is ours
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE)
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const GLenum glErr = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    if (status != GL_FRAMEBUFFER_COMPLETE || glErr != GL_NO_ERROR) {
        trace("deck-capture: readback failed status=0x%x glErr=0x%x", status, glErr);
        return false;
    }
    return true;
}

/// Written to a temp and renamed, so a reader can never open a half-written
/// file — it looks for the path rather than being signalled.
static bool writeRaw(const std::string& path, const std::vector<uint8_t>& px) {
    const std::string tmp = path + ".part";
    FILE*             f   = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(px.data(), 1, px.size(), f) == px.size();
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

/// Capture a whole set of windows in **one** workspace pass.
///
/// This is the point of the batch form. `captureWorkspaces` renders every tile
/// in its mask together and `captureWindows` then crops all of them, so asking
/// for eight tiles costs about what asking for one does — whereas eight separate
/// `capture_window` calls pay the monitor-resolution workspace render eight
/// times over. Filling the deck when the mode opens is only affordable this way.
///
/// Each window is written to `<dir>/<addr>.rgba`. Returns how many landed.
static int captureDeckToDir(const std::string& addrsCsv, int size, double tileAspect, const std::string& dir) {
    if (size <= 0 || size > 1024)
        return 0;

    struct Want {
        std::string addr;
        PHLWINDOW   win;
    };
    std::vector<Want> wants;
    uint32_t          mask = 0;
    PHLMONITOR        m;
    for (size_t at = 0; at <= addrsCsv.size();) {
        const size_t end  = std::min(addrsCsv.find(',', at), addrsCsv.size());
        const auto   addr = addrsCsv.substr(at, end - at);
        at                = end + 1;
        if (addr.empty())
            continue;
        const auto w = windowByAddr(addr);
        if (!w)
            continue;
        const auto wm = g_pCompositor->getMonitorFromID(w->monitorID());
        if (!wm)
            continue;
        // One monitor per pass: the workspace snapshots are rendered for a
        // specific output and a tile is only valid on that one.
        if (!m)
            m = wm;
        else if (m != wm)
            continue;
        wants.push_back({addr, w});
    }
    if (wants.empty() || !m)
        return 0;

    // Context first: allocating a framebuffer is a GL call, and doing it before
    // the context is current leaves it unallocated — which then shows up much
    // later as GL_INVALID_FRAMEBUFFER_OPERATION on the read, not at the alloc.
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    auto out = g_pHyprRenderer->createFB("waveview-deck");
    if (!out->alloc(size, size, DRM_FORMAT_ABGR8888) || !out->isAllocated()) {
        trace("deck-capture: alloc %dx%d failed", size, size);
        return 0;
    }
    // One scratch framebuffer for the whole batch, resized as windows differ.
    auto scratch = g_pHyprRenderer->createFB("waveview-deck-src");

    std::vector<uint8_t> px;
    int                  done = 0;
    for (const auto& want : wants) {
        // Render the WINDOW, not a slice of its workspace.
        //
        // The overview's per-window crops are cut out of a whole-workspace
        // snapshot, which cannot work here: while the stage is up a task's
        // siblings are covered by the maximized staged window, so their pixels
        // in that snapshot are the *staged* window's. `captureWindows` knows
        // this and deliberately keeps the last clean crop for an overlapped
        // window — the overview then cover-crops it when drawing, but a
        // thumbnail blitted straight from it comes out at the wrong scale and
        // offset. That was the "uncentered and too big" tile: a stale crop of a
        // window that had since changed size.
        //
        // Rendering the window on its own has no such problem, needs no
        // workspace snapshot at all (so no monitor-resolution framebuffer per
        // workspace), and works for a window on any workspace — including one
        // the grid does not map.
        const auto box = want.win->getWindowMainSurfaceBox();
        const int  pw  = (int)std::round(box.w * m->m_scale);
        const int  ph  = (int)std::round(box.h * m->m_scale);
        if (pw < 1 || ph < 1)
            continue;
        if (scratch->m_size != Vector2D(pw, ph)) {
            scratch->release();
            if (!scratch->alloc(pw, ph, DRM_FORMAT_ABGR8888))
                continue;
        }

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true; // a thumbnail must not drive the client's frame clock
        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, scratch);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        // `ignorePosition` puts it at the origin rather than wherever it lives,
        // and `standalone` leaves off the border and shadow — a tile draws its
        // own frame.
        g_pHyprRenderer->renderWindow(want.win, m, Time::steadyNow(), false, Render::RENDER_PASS_MAIN, true, true);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
        g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

        if (!blitAndRead(m, scratch, out, size, tileAspect, px))
            continue;
        if (writeRaw(dir + "/" + want.addr + ".rgba", px))
            ++done;
    }
    scratch->release();
    out->release();
    return done;
}

// ---- Minimize-to-dock machinery (see the MinFly globals up top) ---------------

// Where a minimized card lands: the dock's right side, macOS's spot for
// minimized windows. Draw-space box, sized to the window's aspect; falls
// back to a bottom centre-right guess when the dock's layer isn't up.
static CBox dockTargetBox(PHLMONITOR m, double aspect) {
    CBox dock{};
    bool found = false;
    for (int lvl = 0; lvl < 4 && !found; ++lvl)
        for (auto& ref : m->m_layerSurfaceLayers[lvl])
            if (const auto ls = ref.lock(); ls && ls->m_namespace == "waverunner-deck") {
                dock  = ls->m_geometry;
                found = true;
                break;
            }
    if (!found)
        dock = CBox{m->m_position.x + m->m_size.x * 0.25, m->m_position.y + m->m_size.y - 96.0, m->m_size.x * 0.5, 84.0};
    if (aspect <= 0.05 || aspect > 20.0)
        aspect = 1.6;
    double tw = 96.0, th = tw / aspect;
    if (th > dock.h - 12.0) { // a tall window's card still fits inside the dock band
        th = std::max(24.0, dock.h - 12.0);
        tw = th * aspect;
    }
    const double cx = dock.x + dock.w * 0.78, cy = dock.y + dock.h * 0.5;
    return logicalToDraw(m, Rect{cx - tw / 2.0, cy - th / 2.0, tw, th});
}

// The window's decorated card (bar + border baked, the spread's look) as its
// own texture, plus its draw-space box. Runs OUTSIDE any render pass (button
// press / Lua call context), same rules as every capture here.
static SP<Render::IFramebuffer> captureCardFly(PHLWINDOW w, PHLMONITOR m, CBox& cardDraw) {
    const CBox wb = w->getWindowMainSurfaceBox();
    if (wb.w <= 1.0 || wb.h <= 1.0)
        return nullptr;
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    const CBox monbox{0.0, 0.0, m->m_pixelSize.x, m->m_pixelSize.y};
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    g_capturing                              = true;
    m->m_solitaryClient.reset();
    if (!g_spreadSrcFB)
        g_spreadSrcFB = g_pHyprRenderer->createFB("waveview-spread-src");
    if (g_spreadSrcFB->m_size != monbox.size()) {
        g_spreadSrcFB->release();
        g_spreadSrcFB->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888);
    }

    const bool covered = w->effectiveAlpha() <= 0.001F; // covered by a fullscreen sibling → render bare, else it is skipped
    CRegion    fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, g_spreadSrcFB);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    g_pHyprRenderer->renderWindow(w, m, Time::steadyNow(), !covered, Render::RENDER_PASS_ALL, false, covered);
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    SP<Render::IFramebuffer> fb;
    double                   top = 0.0, side = 0.0;
    cardExtents(w, top, side);
    const CBox   cb{wb.x - side, wb.y - top, wb.w + 2.0 * side, wb.h + top + side};
    const double u0 = std::clamp((cb.x - m->m_position.x) / m->m_size.x, 0.0, 1.0);
    const double v0 = std::clamp((cb.y - m->m_position.y) / m->m_size.y, 0.0, 1.0);
    const double u1 = std::clamp((cb.x + cb.w - m->m_position.x) / m->m_size.x, 0.0, 1.0);
    const double v1 = std::clamp((cb.y + cb.h - m->m_position.y) / m->m_size.y, 0.0, 1.0);
    if (const auto srcTex = g_spreadSrcFB->getTexture(); srcTex && u1 - u0 > 0.0 && v1 - v0 > 0.0) {
        const int fbw = std::max(1, (int)std::lround(cb.w * m->m_scale));
        const int fbh = std::max(1, (int)std::lround(cb.h * m->m_scale));
        fb            = g_pHyprRenderer->createFB("waveview-minfly");
        if (fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888)) {
            g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, fb);
            glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            Render::GL::CHyprOpenGLImpl::STextureRenderData td;
            td.allowCustomUV               = true;
            td.primarySurfaceUVTopLeft     = Vector2D(u0, v0);
            td.primarySurfaceUVBottomRight = Vector2D(u1, v1);
            Render::GL::g_pHyprOpenGL->renderTexture(srcTex, CBox{0.0, 0.0, (double)fbw, (double)fbh}, td);
            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
        } else {
            fb->release();
            fb.reset();
        }
    }
    g_capturing                              = false;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
    cardDraw = logicalToDraw(m, Rect{cb.x, cb.y, cb.w, cb.h});
    return fb;
}

static void runDispatcher(const std::string& name, const std::string& arg) {
    if (!g_pKeybindManager)
        return;
    const auto it = g_pKeybindManager->m_dispatchers.find(name);
    if (it != g_pKeybindManager->m_dispatchers.end())
        it->second(arg);
}

// The orange button's action. Card capture + thumbnail first (the window
// must still be on screen for both), then the park and the fly.
void golemMinimize(PHLWINDOW w) {
    if (!w || g_active || g_spreadActive)
        return;
    const auto m = g_pCompositor->getMonitorFromID(w->monitorID());
    if (!m)
        return;
    const std::string addr = windowAddr(w);
    // NEVER minimize a window that is already minimized, or any window on a
    // special workspace. Max, 2026-09-16: the orange button on a Chrome that
    // was already parked (special:minimized, id -98) fired minimize THREE
    // times in a row — the special ws was showing, so the window still looked
    // present, he re-clicked, and each re-minimize recorded a bogus special
    // return ws and churned focus until nothing could be focused. Both guards
    // make a re-minimize a no-op.
    if (w->workspaceID() < 0)
        return;
    for (const auto& r : g_minimized)
        if (r.addr == addr)
            return;
    const std::string dir  = minThumbDir();
    mkdir(dir.c_str(), 0700);
    // Capture the dock thumbnail at the window's OWN aspect: blitAndRead
    // letterboxes by source/tileAspect, so passing the window's aspect makes
    // it FILL the square (want = 1) at full resolution. The daemon then
    // un-stretches that square to the same aspect — the tile carries the
    // window's SHAPE, macOS-style (Max, 2026-09-16), no padding, no squish.
    const CBox   wb     = w->getWindowMainSurfaceBox(); // content box (captureDeckToDir renders standalone, no deco)
    const double aspect = wb.h > 0.0 ? wb.w / wb.h : 1.6;
    captureDeckToDir(addr, MIN_THUMB_SIZE, aspect, dir);

    CBox       from{};
    const auto fb = captureCardFly(w, m, from);
    const CBox to = dockTargetBox(m, aspect);

    const int64_t     ws  = w->workspaceID(); // the return ws (recorded pre-move)
    const std::string cls = w->fetchClass();

    // PARK FIRST, then VERIFY — the dock tile is only committed once the
    // window has actually left the workspace. `movetoworkspacesilent`
    // occasionally does not take (Max, 2026-09-16: "the windows were on the
    // WS and on the dock at the same time"); if the window is still on a
    // normal ws afterwards, abort cleanly — release the card, drop the
    // thumbnail, send NOTHING to the daemon — so the button is a harmless
    // no-op the user can retry, never a tile for a window that's still there.
    runDispatcher("movetoworkspacesilent", "special:minimized,address:" + addr);
    if (w->workspaceID() >= 0) {
        trace("minimize ABORT: %s still on ws %d after move", addr.c_str(), (int)w->workspaceID());
        if (fb)
            fb->release();
        unlink((dir + "/" + addr + ".rgba").c_str());
        return;
    }
    // The park must be INVISIBLE: if moving the window left special:minimized
    // showing on the monitor (it overlays with the window and its titlebar,
    // so the app "is there but isn't"), hide it. Only our own special ws,
    // never a scratchpad the user opened.
    if (const auto sw = m->m_activeSpecialWorkspace; sw && sw->m_name == "special:minimized")
        m->setSpecialWorkspace(nullptr);

    // The app's class rides along so the daemon can badge the tile with the
    // small app icon — resolved by class, like running apps match .desktop.
    const std::string land =
        std::format("min-add {} {} {:.4f} {} {}/{}.rgba {}\n", addr, ws, aspect, cls.empty() ? "?" : cls, dir, addr, w->m_title);
    if (fb) {
        g_minFlyLastT = Time::steadyNow();
        g_minFlies.push_back(MinFly{fb, from, to, 0.0f, m, land});
    } else
        sendWaverunner(land); // no card, no fly — the entry just appears
    std::erase_if(g_minimized, [&](const MinRec& r) { return r.addr == addr; });
    g_minimized.push_back(MinRec{addr, ws, w});
    trace("minimize %s ws=%d fly=%d", addr.c_str(), (int)ws, (int)(bool)fb);
    damageAll();
}

// `hl.plugin.waveview.minimize("0x…")` — the button's action from a script
// (the daemon, a bind, or a debug shell). Empty/missing address = the
// focused window.
static int luaMinimize(lua_State* L) {
    const char* a = lua_tostring(L, 1);
    PHLWINDOW   w = a && *a ? windowByAddr(a) : Desktop::focusState()->window();
    golemMinimize(w);
    return 0;
}

// `hl.plugin.waveview.restore_min("0x…")` — the dock thumbnail's click. The
// window returns to its workspace, the screen follows, the card flies back
// out of the dock onto it.
static int luaRestoreMin(lua_State* L) {
    const char* a = lua_tostring(L, 1);
    if (!a || !*a)
        return 0;
    const std::string addr = a;
    int64_t           ws   = -1;
    PHLWINDOW         w;
    for (const auto& r : g_minimized)
        if (r.addr == addr) {
            ws = r.ws;
            w  = r.win.lock();
            break;
        }
    if (!w)
        w = windowByAddr(addr); // plugin reloaded since the minimize: the record is gone, the window isn't
    std::erase_if(g_minimized, [&](const MinRec& r) { return r.addr == addr; });
    const std::string thumb = minThumbDir() + "/" + addr + ".rgba";
    if (!w) { // the window died while minimized and the destroy path missed it
        sendWaverunner("min-del " + addr + "\n");
        unlink(thumb.c_str());
        return 0;
    }
    // The window comes to WHERE YOU ARE, not back to where it was minimized
    // (Max, 2026-09-16: "the windows have to come back on the WS im on"). The
    // dock is a per-monitor thing; a click means "give me this window here",
    // so the target is the CURRENT monitor's active workspace, and there is
    // no workspace switch — you don't travel to the window, it travels to
    // you. (`ws` from the record is now unused for placement.)
    (void)ws;
    const auto curMon = g_pCompositor->getMonitorFromCursor();
    const auto here   = curMon ? curMon->activeWorkspaceID() : (w ? w->workspaceID() : 1);
    runDispatcher("movetoworkspacesilent", std::to_string(here) + ",address:" + addr);
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);

    if (const auto m = curMon ? curMon : g_pCompositor->getMonitorFromID(w->monitorID())) {
        CBox       home{};
        const auto fb = captureCardFly(w, m, home);
        if (fb) {
            const CBox wb = w->getWindowMainSurfaceBox();
            g_minFlyLastT = Time::steadyNow();
            g_minFlies.push_back(MinFly{fb, dockTargetBox(m, wb.h > 0.0 ? wb.w / wb.h : 1.6), home, 0.0f, m, ""});
        }
    }
    sendWaverunner("min-del " + addr + "\n");
    unlink(thumb.c_str());
    trace("restore %s -> ws=%d (current)", addr.c_str(), (int)here);
    damageAll();
    return 0;
}

/// Capture a set of whole WORKSPACES, one file each.
///
/// The deck's other half. In Golem's per-workspace stage a tile stands for a
/// desk rather than a window, so its picture has to be the workspace as the user
/// built it — every window where they put it — which is a different render from
/// `captureDeckToDir`'s standalone window, not a crop of it.
///
/// It is the overview's own workspace snapshot (`captureWorkspaces`) reduced to
/// what a thumbnail needs: no tile grid, no backdrop, no per-window crops, and
/// written out instead of kept. The delicate parts are copied from there because
/// they were learned the hard way — the solitary-client reset, hiding the live
/// workspace so its windows do not bleed into every tile, and snapping each
/// workspace's windows on-screen before rendering it.
///
/// Each workspace is written to `<dir>/ws-<id>.rgba`. Returns how many landed.
static int captureDesksToDir(const std::string& wsCsv, int size, double tileAspect, const std::string& dir) {
    if (size <= 0 || size > 1024)
        return 0;

    std::vector<PHLWORKSPACE> wants;
    PHLMONITOR                m;
    for (size_t at = 0; at <= wsCsv.size();) {
        const size_t end = std::min(wsCsv.find(',', at), wsCsv.size());
        const auto   tok = wsCsv.substr(at, end - at);
        at               = end + 1;
        if (tok.empty())
            continue;
        int id = 0;
        try {
            id = std::stoi(tok);
        } catch (...) { continue; }
        const auto ws = g_pCompositor->getWorkspaceByID((WORKSPACEID)id);
        if (!ws)
            continue;
        // The monitor comes from a window on the workspace, the way the window
        // capture takes it from the window itself: one monitor per pass, since a
        // snapshot is rendered for a specific output and is only valid there.
        PHLMONITOR wm;
        for (auto& w : g_pCompositor->m_windows) {
            if (w && w->m_isMapped && !w->isHidden() && w->workspaceID() == ws->m_id) {
                wm = g_pCompositor->getMonitorFromID(w->monitorID());
                break;
            }
        }
        if (!wm)
            continue; // an empty desk has no tile, so nothing is owed one
        if (!m)
            m = wm;
        else if (m != wm)
            continue;
        wants.push_back(ws);
    }
    if (wants.empty() || !m)
        return 0;

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    auto out = g_pHyprRenderer->createFB("waveview-desk");
    if (!out->alloc(size, size, DRM_FORMAT_ABGR8888) || !out->isAllocated()) {
        trace("desk-capture: alloc %dx%d failed", size, size);
        return 0;
    }
    // One monitor-resolution scratch for the whole batch: every workspace
    // renders at exactly this size, so it is allocated once.
    const CBox monbox{0.0, 0.0, m->m_pixelSize.x, m->m_pixelSize.y};
    auto       scratch = g_pHyprRenderer->createFB("waveview-desk-src");
    if (!scratch->alloc(monbox.w, monbox.h, DRM_FORMAT_ABGR8888)) {
        out->release();
        return 0;
    }

    const auto startedOn = m->m_activeWorkspace;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true; // a thumbnail must not drive the client's frame clock
    g_capturing                              = true; // suppress our own render hook
    m->m_solitaryClient.reset(); // else renderWorkspace draws only the one fullscreen window
    if (startedOn)
        startedOn->m_visible = false; // the live workspace's windows would bleed into every tile

    std::vector<uint8_t> px;
    int                  done = 0;
    for (const auto& ws : wants) {
        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(m, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, scratch);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        m->m_activeWorkspace = ws; // renderWorkspace draws the monitor's active ws
        // Non-active workspaces are parked offscreen; without this snap the
        // render captures nothing of them. instant=true so the real desktop
        // does not visibly animate.
        g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
        ws->m_visible = true;
        g_pHyprRenderer->renderWorkspace(m, ws, Time::steadyNow(), monbox);
        ws->m_visible = false;
        g_pDesktopAnimationManager->startAnimation(ws, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        if (!blitAndRead(m, scratch, out, size, tileAspect, px))
            continue;
        if (writeRaw(dir + "/ws-" + std::to_string((int)ws->m_id) + ".rgba", px))
            ++done;
    }

    g_capturing                              = false;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
    m->m_activeWorkspace                     = startedOn;
    if (startedOn) {
        startedOn->m_visible = true;
        g_pDesktopAnimationManager->startAnimation(startedOn, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
    }
    scratch->release();
    out->release();
    return done;
}

// `hl.plugin.waveview.capture_desks(workspaces, size, tile_aspect, dir)` —
// `workspaces` is a comma-separated list of ids; each lands at
// `<dir>/ws-<id>.rgba` as `size`×`size` raw RGBA. Returns how many were written.
// Best effort by contract, exactly like `capture_deck`.
static int luaCaptureDesks(lua_State* L) {
    const char*  wss    = lua_tostring(L, 1);
    const int    size   = (int)lua_tointeger(L, 2);
    const double aspect = lua_tonumber(L, 3);
    const char*  dir    = lua_tostring(L, 4);
    int          n      = 0;
    if (wss && dir)
        n = captureDesksToDir(wss, size, aspect, dir);
    lua_pushinteger(L, n);
    return 1;
}

// `hl.plugin.waveview.capture_deck(addrs, size, tile_aspect, dir)` — `addrs` is a
// comma-separated list of window addresses; each lands at `<dir>/<addr>.rgba` as
// `size`×`size` raw RGBA. Returns how many were written. Best effort by
// contract: the deck falls back to a title-only tile, never to an error.
static int luaCaptureDeck(lua_State* L) {
    const char* addrs = lua_tostring(L, 1);
    const int   size  = (int)lua_tointeger(L, 2);
    const double aspect = lua_tonumber(L, 3);
    const char*  dir    = lua_tostring(L, 4);
    int          n      = 0;
    if (addrs && dir)
        n = captureDeckToDir(addrs, size, aspect, dir);
    lua_pushinteger(L, n);
    return 1;
}

// `hl.plugin.waveview.close()` — shut the overview if it is up, and do nothing
// if it is not.
//
// A toggle cannot serve as a close, and this is where that bit: waverunner shuts
// the map as it opens the stage, and `set_stage` below shuts it too. Through
// `toggle()` the second call REOPENED it, so opening the stage from inside the
// overview left the overview on screen (Max, 2026-09-12). `closeOverview` is
// idempotent by construction — it returns early unless the map is actually up.
static int luaClose(lua_State* L) {
    closeOverview();
    snapCloseSpread(); // "close" means everything of ours off the screen, now
    return 0;
}

// `hl.plugin.waveview.stage_ready()` — waverunner has finished rebuilding the
// STAGE after our `stage-resume`, so the close it was holding may now run.
//
// This is the second beat of `startClose`: the tile we are about to fly into
// still holds a picture of the bare workspace (the map lifted the stage's
// concealment when it opened), so it is RE-PHOTOGRAPHED here — just that one
// tile, the only one the rebuild changed, because a full pass stalls a frame
// and we are about to animate. The zoom then flies into the stage and lands on
// the stage it already shows: no workspace step (Max, 2026-09-17).
//
// waverunner answers whether or not it actually had a stage to rebuild, so the
// ordinary paths (the daemon shutting the map as it opens the stage) cost one
// socket round-trip and not the fallback wait.
// `hl.plugin.waveview.recapture()` — waverunner has just changed the desktop
// under an OPEN map and wants it photographed NOW.
//
// The live timer refuses to capture mid-animation, so opening over a stage used
// to show the STAGE picture for the whole zoom and only pop to the real
// workspace once it settled — the windows "moving out of the way" arrived after
// the map had finished opening, which is what reads as slow no matter how fast
// the zoom is (Max, 2026-09-17, twice). This is the open's half of the close's
// mid-flight morph: same cost, 1-4ms, and the spread then plays ON the zoom.
static int luaRecapture(lua_State*) {
    if (!g_active)
        return 0;
    if (const auto m = g_captureMon.lock())
        captureWorkspaces(m, ALL_TILES);
    damageAll();
    return 0;
}

static int luaStageReady(lua_State*) {
    if (!g_stageClosePending)
        return 0;
    g_stageClosePending = false;
    // The arrival is already in flight (`startClose`). Re-photograph MID-MOTION —
    // exactly where the open does it, and exactly why the open reads well: its
    // tile morphs while it zooms OUT, and nobody sees a rebuild, they see one
    // movement. Doing it before the motion is what Max saw as *"the rebuilding"*.
    //
    // EVERY tile, not just the one we fly into: the dim is back on the desktop
    // by now and it is baked into whatever we photograph, so re-shooting only the
    // zoom tile left ONE dark tile growing across a bright grid — darkness that
    // arrives gradually, over the whole flight. Max, 2026-09-17: *"the dimming
    // should happen at the same time that the windows start coming, it comes too
    // late."* Shooting all of them lands the dim as ONE step, on the same frame
    // the picture morphs — which is the moment he is pointing at.
    // A full pass is 1-4ms (measured, see the trace) against a 16ms frame.
    if (const auto m = g_captureMon.lock(); m && g_zoomTile >= 0 && g_zoomTile < N_TILES)
        captureWorkspaces(m, 1u << g_zoomTile);
    g_stageDimPaint = true; // same frame as the morph: one step, not a growth
    g_stageDimAsk   = true;
    damageAll();
    return 0;
}

// `hl.plugin.waveview.spread()` — the 3-up decision without a trackpad (a
// swipe cannot be faked from a script; this is the spread's debug/CLI route,
// same idiom as the toggle dispatch trick). Walks the ladder exactly like the
// gesture: spread when this workspace hides windows, escalate when the spread
// is up, the overview otherwise.
static int luaSpread(lua_State*) {
    if (!g_active && !g_spreadActive && g_animTarget < 0.5f) {
        const auto m = g_pCompositor->getMonitorFromCursor();
        if (wsNeedsSpread(m)) {
            openSpread(m);
            return 0;
        }
    }
    toggle();
    return 0;
}

// `hl.plugin.waveview.set_stage(true|false)` — waverunner tells us when Golem's
// STAGE mode owns the screen. Entering also shuts the overview if it happens to
// be open, so the stage never comes up underneath it.
static int luaSetStage(lua_State* L) {
    g_stageMode = lua_toboolean(L, 1) != 0;
    if (g_stageMode && g_active)
        closeOverview();
    if (g_stageMode)
        snapCloseSpread(); // the stage takes the whole screen; nothing to glide over
    return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    // Entry point for the keybind. This Lua-config Hyprland routes `hyprctl
    // dispatch` through hl.dispatch(), which only accepts built-in hl.dsp.*
    // dispatchers — plugin dispatchers (addDispatcherV2) never surface there. So
    // a Lua function is the ONLY reachable trigger; bind it deferred so the lookup
    // happens at keypress (plugin loads after config eval):
    //   hl.bind(mainMod .. " + G", function() hl.plugin.waveview.toggle() end)
    HyprlandAPI::addLuaFunction(handle, "waveview", "toggle", luaToggle);
    HyprlandAPI::addLuaFunction(handle, "waveview", "close", luaClose);
    HyprlandAPI::addLuaFunction(handle, "waveview", "stage_ready", luaStageReady);
    HyprlandAPI::addLuaFunction(handle, "waveview", "recapture", luaRecapture);
    HyprlandAPI::addLuaFunction(handle, "waveview", "spread", luaSpread);
    HyprlandAPI::addLuaFunction(handle, "waveview", "minimize", luaMinimize);
    HyprlandAPI::addLuaFunction(handle, "waveview", "restore_min", luaRestoreMin);
    HyprlandAPI::addLuaFunction(handle, "waveview", "set_stage", luaSetStage);
    HyprlandAPI::addLuaFunction(handle, "waveview", "capture_deck", luaCaptureDeck);
    HyprlandAPI::addLuaFunction(handle, "waveview", "capture_desks", luaCaptureDesks);
    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage s) { onRender(s); });
    g_keyListener    = Event::bus()->m_events.input.keyboard.key.listen(onKey);
    g_moveListener   = Event::bus()->m_events.input.mouse.move.listen(onMouseMove);
    g_buttonListener = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_axisListener   = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_swipeBeginListener  = Event::bus()->m_events.gesture.swipe.begin.listen(onSwipeBegin);
    g_swipeUpdateListener = Event::bus()->m_events.gesture.swipe.update.listen(onSwipeUpdate);
    g_swipeEndListener    = Event::bus()->m_events.gesture.swipe.end.listen(onSwipeEnd);
    // Golem's titlebars for floating windows (src/hyprbars/, a stripped fork of
    // hyprbars — see PROVENANCE.md there). They live in this plugin rather than
    // beside it so there is ONE thing to keep in ABI lockstep with Hyprland.
    Bars::init(handle);
    g_windowOpenListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { Bars::onWindowOpen(w); });
    // A window that dies while minimized must leave the dock too — and its
    // record must go, because Hyprland REUSES window pointer addresses.
    g_windowDestroyListener = Event::bus()->m_events.window.destroy.listen([](PHLWINDOW w) {
        const std::string addr = windowAddr(w);
        const bool        had  = std::erase_if(g_minimized, [&](const MinRec& r) { return r.addr == addr; }) > 0;
        if (had) {
            sendWaverunner("min-del " + addr + "\n");
            unlink((minThumbDir() + "/" + addr + ".rgba").c_str());
        }
    });
    g_liveTimer      = makeShared<CEventLoopTimer>(std::nullopt, onLiveTimer, nullptr);
    g_pEventLoopManager->addTimer(g_liveTimer);
    g_dragCheckTimer = makeShared<CEventLoopTimer>(std::nullopt, onDragCheckTimer, nullptr);
    g_pEventLoopManager->addTimer(g_dragCheckTimer);
    g_handTimer = makeShared<CEventLoopTimer>(std::nullopt, onHandTimer, nullptr);
    g_pEventLoopManager->addTimer(g_handTimer);
    g_minReapTimer = makeShared<CEventLoopTimer>(std::nullopt, onMinReapTimer, nullptr);
    g_pEventLoopManager->addTimer(g_minReapTimer);
    HyprlandAPI::addNotification(handle, std::string("[waveview] loaded -- ") + waveview_hello(),
                                 CHyprColor(0.3, 1.0, 0.5, 1.0), 3000);
    // Bump on every behavior change: crash reports print this, and it's the
    // only way to tell a stale loaded .so from the freshly built one.
    return {"waveview", "Live 3x3 workspace overview (Rust brain + C++ shim)", "max", "0.82"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Unloading while the overview draws is how the 2026-08-30 crash happened:
    // our CUVTexElement/CUVResetElement vtables live in this .so, and Hyprland
    // clears the PREVIOUS frame's pass at the START of the next one
    // (CRenderPass::clear() is beginRender's first call) — so a hot-unload
    // mid-draw leaves queued elements whose virtual dtors point into unmapped
    // memory. Hard-close first, then flush the pass while we're still mapped.
    if (g_active) {
        restoreOriginal(); // ends a live drag; float + workspace restored
        notifyWaverunner(false);
    } else if (g_spreadActive)
        notifyWaverunner(false); // the dock must come back when we go
    // Unloaded mid-open-warp: hand the pointer back before our code is gone,
    // or the session is left with an invisible cursor and no one to restore it.
    g_shapeWantSet = false;
    g_warpPending  = false;
    g_warpWin.reset();
    endWarpHide();
    g_active     = false;
    g_animTarget = 0.0f;
    g_landAnim   = 0.0f;
    g_closeLinger = 0;
    g_freePending = false; // PLUGIN_EXIT frees synchronously below
    g_spreadActive = false;
    g_spreadTarget = 0.0f;
    g_spreadHover.reset();
    g_spreadPick.reset();
    g_spreadMon.reset();
    g_dragWin.reset();
    g_hoverWin.reset();
    g_watchWin.reset();
    g_resizing = false;
    g_resizeWin.reset();
    resetEdgeCursor();
    g_commit  = {};
    g_pending = {};
    // The bars hold per-window decorations and their own listeners; drop them
    // while this .so is still mapped, for the same reason the render pass is
    // flushed above.
    g_windowOpenListener.reset();
    g_windowDestroyListener.reset();
    for (auto& f : g_minFlies) // fly textures die with the .so; the minimized RECORDS stay meaningful to a reload via windowByAddr
        if (f.fb)
            f.fb->release();
    g_minFlies.clear();
    for (auto& fb : g_minReap)
        if (fb)
            fb->release();
    g_minReap.clear();
    if (g_minReapTimer) {
        g_pEventLoopManager->removeTimer(g_minReapTimer);
        g_minReapTimer.reset();
    }
    Bars::shutdown();
    g_renderListener.reset();
    g_keyListener.reset();
    g_moveListener.reset();
    g_buttonListener.reset();
    g_swipeBeginListener.reset();
    g_swipeUpdateListener.reset();
    g_swipeEndListener.reset();
    if (g_liveTimer) {
        g_pEventLoopManager->removeTimer(g_liveTimer);
        g_liveTimer.reset();
    }
    if (g_resizeDragSent) {
        g_resizeDragSent = false;
        sendWaverunner("resize-drag-off\n");
    }
    if (g_dragCheckTimer) {
        g_pEventLoopManager->removeTimer(g_dragCheckTimer);
        g_dragCheckTimer.reset();
    }
    if (g_handTimer) {
        g_pEventLoopManager->removeTimer(g_handTimer);
        g_handTimer.reset();
    }
    freeCaptures();
    freeSpread();
    // Destroy any of OUR queued pass elements now, not next frame. Stock
    // elements die a frame early with them: harmless, the pass rebuilds.
    g_pHyprRenderer->m_renderPass.clear();
    damageAll();
}
