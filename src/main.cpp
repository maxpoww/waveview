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
int         waveview_workspace_tiles(double mw, double mh, Rect* out);
int         waveview_tile_for_workspace(int64_t ws_id);
void        waveview_map_window(double tx, double ty, double tw, double th, double mon_x, double mon_y, double mon_w,
                                double mon_h, double wx, double wy, double ww, double wh, Rect* out);
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
static SP<CEventLoopTimer> g_liveTimer; // re-arms every REFRESH_MS while open to keep thumbnails live

// evdev keycodes as delivered by the input event (xkb code = evdev + 8). Digit
// row is contiguous: KEY_1..KEY_9 = 2..10, so workspace N is keycode N + 1.
static constexpr uint32_t EVDEV_ESC = 1;
static constexpr uint32_t EVDEV_1   = 2;
static constexpr uint32_t EVDEV_9   = 10;
static constexpr uint32_t EVDEV_Q   = 16; // KEY_Q — close the hovered window

static constexpr auto REFRESH_MS = std::chrono::milliseconds(150);

// One captured thumbnail per grid slot (workspaces 1..9), plus the monitor the
// snapshots belong to — thumbnails are only valid on that monitor. These full
// workspace snapshots are no longer drawn directly; they're the SOURCE pixels we
// crop individual windows out of (see captureWindows).
static SP<Render::IFramebuffer> g_fbs[9];
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

