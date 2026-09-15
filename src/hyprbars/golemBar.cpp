// Golem's titlebar for floating windows.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md for the upstream tag and every change.
//
// What is kept is everything that knows Hyprland: reserving the strip at the
// window's top edge, the stencil that makes the bar's corners follow the
// window's rounding, drawing through a render-pass element so blur and
// occlusion behave, damage, the input validation, and the drag. What is gone is
// the configuration layer — the look below is constants, and the buttons are
// two fixed ones.

#include "golemBar.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "barsGlobals.hpp"
#include "BarPassElement.hpp"
#include "bars.hpp"

#include <climits>
#include <cmath>

using namespace Render::GL;

// ───────────────────────────── Golem's palette ─────────────────────────────
//
// The bar sits on the window, so it reads as part of it: a near-black strip
// with the shell's own ink, not a system-blue caption bar.
/// The bar wears the window's **border** colour — its first gradient stop, the
/// one at the top, which is the edge the bar is continuous with (Max's call,
/// 2026-09-13).
///
/// So the bar is not a strip laid on the frame, it *is* the frame, widened at
/// the top to hold a title. It follows focus for free: the compositor already
/// animates this colour between the active and inactive border, so the bar
/// lights up and dims with the window it belongs to, and any colour the daemon
/// gives the borders (the screen-following colour pass) arrives here too.
///
/// Falls back to Golem's near-black only if a window somehow has no border
/// colour at all.
static CHyprColor barColor(PHLWINDOW w) {
    if (w && !w->m_realBorderColor.m_colors.empty())
        return w->m_realBorderColor.m_colors.front();
    return CHyprColor{0xEE0D0F14ULL};
}

/// Ink that stays readable on whatever the border turns out to be: Golem's
/// borders run from a light peach (focused) to a dark brown (not), and one fixed
/// text colour cannot serve both.
static CHyprColor inkOn(const CHyprColor& bg) {
    const float L = 0.2126f * bg.r + 0.7152f * bg.g + 0.0722f * bg.b;
    return L > 0.5f ? CHyprColor{0xFF14171CULL} : CHyprColor{0xFFE8E6E3ULL};
}

CGolemBar::CGolemBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (PMONITOR)
        PMONITOR->m_scheduledRecalc = true;

    m_hidden = !pWindow->m_isFloating;

    m_pMouseButtonCallback =
        Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo& info) { onMouseMove(c); });

    g_pAnimationManager->createAnimation(barColor(pWindow), m_cRealBarColor, Config::animationTree()->getAnimationPropertyConfig("border"), pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });
}

CGolemBar::~CGolemBar() {
    // Guarded, because a bar can outlive the state that lists it: unloading the
    // plugin runs PLUGIN_EXIT first and destroys the decorations after, so this
    // runs during teardown. The first cut dereferenced the state here and took
    // the session down with it (SEGV, 2026-09-13 — `unloadPlugin` →
    // `removeWindowDecoration` → here).
    if (g_pBarsState)
        std::erase(g_pBarsState->bars, m_self);

    // Drop the animated colour too. Its update callback captures `this` and
    // calls back into the bar; letting it outlive us is the same shape of bug
    // as the one above, just with a longer fuse.
    m_cRealBarColor.reset();
}

// The bar belongs to windows that have LEFT the layout. Upstream drove this
// from a window rule; asking the window is the same mechanism with the one
// condition Golem actually wants, and it costs a bool compare per update.
void CGolemBar::syncHidden() {
    if (!validMapped(m_pWindow))
        return;

    const bool WANT = !m_pWindow->m_isFloating;
    if (WANT == m_hidden)
        return;

    m_hidden = WANT;
    // A bar arriving or leaving changes how much room the window has, so the
    // layout has to hear about it — without this the window keeps the reserved
    // strip it no longer has (or loses one it just gained).
    //
    // SCHEDULED, not done here: this is reached from `draw()`, inside the
    // render pass, and repositioning walks the very decoration list the pass is
    // iterating. The constructor announces a new bar the same way, and the
    // recalc lands a frame later — which nobody can see, and which cannot
    // reenter. (There is no float-change signal to hook instead: neither the
    // event socket nor the internal bus has one — checked both, 2026-09-13.)
    if (const auto PMONITOR = m_pWindow->m_monitor.lock())
        PMONITOR->m_scheduledRecalc = true;
    damageEntire();
}

