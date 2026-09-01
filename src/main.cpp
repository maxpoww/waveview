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
static constexpr double DSN_OUTER      = 35.0; // side + bottom margins
static constexpr double DSN_TOP_GAP    = 12.0; // below the bar
static constexpr double DSN_TILE_ROUND = 28.0; // hover/drop frame corners
static constexpr double DSN_WIN_ROUND  = 20.0; // window mini corners
static constexpr double DSN_WIN_GAP    = 0.028; // in-tile gap, fraction of tile
// Overview borders mirror the DESKTOP's window borders (hyprland.lua:
// general.border_size 3, active color #ffbe98) — same thickness on every
// window regardless of its size, same amber, per Max.
static constexpr double     DSN_BORDER_W = 3.0; // logical, scaled at use
static const CHyprColor     DSN_BORDER_COL{1.0, 0.745, 0.596, 1.0};   // #ffbe98

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
    Rect                     logical; // window box in logical layout coords
    int                      tile;    // grid slot 0..8 (its workspace)
    bool                     active;  // currently focused window
    CBox                     screen;  // last-drawn box in draw space (for hit-testing)
    CBox                     drawCur; // eased chase of the mapped slot — re-tiles glide at frame rate
};
// One shared ease rate for every drag chase (slot glides, ghost dims) —
// slowed from the original 14/s snap to sit closer to the desktop's
// window spring, so reactions read as motion, not as teleports.
static constexpr float PREVIEW_RATE = 9.f;
static std::vector<CapWin> g_wins;

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
static constexpr float ANIM_SECONDS = 0.28f;

// Trackpad gesture: a 3-finger vertical swipe toggles the overview (up = open,
// down = close). Deltas accumulate over the gesture; once the dominant axis is
// vertical and past SWIPE_TRIGGER we fire once and latch until the gesture ends.
static uint32_t         g_swipeFingers = 0;
static Vector2D         g_swipeAcc;
static bool             g_swipeFired   = false;
static constexpr double SWIPE_TRIGGER  = 120.0; // accumulated px of vertical travel