// Defined further down; used by the pointer handlers above their definitions.
static void jumpTo(int wsId);
static void jumpToWindow(PHLWINDOW w);

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
    std::vector<std::pair<PHLWINDOWREF, CBox>> prevBoxes;
    prevBoxes.reserve(g_wins.size());
    for (auto& cw : g_wins) {
        prevBoxes.emplace_back(cw.win, cw.screen);
        if (cw.fb) // free last cycle's textures before rebuilding
            cw.fb->release();
    }
    g_wins.clear();

    const double scale = m->m_scale;
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
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

        for (auto& pb : prevBoxes) // carry the hit-box forward if we saw this window last cycle
            if (pb.first.lock() == w) {
                cw.screen = pb.second;
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
static void captureWorkspaces(PHLMONITOR m) {
    if (!m)
        return;

    Rect tiles[9];
    if (waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, tiles) < 9)
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

    for (int i = 0; i < 9; ++i) {
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
    for (int i = 0; i < 9; ++i)
        if (const auto ws = g_pCompositor->getWorkspaceByID(i + 1))
            g_pHyprRenderer->sendFrameEventsToWorkspace(m, ws, Time::steadyNow());

    g_captureMon = m;
}

// Schematic fallback: dark tiles + each live window mapped into its workspace
// tile (focused window highlighted). Used on monitors we haven't captured
// thumbnails for. Tiles are pixel-space; window boxes are logical — the brain
// reconciles the two.
static void drawSchematic(PHLMONITOR m, const Rect tiles[9]) {
    for (int i = 0; i < 9; ++i)
        renderRect(CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}, CHyprColor(0.0, 0.0, 0.0, 0.35));

    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
        const int ti = waveview_tile_for_workspace(w->workspaceID());
        if (ti < 0)
            continue;

        const CBox wb = w->getWindowMainSurfaceBox();
        Rect       mini;
        waveview_map_window(tiles[ti].x, tiles[ti].y, tiles[ti].w, tiles[ti].h, m->m_position.x, m->m_position.y,
                            m->m_size.x, m->m_size.y, wb.x, wb.y, wb.w, wb.h, &mini);
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

    Rect tiles[9];
    if (waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, tiles) < 9)
        return;

    // No captures for this monitor: fall back to the flat schematic.
    if (g_captureMon.lock() != m || !g_bgFB) {
        drawSchematic(m, tiles);
        return;
    }

    // Zoom transform: scale about zoomTile's top-left so at p=0 that tile becomes
    // the full monitor; mix an arbitrary rest-rect toward its zoomed rect by p.
    const double mw = m->m_transformedSize.x;
    const Rect&  az = tiles[zoomTile];
    const double s0 = mw / az.w;
    auto         dispRect = [&](const CBox& r) -> CBox {
        const double zx = (r.x - az.x) * s0, zy = (r.y - az.y) * s0;
        const double zw = r.w * s0, zh = r.h * s0;
        return CBox{mix(zx, r.x, p), mix(zy, r.y, p), mix(zw, r.w, p), mix(zh, r.h, p)};
    };

    // The single wallpaper backdrop, filling the whole monitor.
    if (const auto bg = g_bgFB->getTexture()) {
        CTexPassElement::SRenderData td;
        td.tex = bg;
        td.box = CBox{0.0, 0.0, m->m_transformedSize.x, m->m_transformedSize.y};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
    }

    auto drawTex = [&](SP<Render::ITexture> tex, const CBox& b, int round) {
        CTexPassElement::SRenderData td;
        td.tex           = tex;
        td.box           = b;
        td.round         = round;
        td.roundingPower = 2.0f;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(td));
    };
    // A rounded outline (real hollow border, not four rects) for the drop-target
    // tile. Corner radius scales with the tile so it matches the rounded windows.
    auto drawBorder = [&](const CBox& b, const CHyprColor& c, double bw) {
        CBorderPassElement::SBorderData bd;
        bd.box           = b;
        bd.grad1         = Config::CGradientValueData(c);
        bd.borderSize    = std::max(1, (int)std::lround(bw));
        bd.round         = (int)std::lround(std::min(b.w, b.h) * 0.04);
        bd.roundingPower = 2.0f;
        bd.a             = 1.0f; // alpha carried by the gradient color; don't double-dim
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(bd));
    };
    // A rounded border, drawn as a filled rounded rect *behind* the window: the
    // texture (rounded to `round`) covers the interior, leaving a rounded ring.
    const CHyprColor kHoverCol(0.40, 0.70, 1.0, 1.0);
    auto             haloBorder = [&](const CBox& box, int round) {
        const double bw = std::max(2.0, std::min(box.w, box.h) * 0.016);
        renderRect(CBox{box.x - bw, box.y - bw, box.w + 2.0 * bw, box.h + 2.0 * bw}, kHoverCol,
                               round + (int)std::lround(bw));
    };

    const auto hoverW = g_hoverWin.lock();
    // Only treat it as a drag once the cursor has left the click slop — before that
    // a press is still a potential click, so the window stays put in its tile.
    const auto dragW  = g_dragMoved ? g_dragWin.lock() : PHLWINDOW{};

    // While dragging, outline the tile the cursor is over — the drop target.
    if (dragW) {
        for (int i = 0; i < 9; ++i) {
            const CBox t = dispRect(CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h});
            if (t.containsPoint(g_dragCursor))
                drawBorder(t, CHyprColor(0.40, 0.70, 1.0, 0.55), std::max(2.0, t.h * 0.008));
        }
    }

    // Hovered empty workspace: outline it as a click-to-jump target.
    if (!dragW && g_hoverTile >= 0) {
        const CBox t = dispRect(CBox{tiles[g_hoverTile].x, tiles[g_hoverTile].y, tiles[g_hoverTile].w, tiles[g_hoverTile].h});
        drawBorder(t, kHoverCol, std::max(2.0, t.h * 0.01));
    }

    // Windows shrink toward their centers and round as the grid zooms out — so at
    // p=0 (zoomed fully into the active workspace) they're pixel-exact and the
    // close animation lands seamlessly on the real desktop.
    const double shrink = mix(1.0, 0.975, p);
    for (auto& cw : g_wins) {
        const auto tex = cw.fb ? cw.fb->getTexture() : nullptr;
        if (!tex)
            continue;

        Rect mini;
        waveview_map_window(tiles[cw.tile].x, tiles[cw.tile].y, tiles[cw.tile].w, tiles[cw.tile].h, m->m_position.x,
                            m->m_position.y, m->m_size.x, m->m_size.y, cw.logical.x, cw.logical.y, cw.logical.w,
                            cw.logical.h, &mini);
        if (mini.w <= 0.0 || mini.h <= 0.0) {
            cw.screen = CBox{};
            continue;
        }

        const double cx = mini.x + mini.w / 2.0, cy = mini.y + mini.h / 2.0;
        const CBox   box = dispRect(CBox{cx - mini.w * shrink / 2.0, cy - mini.h * shrink / 2.0, mini.w * shrink, mini.h * shrink});
        cw.screen        = box; // remembered for pointer hit-testing
        const int round  = (int)std::lround(std::min(box.w, box.h) * 0.06 * p);
        const auto w     = cw.win.lock();

        if (w && w == dragW)
            continue; // the dragged window is drawn last, under the cursor

        if (w && w == hoverW)
            haloBorder(box, round); // rounded ring behind the window
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
    Rect tiles[9];
    if (waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, tiles) != 9)
        return -1;
    for (int i = 0; i < 9; ++i)
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
    const Vector2D c = cursorDrawSpace(m);

    // Any pending press (window or empty tile) that leaves the slop is a drag.
    if ((g_dragWin.lock() || g_pressTile >= 0) && !g_dragMoved && (c - g_pressPos).size() > CLICK_SLOP)
        g_dragMoved = true;

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
        Rect tiles[9];
        int  drop = -1;
        if (waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, tiles) == 9)
            for (int i = 0; i < 9; ++i)
                if (CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}.containsPoint(c)) {
                    drop = i;
                    break;
                }
        if (drop >= 0 && dw->workspaceID() != drop + 1) {
            auto ws = g_pCompositor->getWorkspaceByID(drop + 1);
            if (!ws)
                ws = g_pCompositor->createNewWorkspace(drop + 1, m->m_id);
            if (ws)
                g_pCompositor->moveWindowToWorkspaceSafe(dw, ws);
        }
        captureWorkspaces(m); // reflect the move immediately
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
    const float step = dt / ANIM_SECONDS;
    if (g_anim < g_animTarget)
        g_anim = std::min(g_animTarget, g_anim + step);
    else if (g_anim > g_animTarget)
        g_anim = std::max(g_animTarget, g_anim - step);

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