SDecorationPositioningInfo CGolemBar::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.policy = m_hidden ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges  = DECORATION_EDGE_TOP;
    // OUTSIDE the border: the frame stays around the window, and the bar sits
    // on top of it. It does not read as a detached strip because it is painted
    // the border's own colour (see `barColor`) — so the two are one piece of
    // frame, rather than one wrapping the other. (Max's call, 2026-09-13, over
    // the alternative of letting the border wrap the bar.)
    info.priority       = 5000;
    info.reserved       = true;
    info.desiredExtents = {{0, m_hidden ? 0 : GOLEM_BAR_HEIGHT}, {0, 0}};
    return info;
}

void CGolemBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CGolemBar::getDisplayName() {
    return "Golembar";
}

bool CGolemBar::inputIsValid() {
    // The window first, and before anything dereferences it: these listeners
    // fire for every pointer event on every window's bar, so one bar outliving
    // its window by a beat would take the session down on the next mouse move.
    // (Upstream checks its config here and the window not at all.)
    if (m_hidden || !validMapped(m_pWindow))
        return false;

    if (!m_pWindow->m_workspace || !m_pWindow->m_workspace->isVisible() || !g_pInputManager->m_exclusiveLSes.empty() ||
        (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource())))
        return false;

    const auto WINDOWATCURSOR = g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                                     Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto       focusState = Desktop::focusState();
    auto       window     = focusState->window();
    auto       monitor    = focusState->monitor();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    // A layer surface over the pointer owns the click — the dock, an OPTIONS
    // box, anything of ours that happens to be above this window.
    auto     PMONITOR     = monitor;
    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    if (!PMONITOR)
        return false;

    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords,
                                        &foundSurface);
    if (foundSurface)
        return false;

    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords,
                                        &foundSurface);
    if (foundSurface)
        return false;

    return true;
}

void CGolemBar::onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (!inputIsValid())
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        handleUpEvent(info);
        return;
    }

    handleDownEvent(info);
}

void CGolemBar::onMouseMove(Vector2D coords) {
    damageOnButtonHover();

    if (!m_bDragPending || !validMapped(m_pWindow))
        return;

    m_bDragPending = false;
    handleMovement();
}

void CGolemBar::handleDownEvent(Event::SCallbackInfo& info) {
    const auto PWINDOW = m_pWindow.lock();
    const auto COORDS  = cursorRelativeToBar();

    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, GOLEM_BAR_HEIGHT - 1)) {
        if (m_bDraggingThis) {
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Log::logger->log(Log::DEBUG, "[golembar] drag ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        return;
    }

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        g_pCompositor->changeWindowZOrder(PWINDOW, true);

    info.cancelled   = true;
    m_bCancelledDown = true;

    if (doButtonPress(COORDS))
        return;

    m_bDragPending = true;
}

void CGolemBar::handleUpEvent(Event::SCallbackInfo& info) {
    if (m_pWindow.lock() != Desktop::focusState()->window())
        return;

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        Log::logger->log(Log::DEBUG, "[golembar] drag ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;
}

void CGolemBar::handleMovement() {
    g_pKeybindManager->changeMouseBindMode(MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[golembar] drag started on {:x}", (uintptr_t)m_pWindow.lock().get());
}

bool CGolemBar::doButtonPress(Vector2D COORDS) {
    for (size_t i = 0; i < g_pBarsState->buttons.size(); ++i) {
        auto&      b          = g_pBarsState->buttons[i];
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, GOLEM_BAR_HEIGHT};
        Vector2D   currentPos = Vector2D{golemButtonX(i, 1.F), golemButtonY(BARBUF.y, borderBelow(), GOLEM_BUTTON_SIZE)}.floor();

        if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING, currentPos.y + GOLEM_BUTTON_SIZE)) {
            const auto PWINDOW = m_pWindow.lock();
            switch (b.action) {
                // The window's own close, not a dispatcher on "activewindow":
                // the press focuses this window first, but saying which window
                // outright cannot be raced by anything that focuses in between.
                case GOLEM_BAR_CLOSE: PWINDOW->sendClose(); break;
                // Back into the layout — through the DAEMON, not the
                // compositor. Golem's tiled state is more than "not floating":
                // a space showing one tile shows it pseudo, and the bar's state
                // pill has to hear about the change. The compositor's own float
                // toggle does neither, and using it left windows plainly tiled
                // where the rule promised pseudo (2026-09-13). The press has
                // already focused this window, which is the one the verb acts
                // on.
                case GOLEM_BAR_TILE: sendWaverunner("window-mode tiled\n"); break;
            }
            return true;
        }

    }
    return false;
}

void CGolemBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const int   scaledSize        = std::round(GOLEM_TITLE_SIZE * scale);
    const auto  scaledButtonsSize = golemButtonsWidth(g_pBarsState->buttons.size()) * scale;
    const auto  scaledBarPadding  = GOLEM_BAR_PADDING * scale;
    // Room for the title: the bar, less the buttons standing at its LEFT (their
    // own inset included) and the air at its right end.
    const int   paddingTotal = GOLEM_BUTTONS_LEFT * scale + scaledButtonsSize + scaledBarPadding;
    const int   maxWidth     = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

    if (m_szLastTitle.empty() || maxWidth < 1) {
        m_pTextTex = nullptr;
        return;
    }

    // `m_lastInk` is set by the caller, which is also what decides when this
    // has to run again.
    m_pTextTex = g_pHyprRenderer->renderText(m_szLastTitle, m_lastInk, scaledSize, false, GOLEM_TITLE_FONT, maxWidth);
}

size_t CGolemBar::getVisibleButtonCount(const Vector2D& bufferSize, const float scale) {
    float  availableSpace = bufferSize.x - (GOLEM_BUTTONS_LEFT + GOLEM_BAR_PADDING) * scale;
    size_t count          = 0;

    for (size_t i = 0; i < g_pBarsState->buttons.size(); ++i) {
        const float buttonSpace = (GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CGolemBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const auto visibleCount = getVisibleButtonCount(Vector2D{barBox->w, barBox->h}, scale);
    const auto BORDER       = borderBelow();

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pBarsState->buttons[i];
        const auto scaledButtonSize = GOLEM_BUTTON_SIZE * scale;

        auto       color = button.bgcol;
        color.a *= a;

        CBox buttonBox = {barBox->x + golemButtonX(i, scale), barBox->y + golemButtonY(barBox->h, BORDER * scale, scaledButtonSize), scaledButtonSize, scaledButtonSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(buttonBox, color, {.round = static_cast<int>(std::round(scaledButtonSize / 2.0)), .roundingPower = 2.F});
    }
}

void CGolemBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const auto visibleCount = getVisibleButtonCount(Vector2D{barBox->w, barBox->h}, scale);
    const auto COORDS       = cursorRelativeToBar();
    const auto BORDER       = borderBelow();

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pBarsState->buttons[i];
        const auto scaledButtonSize = GOLEM_BUTTON_SIZE * scale;

        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, GOLEM_BAR_HEIGHT};
        Vector2D   currentPos = Vector2D{golemButtonX(i, 1.F), golemButtonY(BARBUF.y, borderBelow(), GOLEM_BUTTON_SIZE)}.floor();
        bool       hovering = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING, currentPos.y + GOLEM_BUTTON_SIZE);

        if ((!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty())
            button.iconTex = g_pHyprRenderer->renderText(button.icon, button.fgcol, std::round(GOLEM_BUTTON_SIZE * 0.62 * scale), false, GOLEM_TITLE_FONT, scaledButtonSize);

        if (!button.iconTex || button.iconTex->m_texID == 0)
            continue;

        // Centred on the button it belongs to, which is what the shared
        // `golemButtonX` places.
        const auto iconX = barBox->x + golemButtonX(i, scale) + (scaledButtonSize - button.iconTex->m_size.x) / 2.0;
        // Centred on the BUTTON, which is what `golemButtonY` places — not on
        // the bar, or the glyph floats above the disc it belongs to.
        const auto iconY =
            barBox->y + golemButtonY(barBox->height, BORDER * scale, scaledButtonSize) + (scaledButtonSize - button.iconTex->m_size.y) / 2.0;
        CBox       pos   = {iconX, iconY, button.iconTex->m_size.x, button.iconTex->m_size.y};

        g_pHyprOpenGL->renderTexture(button.iconTex, pos, {.a = a});

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit) {
            m_iButtonHoverState ^= (1 << i);
            damageEntire();
        }
    }
}

