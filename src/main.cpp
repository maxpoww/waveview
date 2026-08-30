// Pull in the standard headers Hyprland includes transitively BEFORE the
// private/public hack below, so their include guards are already set and the
// macro can't rewrite libstdc++ access specifiers.
#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstring>
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
static constexpr int N_TILES = 18;

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
    float                    previewT    = 0.f; // split-preview glide: 0 = home, 1 = at its kept half
    CBox                     previewBox;        // the half it glides toward (kept for the ease-back)
    bool                     holdPreview = false; // drop landed here: hold the preview until recapture
};
static std::vector<CapWin> g_wins;

// Pointer interaction, all in "draw space" (whole monitor = [0,0,transformedSize]).
// Tracked by window handle, not g_wins index — the vector is rebuilt every capture.
static PHLWINDOWREF g_hoverWin;              // window under the cursor (gets a border)
static PHLWINDOWREF g_dragWin;              // window pressed on; becomes a drag once the cursor moves past CLICK_SLOP
static Vector2D     g_dragCursor;            // current cursor in draw space (while dragging)
static Vector2D     g_dragGrab;              // cursor→box-topleft offset captured at grab
static Vector2D     g_pressPos;              // cursor at button-press (draw space) — to tell a click from a drag
static bool         g_dragMoved = false;     // cursor left the CLICK_SLOP radius since press → treat as a drag, not a click
static int          g_hoverTile = -1;        // empty workspace tile under the cursor (gets a border), or -1
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
static void beginRealDrag(PHLWINDOW dw);
static void endRealDrag(std::optional<Vector2D> at, PHLWINDOW splitTarget = nullptr);

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
static void captureWindows(PHLMONITOR m) {
    // Remember each window's last-drawn hit-box so pointer hit-testing keeps
    // working across the rebuild — otherwise the fresh entries carry a zeroed
    // box until the next drawOverview, and a pointer move in that gap drops the
    // hover (the border blinks ~every REFRESH_MS).
    struct Carry {
        CBox  screen;
        float previewT;
        CBox  previewBox;
    };
    std::vector<std::pair<PHLWINDOWREF, Carry>> prevBoxes;
    prevBoxes.reserve(g_wins.size());
    // Mid-drag, the dragged window is parked offscreen — recropping it
    // would blank the cursor ghost. Stash its whole capture and reuse it.
    std::optional<CapWin> stashDragged;
    for (auto& cw : g_wins) {
        prevBoxes.emplace_back(cw.win, Carry{cw.screen, cw.previewT, cw.previewBox});
        if (g_dragReal && cw.win.lock() && cw.win.lock() == g_dragWin.lock()) {
            stashDragged = cw; // keep its fb alive
            continue;
        }
        if (cw.fb) // free last cycle's textures before rebuilding
            cw.fb->release();
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

        const CBox wb = w->getWindowMainSurfaceBox(); // logical coords
        if (wb.w <= 1.0 || wb.h <= 1.0)
            continue;

        // The window's rect as normalized UV within the full-workspace snapshot.
        const double u0 = std::clamp((wb.x - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v0 = std::clamp((wb.y - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        const double u1 = std::clamp((wb.x + wb.w - m->m_position.x) / m->m_size.x, 0.0, 1.0);
        const double v1 = std::clamp((wb.y + wb.h - m->m_position.y) / m->m_size.y, 0.0, 1.0);
        if (u1 - u0 <= 0.0 || v1 - v0 <= 0.0)
            continue; // fully offscreen

        CapWin cw;
        cw.win     = w;
        cw.logical = Rect{wb.x, wb.y, wb.w, wb.h};
        cw.tile    = tile;
        cw.active  = g_pCompositor->isWindowActive(w);

        const int fbw = std::max(1, (int)std::lround(wb.w * scale));
        const int fbh = std::max(1, (int)std::lround(wb.h * scale));
        cw.fb         = g_pHyprRenderer->createFB("waveview-win");
        cw.fb->alloc(fbw, fbh, DRM_FORMAT_ABGR8888);

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

        for (auto& pb : prevBoxes) // carry hit-box + preview glide forward across the rebuild
            if (pb.first.lock() == w) {
                cw.screen     = pb.second.screen;
                cw.previewT   = pb.second.previewT;
                cw.previewBox = pb.second.previewBox;
                break;
            }

        g_wins.push_back(std::move(cw));
    }
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

// The monitor's USABLE logical area — position/size minus every reserved
// strip. Windows are mapped into tiles against THIS, not the full monitor:
// mapping against the full monitor bakes the bar strip into every tile as
// a dead band no window can ever occupy (the "out gaps" that survived four
// rounds of seam logic — a maximized window must BE the full tile).
static void usableArea(PHLMONITOR m, double& x, double& y, double& w, double& h) {
    const double l = m->m_reservedArea.left(), r = m->m_reservedArea.right();
    const double t = m->m_reservedArea.top(), b = m->m_reservedArea.bottom();
    x = m->m_position.x + l;
    y = m->m_position.y + t;
    w = std::max(1.0, m->m_size.x - l - r);
    h = std::max(1.0, m->m_size.y - t - b);
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

static void captureWorkspaces(PHLMONITOR m) {
    if (!m)
        return;

    Rect tiles[N_TILES];
    if (computeTiles(m, tiles) < N_TILES)
        return;

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

    for (int i = 0; i < N_TILES; ++i) {
        auto& fb = g_fbs[i];
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

    // With all 9 workspace snapshots ready, build the two things we actually draw:
    // the single wallpaper backdrop, and one cropped texture per window.
    captureBackdrop(m, monbox);
    captureWindows(m);

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

        const CBox wb = w->getWindowMainSurfaceBox();
        Rect       mini;
        double     ux, uy, uw, uh;
        usableArea(m, ux, uy, uw, uh);
        waveview_map_window(tiles[ti].x, tiles[ti].y, tiles[ti].w, tiles[ti].h, ux, uy, uw, uh, wb.x, wb.y, wb.w,
                            wb.h, &mini);
        if (mini.w <= 0.0 || mini.h <= 0.0)
            continue;

        const bool active = g_pCompositor->isWindowActive(w);
        renderRect(CBox{mini.x, mini.y, mini.w, mini.h},
                   active ? CHyprColor(0.40, 0.70, 1.0, 0.90) : CHyprColor(0.85, 0.85, 0.90, 0.80));
    }
}

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
    double               ux, uy, uw2, uh2;
    usableArea(m, ux, uy, uw2, uh2);
    for (size_t i = 0; i < g_wins.size(); ++i) {
        auto& cw = g_wins[i];
        Rect  mini;
        waveview_map_window(tiles[cw.tile].x, tiles[cw.tile].y, tiles[cw.tile].w, tiles[cw.tile].h, ux, uy, uw2, uh2,
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
    for (size_t i = 0; i < g_wins.size(); ++i) {
        if (!ok[i])
            continue;
        auto&       cw     = g_wins[i];
        const float target = (cw.holdPreview || (ssize_t)i == swapIdx) ? 1.f : 0.f;
        if ((ssize_t)i == swapIdx) {
            // The half it keeps: split on the longer axis, away from the
            // cursor side — mirroring where dwindle puts the newcomer.
            const CBox& b = boxes[i];
            if (b.w >= b.h) {
                const bool left = g_dragCursor.x < b.x + b.w / 2.0;
                cw.previewBox   = left ? CBox{b.x + b.w / 2.0, b.y, b.w / 2.0, b.h}
                                       : CBox{b.x, b.y, b.w / 2.0, b.h};
            } else {
                const bool top = g_dragCursor.y < b.y + b.h / 2.0;
                cw.previewBox  = top ? CBox{b.x, b.y + b.h / 2.0, b.w, b.h / 2.0}
                                     : CBox{b.x, b.y, b.w, b.h / 2.0};
            }
        }
        cw.previewT += (target - cw.previewT) * std::min(1.0f, g_frameDt * 14.f);
        if (std::abs(cw.previewT - target) < 0.01f)
            cw.previewT = target;
        else
            previewMoving = true;
        if (cw.previewT > 0.001f && cw.previewBox.w > 0.0) {
            const double e = easeInOutCubic(cw.previewT);
            const CBox&  h = boxes[i];
            boxes[i]       = CBox{mix(h.x, cw.previewBox.x, e), mix(h.y, cw.previewBox.y, e),
                                  mix(h.w, cw.previewBox.w, e), mix(h.h, cw.previewBox.h, e)};
        }
    }
    if (previewMoving) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
    for (size_t i = 0; i < g_wins.size(); ++i) {
        auto& cw = g_wins[i];
        if (!ok[i])
            continue;
        const auto tex = cw.fb ? cw.fb->getTexture() : nullptr;
        if (!tex)
            continue;
        const CBox& box   = boxes[i];
        const int   round = (int)std::lround(DSN_WIN_ROUND * m->m_scale * p);
        const auto  w     = cw.win.lock();

        if (w && w == dragW)
            continue; // the dragged window is drawn last, under the cursor

        if ((w && w == hoverW) || (ssize_t)i == swapIdx)
            haloBorder(box, round); // ring: hover, or the live swap partner
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
            CBox b{g_dragCursor.x - g_dragGrab.x, g_dragCursor.y - g_dragGrab.y, cw.screen.w, cw.screen.h};
            const int round = (int)std::lround(std::min(b.w, b.h) * 0.06);
            haloBorder(b, round);
            drawTex(tex, b, round);
            break;
        }
    }
}

// While the overview is open, re-capture thumbnails on a timer so they stay live.
// Fires outside the render pass (so beginRender is safe) and re-arms itself.
static void onLiveTimer(SP<CEventLoopTimer> self, void*) {
    if (!g_active)
        return; // disarmed on close; don't re-arm
    // Never recapture mid-animation: snapshotting 18 workspaces stalls a
    // frame, which reads as a hitch in the page-flip / zoom glide. Poll
    // quickly until the motion settles, then catch up.
    if (g_scrollProg < 1.0f || g_anim != g_animTarget) {
        self->updateTimeout(std::chrono::milliseconds(50));
        return;
    }
    if (const auto m = g_captureMon.lock()) {
        captureWorkspaces(m);
        damageAll();
    }
    self->updateTimeout(REFRESH_MS);
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

// Cursor motion while open: update the hovered window, or the drag position —
// then SWALLOW the event. Cancelling is safe in this fork: PointerManager::move
// runs before the hook fires (verified in InputManager.cpp::onMouseMoved), so
// the sprite keeps moving; what cancelling stops is focus-follows-mouse and
// surface motion reaching the desktop underneath — which used to leak (windows
// refocused, the dock revealed, topbar pills lit while the overview was open).
// The first uncancelled motion after close re-focuses under the cursor.
static void onMouseMove(Vector2D, Event::SCallbackInfo& info) {
    if (!g_active || g_animTarget < 0.5f)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
    info.cancelled   = true;
    updateHoverAt(m, cursorDrawSpace(m));
}

// Begin the compositor's own drag for `dw` at GRAB time: the layout floats
// the window out and re-tiles the siblings immediately (the live thumbnails
// show it — no hole). The cursor warps to the window's desktop centre to
// seed the drag, then returns; our motion-swallow mutes the side effects.
static void beginRealDrag(PHLWINDOW dw) {
    if (g_dragReal || !dw || dw->isFullscreen())
        return;
    const CBox     wb = dw->getWindowMainSurfaceBox();
    const Vector2D home{wb.x + wb.w / 2.0, wb.y + wb.h / 2.0};
    const Vector2D saved = g_pInputManager->getMouseCoordsInternal();
    g_dragHomeCenter     = home;
    g_pCompositor->warpCursorTo(home, true);
    g_layoutManager->beginDragTarget(dw->layoutTarget(), MBIND_MOVE);
    g_layoutManager->moveMouse(home + Vector2D(3, 3)); // trip the drag threshold
    g_layoutManager->moveMouse(home);
    // Park the float far offscreen for the drag's duration: it floats at
    // its old spot on the REAL workspace, so the live captures were baking
    // its pixels into the re-tiled siblings' textures ("the grabbed
    // window's image gets printed on the reacting one"). The end-drag
    // recomputes position from the begin anchor, so parking is invisible
    // to the drop math; our cursor ghost is the only visual.
    g_layoutManager->setTargetGeom(CBox{-20000.0, -20000.0, wb.w, wb.h}, dw->layoutTarget());
    g_pCompositor->warpCursorTo(saved, true);
    g_dragReal = true;
    if (const auto m = g_captureMon.lock())
        captureWorkspaces(m); // show the re-tile right away
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
    g_dragReal           = false;
    const Vector2D dest  = at.value_or(g_dragHomeCenter);
    const Vector2D saved = g_pInputManager->getMouseCoordsInternal();
    g_pCompositor->warpCursorTo(dest, true);
    if (splitTarget)
        Desktop::focusState()->fullWindowFocus(splitTarget, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    g_layoutManager->moveMouse(dest);
    g_layoutManager->endDragTarget();
    g_pCompositor->warpCursorTo(saved, true);
}

// Shared by motion and grid-scroll: recompute hover/drag targets at cursor `c`
// (a scroll moves the tiles under a stationary cursor, so hover must follow).
static void updateHoverAt(PHLMONITOR m, const Vector2D& c) {
    // Any pending press (window or empty tile) that leaves the slop is a drag.
    if ((g_dragWin.lock() || g_pressTile >= 0) && !g_dragMoved && (c - g_pressPos).size() > CLICK_SLOP) {
        g_dragMoved = true;
        // The grab is real from frame one: the compositor pulls the window
        // out of the layout NOW, so the siblings re-tile live (the
        // thumbnails show it — no hole where the window was).
        if (const auto dw = g_dragWin.lock())
            beginRealDrag(dw);
    }

    if (g_dragWin.lock()) {
        g_dragCursor = c;
        damageAll();
        return;
    }

    // Hover: a window under the cursor, else an empty workspace tile (jump target).
    PHLWINDOWREF hov = winAt(c);
    int          tile = -1;
    if (!hov.lock()) {
        const int t = tileAt(m, c);
        if (t >= 0 && tileEmpty(t))
            tile = t;
    }
    if (hov.lock() != g_hoverWin.lock() || tile != g_hoverTile) {
        g_hoverWin  = hov;
        g_hoverTile = tile;
        damageAll();
    }
}

// Scroll while open flips between the two pages, clamped — never a loop.
// A mouse wheel flips per notch; a 2-finger touchpad scroll streams tiny
// deltas, so it accumulates to a travel threshold first. Swallowed either
// way so the desktop underneath never scrolls; the eased glide runs in
// onRender (dt-based).
static double           g_fingerAcc    = 0.0;
static constexpr double FINGER_FLIP_AT = 140.0; // accumulated px per page flip
static void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    if (!g_active || g_animTarget < 0.5f)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
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
    if (!g_active || g_animTarget < 0.5f || e.button != BTN_LEFT)
        return;
    const auto m = g_captureMon.lock();
    if (!m)
        return;
    info.cancelled = true;

    const Vector2D c = cursorDrawSpace(m);
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        g_pressPos  = c;
        g_dragMoved = false;
        g_pressTile = -1;
        if (const auto w = winAt(c).lock()) {
            g_dragWin    = w;
            g_dragCursor = c;
            // grab offset from the window's drawn top-left, so it tracks naturally
            for (auto& cw : g_wins)
                if (cw.win.lock() == w) {
                    g_dragGrab = Vector2D(c.x - cw.screen.x, c.y - cw.screen.y);
                    break;
                }
            damageAll();
        } else {
            const int t = tileAt(m, c);
            if (t >= 0 && tileEmpty(t))
                g_pressTile = t; // press landed on an empty workspace → candidate jump
        }
        return;
    }

    // Released: a click (never left the slop) jumps — to the window, or to an empty
    // workspace; a real drag drops the window onto the tile under the cursor.
    const auto dw        = g_dragWin.lock();
    const bool moved     = g_dragMoved;
    const int  pressTile = g_pressTile;
    g_dragWin.reset();
    g_dragMoved = false;
    g_pressTile = -1;
    if (dw && !moved) {
        jumpToWindow(dw); // click → switch to & focus that window, closing the overview
        return;
    }
    if (!dw && !moved && pressTile >= 0) {
        jumpTo(pressTile + 1); // click on an empty workspace → jump there, closing the overview
        return;
    }
    if (dw) {
        Rect      tiles[N_TILES];
        int       drop = -1;
        PHLWINDOW under;
        if (computeTiles(m, tiles) == N_TILES)
            for (int i = 0; i < N_TILES; ++i)
                if (CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}.containsPoint(c)) {
                    drop = i;
                    break;
                }
        if (drop >= 0) {
            // The drop point, mapped back into desktop logical coords
            // within the target view (inverse of the tile mapping).
            Rect   tiles2[N_TILES];
            double ux2, uy2, uw3, uh3;
            usableArea(m, ux2, uy2, uw3, uh3);
            const Rect&    t = tiles[drop];
            const Vector2D desk{ux2 + (c.x - t.x) * uw3 / t.w, uy2 + (c.y - t.y) * uh3 / t.h};
            (void)tiles2;
            if (dw->workspaceID() != drop + 1) {
                auto ws = g_pCompositor->getWorkspaceByID(drop + 1);
                if (!ws)
                    ws = g_pCompositor->createNewWorkspace(drop + 1, m->m_id);
                if (ws)
                    g_pCompositor->moveWindowToWorkspaceSafe(dw, ws);
            }
            // Finish the drag begun at grab: end it at the drop point —
            // the dwindle insert splits whatever sits under it, exactly
            // like releasing the drag on the real desktop. Pass the window
            // under the drop as the split target (see endRealDrag).
            for (auto it = g_wins.rbegin(); it != g_wins.rend(); ++it) {
                if (it->screen.w <= 0.0 || !it->screen.containsPoint(c))
                    continue;
                if (auto w2 = it->win.lock(); w2 && w2 != dw) {
                    under = w2;
                    break;
                }
            }
            endRealDrag(desk, under);
        } else {
            endRealDrag(std::nullopt); // dropped outside every view: go home
        }
        // DO NOT recapture until the landing spring rests: windows animate on a
        // ~480ms spring (hyprland.lua: windows speed 4.79) with a settle tail —
        // any earlier snapshot catches them mid-flight and blinks. The held
        // preview IS the landing shape, so stillness is correct meanwhile; the
        // dragged window.s stale mini goes away so nothing overlaps.
        for (auto& cw : g_wins)
            if (cw.win.lock() == dw && cw.fb)
                cw.fb->release();
        std::erase_if(g_wins, [&](const CapWin& cw) { return cw.win.lock() == dw; });
        if (g_liveTimer)
            g_liveTimer->updateTimeout(std::chrono::milliseconds(750));
        // HOLD the split target's preview until the recapture: its preview
        // half IS the post-drop truth — zeroing it snapped the sibling back
        // to full size for 200ms and then re-split it (two reactions for
        // one drop). Fresh captures reset the hold; the carried preview then
        // decays onto the (nearly identical) real geometry.
        for (auto& cw : g_wins) {
            if (under && cw.win.lock() == under)
                cw.holdPreview = true;
            else
                cw.previewT = 0.f;
        }
    }
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
        g_hoverTile = -1;
        g_pressTile = -1;
        if (g_liveTimer)
            g_liveTimer->updateTimeout(std::nullopt);
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

// Tell waverunner (the dock/topbar daemon) the overview state, so it
// conceals its surfaces while we own the screen. Fire-and-forget over its
// control socket on a detached thread; a dead daemon = nothing to conceal.
static void notifyWaverunner(bool on) {
    std::thread([on] {
        const char* rt = getenv("XDG_RUNTIME_DIR");
        if (!rt)
            return;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/waverunner.sock", rt);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            const char* msg = on ? "overview-on\n" : "overview-off\n";
            (void)!write(fd, msg, strlen(msg));
        }
        close(fd);
    }).detach();
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
    endRealDrag(std::nullopt); // never leave a real drag dangling
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
        endRealDrag(std::nullopt); // never leave a real drag dangling
        g_dragWin.reset();
        g_dragMoved = false;
    }
    g_animTarget = opening ? 1.0f : 0.0f;
    notifyWaverunner(opening);
    g_animLastT  = Time::steadyNow();
    if (opening) {
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
static void onSwipeBegin(IPointer::SSwipeBeginEvent e, Event::SCallbackInfo&) {
    g_swipeFingers = e.fingers;
    g_swipeAcc     = Vector2D(0.0, 0.0);
    g_swipeFired   = false;
}

// Accumulate the swipe; on a decisive 3-finger vertical move (once per
// gesture): swipe UP walks the same ladder as Super+R — open, then tour the
// other inhabited page, then close; swipe DOWN is Escape (immediate close,
// no touring). libinput reports fingers-up as negative dy.
static void onSwipeUpdate(IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
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

static void onSwipeEnd(IPointer::SSwipeEndEvent, Event::SCallbackInfo&) {
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
    HyprlandAPI::addNotification(handle, std::string("[waveview] loaded -- ") + waveview_hello(),
                                 CHyprColor(0.3, 1.0, 0.5, 1.0), 3000);
    return {"waveview", "Live 3x3 workspace overview (Rust brain + C++ shim)", "max", "0.2"};
}

APICALL EXPORT void PLUGIN_EXIT() {
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