static void toggle() {
    const bool opening = g_animTarget < 0.5f; // currently closed/closing -> open
    g_animTarget       = opening ? 1.0f : 0.0f;
    notifyWaverunner(opening);
    g_animLastT        = Time::steadyNow();
    if (opening) {
        g_active         = true;
        const auto m     = g_pCompositor->getMonitorFromCursor();
        const int  at    = waveview_tile_for_workspace(m ? m->activeWorkspaceID() : -1);
        g_zoomTile       = at >= 0 ? at : 0;
        captureWorkspaces(m); // snapshot on open, outside the render pass
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

// Accumulate the swipe; on a decisive 3-finger vertical move, toggle the overview
// once (up opens, down closes) and latch g_swipeFired so the rest of the gesture
// is inert. libinput reports fingers-up as negative dy.
static void onSwipeUpdate(IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) {
    if (g_swipeFingers != 3 || g_swipeFired)
        return;
    g_swipeAcc += e.delta;
    if (std::abs(g_swipeAcc.y) < SWIPE_TRIGGER || std::abs(g_swipeAcc.x) > std::abs(g_swipeAcc.y))
        return; // not yet decisive, or dominantly horizontal

    const bool up     = g_swipeAcc.y < 0.0;
    const bool opened = g_animTarget > 0.5f; // currently open/opening
    if (up != opened)                        // up & closed -> open; down & open -> close
        toggle();
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

// While the overview is open, a digit 1..9 jumps to that workspace and Escape
// closes it; both are swallowed (info.cancelled) so they never reach the focused
// window. All other keys pass through untouched. Closed: we're transparent.
static void onKey(IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
    if (!g_active || g_animTarget < 0.5f) // only intercept while open (not mid-close)
        return;

    if (e.keycode >= EVDEV_1 && e.keycode <= EVDEV_9) {
        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            jumpTo(static_cast<int>(e.keycode - EVDEV_1 + 1));
        info.cancelled = true; // swallow press AND release so the digit never types into the app
        return;
    }

    if (e.keycode == EVDEV_Q) {
        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            if (const auto w = g_hoverWin.lock())
                g_pXWaylandManager->sendCloseWindow(w); // live timer re-captures, so it vanishes from the grid
        info.cancelled = true; // swallow press AND release so 'q' never types into the app
        return;
    }

    if (e.keycode == EVDEV_ESC) {
        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            toggle(); // close without jumping
        info.cancelled = true;
    }
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