void CGolemBar::draw(PHLMONITOR pMonitor, const float& a) {
    syncHidden();

    if (m_hidden || !validMapped(m_pWindow))
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

void CGolemBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto PWINDOW = m_pWindow.lock();

    // Retargeted every frame: the border colour is itself animated by the
    // compositor (focus) and rewritten by the daemon (the colour pass), so the
    // bar has to follow it rather than sample it once.
    if (barColor(PWINDOW) != m_cRealBarColor->goal())
        *m_cRealBarColor = barColor(PWINDOW);

    CHyprColor color = m_cRealBarColor->value();
    color.a *= a;

    // The window's rounding plus its border, since the bar sits outside the
    // frame and has to curve around it.
    const auto ROUNDING = PWINDOW->rounding() + PWINDOW->getRealBorderSize();
    // The -2 is upstream's: without it the corners read badly against the gaps.
    const int  scaledRounding = ROUNDING > 0 ? static_cast<int>(std::round(ROUNDING * pMonitor->m_scale - 2)) : 0;

    m_seExtents = {{0, GOLEM_BAR_HEIGHT}, {}};

    const auto DECOBOX = assignedBoxGlobal();
    const auto BARBUF  = DECOBOX.size() * pMonitor->m_scale;

    // The bar is drawn TALLER than it is — `ROUNDING * 3` past its bottom edge —
    // because a rounded rect would otherwise curve away at the bottom, where
    // the bar should meet the window square. The overhang is then clipped off.
    CBox       titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w, DECOBOX.h + ROUNDING * 3};
    // What may actually be painted: the bar's own rectangle, nothing below it.
    CBox       clipBox     = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w, DECOBOX.h};

    titleBarBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();
    clipBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
        return;

    // ⭐ THE OVERHANG IS CLIPPED BY **DAMAGE**, AND ONLY BY DAMAGE.
    //
    // `renderRect` scissors the GL state itself, once per rect of the damage
    // region it is handed — so any scissor set *around* the call is overwritten
    // before a single pixel lands. Two earlier cuts of this bar learned that the
    // hard way: upstream's GL stencil, then our own `g_pHyprOpenGL->scissor()`
    // (2026-09-13). Both looked right and both were discarded, which is why the
    // overhang kept escaping — as a one-frame flicker while it was 45px, and,
    // once Golem's floats reported a 72px rounding, as the permanent slab of
    // border colour standing out below and around every floating window that
    // moved or resized (Max, 2026-09-15: *"the rectangle behind the floating
    // windows"*). A static window looked fine only because its damage was
    // bar-shaped already; the moment it moved, damage covered everything.
    //
    // Handing `renderRect` a damage region of our own is the one clip it cannot
    // throw away: it *is* the thing it scissors with. The region is the bar's
    // own box, so the overhang has nowhere to land whatever the frame's damage
    // looks like. Painting the whole strip rather than just the damaged part of
    // it is idempotent: the bar's pixels at a given box are the same every frame.
    CRegion barDamage{clipBox};

    // ⭐ PLUS THE TWO WEDGES AT THE SEAM (Max, 2026-09-15, pointing at a zoom of
    // the corner: *"i need to fill up the wedge between the bar and the border
    // on floating windows"*). The window rounds its own top corners under the
    // bar, so between the bar's straight bottom edge and the border's outer
    // curve there is a wedge of bare desktop at each end.
    //
    // The fill is the SAME rect in the SAME draw, just let past the seam — so
    // its colour, alpha and focus animation are the bar's by construction, its
    // outer edge is the bar's own side continued (the slab's sides are straight
    // down here; its bottom rounding is 2·R further down and never gets in),
    // and there is no second shape to seam against.
    //
    // ⚠️ THE REGION HUGS THE CURVE — it is NOT two solid corner boxes. Window
    // content can be TRANSLUCENT (a terminal's alpha, an opacity rule), and
    // anything painted under it shows through: solid boxes ghosted through the
    // content as a lighter square in each top corner (Max, 2026-09-15, zoom in
    // hand: *"there is a square on each side, inside the content"*). So the
    // region is a staircase of row-bands tracing the border's OUTER curve —
    // Hyprland's own corner shape, radius `ROUNDING`, exponent
    // `roundingPower()` — each band as wide as the curve's inset at its top
    // row, plus 2px that tuck under the opaque border. The paint therefore
    // stops at the border band and NOTHING lies under the content.
    //
    // The staircase's jagged inner edge never shows: it is under the border,
    // and the border draws over it. It runs to the tangent depth `R`, where
    // the wedge closes on its own — no bottom cut — and is clamped to half the
    // window's height and half the bar's width so it can never reach a short
    // window's bottom corners: the slab standing outside the window is the
    // failure this file has already had twice today.
    {
        const double P    = std::max(2.0, static_cast<double>(PWINDOW->roundingPower()));
        const double R    = ROUNDING * pMonitor->m_scale;
        const double seam = clipBox.y + clipBox.h;
        const double HMAX = std::min(R, PWINDOW->m_realSize->value().y * pMonitor->m_scale / 2.0);
        const double WMAX = clipBox.w / 2.0;
        const auto   inset = [&](double d) { // curve's distance in from the side, at depth d below the seam
            const double t = 1.0 - d / R;
            return R - R * std::pow(1.0 - std::pow(t, P), 1.0 / P);
        };
        double d = 0.0;
        while (d < HMAX && R > 0.0) {
            const double w = std::min(inset(d) + 2.0, WMAX);
            if (w <= 0.5)
                break;
            // One band per whole pixel the curve moves in by — ~30 bands, not R.
            double d2 = d + 1.0;
            while (d2 < HMAX && inset(d) - inset(d2) <= 1.0)
                d2 += 1.0;
            barDamage.add(CBox{clipBox.x, seam + d, w, d2 - d});
            barDamage.add(CBox{clipBox.x + clipBox.w - w, seam + d, w, d2 - d});
            d = d2;
        }
    }

    // Rounded on all four corners and clipped at the bar's bottom edge, which
    // leaves exactly a titlebar: curved on top, square where it meets the
    // window.
    g_pHyprOpenGL->renderRect(titleBarBox, color, {.damage = &barDamage, .round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    if (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || inkOn(color) != m_lastInk) {
        m_szLastTitle = PWINDOW->m_title;
        m_lastInk     = inkOn(color);
        renderBarTitle(BARBUF, pMonitor->m_scale);
    }

    // Truncated to whole pixels (upstream's), stated as doubles so the box's
    // own type does not have to narrow them.
    CBox textBox = {titleBarBox.x, titleBarBox.y, static_cast<double>(static_cast<int>(BARBUF.x)), static_cast<double>(static_cast<int>(BARBUF.y))};
    if (m_pTextTex) {
        // LEFT, at the bar's own padding — the arrangement a titlebar has on
        // every desktop these bars exist to feel familiar to, and the one that
        // is honest about the texture: `renderText` pads it out to the width it
        // was given and sets the glyphs at the left, so centring the texture
        // would leave the words looking arbitrarily off-centre (measured — the
        // title sat ~360px left of the middle of its own bar).
        // After the buttons, which stand at the left — the title still starts
        // at the bar's own padding, that padding just begins where the button
        // row ends (Max, 2026-09-13).
        const auto xOffset =
            std::round((GOLEM_BUTTONS_LEFT + golemButtonsWidth(g_pBarsState->buttons.size())) * pMonitor->m_scale);
        const auto yOffset  = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
        CBox       titleBox = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        // Same damage clip as the bar: a title is padded out to the width it was
        // given, and a window narrow enough for that padding to reach past its
        // own edge would otherwise trail glyphs onto the desktop.
        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.damage = &barDamage, .a = a});
    }

    renderBarButtons(&textBox, pMonitor->m_scale, a);
    m_bButtonsDirty = false;

    renderBarButtonsText(&textBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
}