static double mix(double a, double b, double t) {
    return a + (b - a) * t;
}
static float easeOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
// Ease-in-out cubic: gentle start, gentle landing — the page-flip curve.
static double easeInOutCubic(double t) {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

// Defined further down; used by the pointer handlers above their definitions.
static void jumpTo(int wsId);
static void jumpToWindow(PHLWINDOW w);
static void updateHoverAt(PHLMONITOR m, const Vector2D& c);
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
        // resumes the moment they separate.
        bool overlapped = false;
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
            td.primarySurfaceUVTopLeft     = Vector2D(u0, v0);
            td.primarySurfaceUVBottomRight = Vector2D(u1, v1);
            Render::GL::g_pHyprOpenGL->renderTexture(srcTex, CBox{0.0, 0.0, (double)fbw, (double)fbh}, td);

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
static void drawOverview(PHLMONITOR m, float p, int zoomTile) {
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
    // A rounded border, drawn as a filled rounded rect *behind* the window:
    // the texture (rounded to `round`) covers the interior, leaving a ring.
    // CONSTANT width matching the desktop's borders — never proportional
    // (small windows used to get thin halos).
    const double bw = DSN_BORDER_W * m->m_scale;
    auto         haloBorder = [&](const CBox& box, int round) {
        renderRect(CBox{box.x - bw, box.y - bw, box.w + 2.0 * bw, box.h + 2.0 * bw}, DSN_BORDER_COL,
                   round + (int)std::lround(bw));
    };

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
        ok[i]           = true;
        real[i]         = {mini.x, mini.y, mini.x + mini.w, mini.y + mini.h};
        const Rect&  t  = tiles[cw.tile];
        const double thrX = t.w * 0.02, thrY = t.h * 0.02;
        tgt[i]      = real[i];
        tgt[i].x0   = (tgt[i].x0 - t.x < thrX) ? t.x : tgt[i].x0;
        tgt[i].y0   = (tgt[i].y0 - t.y < thrY) ? t.y : tgt[i].y0;
        tgt[i].x1   = (t.x + t.w - tgt[i].x1 < thrX) ? t.x + t.w : tgt[i].x1;
        tgt[i].y1   = (t.y + t.h - tgt[i].y1 < thrY) ? t.y + t.h : tgt[i].y1;
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
                if (!ok[i] || g_wins[i].tile != tile)
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
    // slots), then apply the live swap preview before drawing.
    std::vector<CBox> boxes(g_wins.size());
    ssize_t           dragIdx = -1, swapIdx = -1;
    for (size_t i = 0; i < g_wins.size(); ++i) {
        if (!ok[i])
            continue;
        const double x0 = mix(real[i].x0, tgt[i].x0, p), y0 = mix(real[i].y0, tgt[i].y0, p);
        const double x1 = mix(real[i].x1, tgt[i].x1, p), y1 = mix(real[i].y1, tgt[i].y1, p);
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
}

// While the overview is open, re-capture thumbnails on a timer so they stay live.
// Fires outside the render pass (so beginRender is safe) and re-arms itself.
static void onLiveTimer(SP<CEventLoopTimer> self, void*) {
    checkFloatWatch(); // float-leak watch outlives the overview (leaks show on the desktop)
    if (!g_active) {
        if (g_watchWin.lock())
            self->updateTimeout(std::chrono::milliseconds(100)); // keep watching ≤3s past close
        return; // disarmed on close; don't re-arm
    }
    // Never recapture mid-animation: snapshotting 18 workspaces stalls a
    // frame, which reads as a hitch in the page-flip / zoom glide. Poll
    // quickly until the motion settles, then catch up.
    if (g_scrollProg < 1.0f || g_anim != g_animTarget) {
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
    if (!g_active || g_animTarget < 0.5f)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
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
static void setOverviewCursor(const char* shape, bool force = false) {
    if (!shape) {
        if (!g_edgeShape.empty()) {
            g_pCursorManager->setCursorFromName("left_ptr");
            g_edgeShape.clear();
        }
        return;
    }
    if (force || g_edgeShape != shape) {
        g_pCursorManager->setCursorFromName(shape);
        g_edgeShape = shape;
    }
}

// While dragging, re-assert the closed hand on a throttle rather than on
// every motion event (a fast mouse delivers hundreds a second, and each
// re-assert re-renders the cursor).
static std::chrono::steady_clock::time_point g_handReassert{};
static void                                  holdGrabbingCursor() {
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
        setOverviewCursor(shape);
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
        holdGrabbingCursor();
    else if (hov.lock())
        setOverviewCursor("grab");
    else if (tileAt(m, c) >= 0)
        setOverviewCursor("pointer");
    else
        setOverviewCursor(nullptr);
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
    if (!g_active)
        noteInteraction(true);
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
    if (!g_active && e.state == WL_POINTER_BUTTON_STATE_PRESSED)
        noteInteraction(true);
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

static void onRender(eRenderStage stage) {
    if (!g_active || g_capturing || stage != eRenderStage::RENDER_POST_WINDOWS)
        return;
    const auto m = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!m)
        return;

    // Advance the zoom animation by wall-clock dt (guard first-frame / stalls).
    const auto now = Time::steadyNow();
    float      dt  = std::chrono::duration<float>(now - g_animLastT).count();
    g_animLastT    = now;
    if (dt <= 0.f || dt > 0.1f)
        dt = 0.016f;
    g_frameDt        = dt;
    const float step = dt / ANIM_SECONDS;
    if (g_anim < g_animTarget)
        g_anim = std::min(g_animTarget, g_anim + step);
    else if (g_anim > g_animTarget)
        g_anim = std::max(g_animTarget, g_anim - step);

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

    // Fully closed: disengage and stop the live timer.
    if (g_animTarget <= 0.f && g_anim <= 0.f) {
        g_active = false;
        g_hoverWin.reset();
        g_dragWin.reset();
        g_resizing = false;
        g_resizeWin.reset();
        resetEdgeCursor();
        g_pressTile = -1;
        if (g_liveTimer)
            g_liveTimer->updateTimeout(std::nullopt);
        freeCaptures(); // closed = zero VRAM held; reopen recaptures
        damageAll();
        return;
    }

    drawOverview(m, easeOutCubic(g_anim), g_zoomTile);

    // Keep frames coming while the zoom is still moving.
    if (g_anim != g_animTarget) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

// Fire-and-forget a control verb to waverunner (the dock/topbar daemon)
// over its socket on a detached thread; a dead daemon = nothing to tell.
// Takes the message BY VALUE and moves it into the thread: callers build
// verbs with payloads (titles, sizes) in temporaries, and a captured
// `const char*` into one of those would dangle before the write.
static void sendWaverunner(std::string msg) {
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

// --- Overview → topbar (waverunner draws the bar over the overview) --------
// The current-task pill follows the POINTER while we own the screen: it
// shows the hovered thumbnail's title, plus the live size while a thumbnail
// is being resized. Both are fire-and-forget verbs; the resize stream is
// throttled because it rides pointer motion (one socket write per motion
// event would be one detached thread per event).
static std::string                                 g_sentHoverTitle;
static bool                                        g_sentHoverValid = false;
static std::string                                 g_sentSize;
static std::chrono::steady_clock::time_point       g_sizeSentAt{};
static constexpr std::chrono::milliseconds         SIZE_SEND_EVERY{50};

static void sendOverviewHover(PHLWINDOW w) {
    const std::string title = w ? w->m_title : std::string{};
    if (g_sentHoverValid && title == g_sentHoverTitle)
        return;
    g_sentHoverTitle = title;
    g_sentHoverValid = true;
    // Titles can hold anything except our line terminator; strip newlines.
    std::string line = "overview-hover " + title;
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
    g_sentHoverTitle.clear();
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
static void closeOverview() {
    if (g_animTarget < 0.5f)
        return;
    endRealResize();   // sizes applied live are final; just settle
    resetEdgeCursor(); // the desktop must not inherit a resize pointer
    restoreOriginal(); // never leave a real drag dangling; a commit is undone
    g_dragWin.reset();
    g_dragMoved  = false;
    g_animTarget = 0.0f;
    g_animLastT  = Time::steadyNow();
    notifyWaverunner(false);
    damageAll();
}

static void toggle() {
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
    g_animTarget = 1.0f;
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
        captureWorkspaces(m); // snapshot on open, outside the render pass
        // The pointer must exist over the overview: the capture's workspace
        // juggling can leave the cursor surfaceless (shouldRenderCursor
        // needs one), and the motion-cancel delays the natural re-arm —
        // "overview opens sometimes with no pointer". Unhide and pin the
        // default arrow for the overview's lifetime.
        g_pHyprRenderer->setCursorHidden(false);
        g_pCursorManager->setCursorFromName("left_ptr");
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
    if (g_active)
        info.cancelled = true;
}

// Accumulate the swipe; on a decisive 3-finger vertical move (once per
// gesture): swipe UP walks the same ladder as Super+R — open, then tour the
// other inhabited page, then close; swipe DOWN is Escape (immediate close,
// no touring). libinput reports fingers-up as negative dy.
static void onSwipeUpdate(IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
    if (g_active)
        info.cancelled = true; // overview owns the trackpad while open
    if (g_swipeFingers != 3 || g_swipeFired)
        return;
    g_swipeAcc += e.delta;
    if (std::abs(g_swipeAcc.y) < SWIPE_TRIGGER || std::abs(g_swipeAcc.x) > std::abs(g_swipeAcc.y))
        return; // not yet decisive, or dominantly horizontal

    if (g_swipeAcc.y < 0.0)
        toggle(); // up: open → tour → close (the Super+R ladder)
    else
        closeOverview(); // down: Esc (no-op when already closed)
    g_swipeFired   = true;
    info.cancelled = true; // consume so no built-in gesture also reacts
}

static void onSwipeEnd(IPointer::SSwipeEndEvent, Event::SCallbackInfo& info) {
    if (g_active)
        info.cancelled = true;
    g_swipeFingers = 0;
    g_swipeFired   = false;
}

// Jump to workspace `wsId` (1..9) and close the overview by zooming into that
// workspace's tile. Switching workspace happens under the still-covering
// overview; the close animation then flies into the chosen tile, so releasing
// the overview reveals the workspace we just switched to — seamless.
static void jumpTo(int wsId) {
    if (!g_active)
        return;
    const int t = waveview_tile_for_workspace(wsId);
    if (t < 0)
        return;

    g_zoomTile   = t;    // close animation pivots on (zooms into) the chosen tile
    g_animTarget = 0.0f; // animate closed
    g_animLastT  = Time::steadyNow();
    notifyWaverunner(false);

    if (g_pKeybindManager) {
        const auto it = g_pKeybindManager->m_dispatchers.find("workspace");
        if (it != g_pKeybindManager->m_dispatchers.end())
            it->second(std::to_string(wsId));
    }
    damageAll();
}

// Clicking a window: jump to its workspace (same seamless zoom-into-tile close as
// digit-jump) and focus that specific window, so releasing the overview lands on
// exactly the window that was clicked — not just its workspace's last focus.
static void jumpToWindow(PHLWINDOW w) {
    if (!w)
        return;
    jumpTo(static_cast<int>(w->workspaceID())); // switch workspace + start the close
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_CLICK);
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
    if (!g_active && !g_superHeld && e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
        noteInteraction(false);
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
    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage s) { onRender(s); });
    g_keyListener    = Event::bus()->m_events.input.keyboard.key.listen(onKey);
    g_moveListener   = Event::bus()->m_events.input.mouse.move.listen(onMouseMove);
    g_buttonListener = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_axisListener   = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_swipeBeginListener  = Event::bus()->m_events.gesture.swipe.begin.listen(onSwipeBegin);
    g_swipeUpdateListener = Event::bus()->m_events.gesture.swipe.update.listen(onSwipeUpdate);
    g_swipeEndListener    = Event::bus()->m_events.gesture.swipe.end.listen(onSwipeEnd);
    g_liveTimer      = makeShared<CEventLoopTimer>(std::nullopt, onLiveTimer, nullptr);
    g_pEventLoopManager->addTimer(g_liveTimer);
    g_dragCheckTimer = makeShared<CEventLoopTimer>(std::nullopt, onDragCheckTimer, nullptr);
    g_pEventLoopManager->addTimer(g_dragCheckTimer);
    g_handTimer = makeShared<CEventLoopTimer>(std::nullopt, onHandTimer, nullptr);
    g_pEventLoopManager->addTimer(g_handTimer);
    HyprlandAPI::addNotification(handle, std::string("[waveview] loaded -- ") + waveview_hello(),
                                 CHyprColor(0.3, 1.0, 0.5, 1.0), 3000);
    // Bump on every behavior change: crash reports print this, and it's the
    // only way to tell a stale loaded .so from the freshly built one.
    return {"waveview", "Live 3x3 workspace overview (Rust brain + C++ shim)", "max", "0.36"};
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
    }
    g_active     = false;
    g_animTarget = 0.0f;
    g_dragWin.reset();
    g_hoverWin.reset();
    g_watchWin.reset();
    g_resizing = false;
    g_resizeWin.reset();
    resetEdgeCursor();
    g_commit  = {};
    g_pending = {};
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
    // Destroy any of OUR queued pass elements now, not next frame. Stock
    // elements die a frame early with them: harmless, the pass rebuilds.
    g_pHyprRenderer->m_renderPass.clear();
    damageAll();
}