eDecorationType CGolemBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGolemBar::updateWindow(PHLWINDOW pWindow) {
    // The one place a float/tile flip reliably reaches us.
    syncHidden();
    damageEntire();
}

void CGolemBar::damageEntire() {
    // The strip PLUS the notch fill below it (`renderPass`): the fill wears the
    // animated bar colour, so a focus flip has to redraw it too — damaged as
    // only the strip, the notches keep the old colour until the window next
    // moves. Damage is not paint; the window simply redraws over most of it.
    CBox box = assignedBoxGlobal();
    box.h += notchDepth();
    g_pHyprRenderer->damageBox(box);
}

// How far below the seam the notch fill reaches: the corner's full span, where
// the window's outer curve meets its side. In logical px, like the boxes here.
double CGolemBar::notchDepth() {
    const auto PWINDOW = m_pWindow.lock();
    return PWINDOW ? PWINDOW->rounding() + PWINDOW->getRealBorderSize() : 0.0;
}

Vector2D CGolemBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

eDecorationLayer CGolemBar::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CGolemBar::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
}

CBox CGolemBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CGolemBar::getOwner() {
    return m_pWindow.lock();
}

/// The window's top border, in logical px — the strip painted the bar's own
/// colour directly below it, which the buttons centre against (see
/// [`golemButtonY`]). Zero if the window has gone.
float CGolemBar::borderBelow() {
    const auto PWINDOW = m_pWindow.lock();
    return PWINDOW ? static_cast<float>(PWINDOW->getRealBorderSize()) : 0.F;
}

void CGolemBar::damageOnButtonHover() {
    const auto COORDS = cursorRelativeToBar();

    for (size_t i = 0; i < g_pBarsState->buttons.size(); ++i) {
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, GOLEM_BAR_HEIGHT};
        Vector2D   currentPos = Vector2D{golemButtonX(i, 1.F), golemButtonY(BARBUF.y, borderBelow(), GOLEM_BUTTON_SIZE)}.floor();

        bool       hover = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING, currentPos.y + GOLEM_BUTTON_SIZE);

        if (hover != m_bButtonHovered) {
            m_bButtonHovered = hover;
            damageEntire();
        }
    }
}
