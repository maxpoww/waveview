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
#include <hyprland/src/managers/CursorManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
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
#include <algorithm>
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
    // Hand the window its own border colours back, or a float that outlives
    // this plugin keeps a tint nothing owns any more. Unset only — NOT
    // `updateDecorationValues`: teardown destroys decorations after
    // PLUGIN_EXIT, and reaching back into the window from here is the
    // re-entrancy that took the session down on 2026-09-13. Everything
    // re-renders as the library unmaps.
    if (m_tinted && validMapped(m_pWindow)) {
        const auto PWINDOW = m_pWindow.lock();
        PWINDOW->m_ruleApplicator->activeBorderColor().unset(Desktop::Types::PRIORITY_SET_PROP);
        PWINDOW->m_ruleApplicator->inactiveBorderColor().unset(Desktop::Types::PRIORITY_SET_PROP);
        PWINDOW->m_ruleApplicator->borderSize().unset(Desktop::Types::PRIORITY_SET_PROP);
        m_tinted = false;
    }

    // If this bar was holding the grab hand, the pointer has to get its arrow
    // back from somewhere — nothing else knows to.
    releaseCursorIfOwner();

    if (g_pBarsState)
        std::erase(g_pBarsState->bars, m_self);

    // Drop the animated colour too. Its update callback captures `this` and
    // calls back into the bar; letting it outlive us is the same shape of bug
    // as the one above, just with a longer fuse.
    m_cRealBarColor.reset();
}

/// The compositor's LIVE border gradient — the very object the desktop draws
/// its own borders from, so the tint inherits the colours, stop count and
/// angle for free, including whatever the daemon's colour pass pushed a moment
/// ago. nullptr if the value is missing, is not a gradient, or has no colours.
///
/// Defined in `main.cpp`: reading it safely needs the privates-opening hack at
/// the top of that file, and the overview's rings already read it the same way.
extern Config::CGradientValueData* borderGradient(bool active);

void CGolemBar::syncFloatTint() {
    if (!validMapped(m_pWindow))
        return;
    const auto PWINDOW = m_pWindow.lock();

    // Back in the layout: hand the window its own colours and its border back.
    // A tiled window must look exactly like every other tiled window.
    if (m_hidden) {
        if (!m_tinted)
            return;
        PWINDOW->m_ruleApplicator->activeBorderColor().unset(Desktop::Types::PRIORITY_SET_PROP);
        PWINDOW->m_ruleApplicator->inactiveBorderColor().unset(Desktop::Types::PRIORITY_SET_PROP);
        PWINDOW->m_ruleApplicator->borderSize().unset(Desktop::Types::PRIORITY_SET_PROP);
        m_tinted = false;
        refreshDecorationsLater();
        return;
    }

    const auto* const ACTIVE   = borderGradient(true);
    const auto* const INACTIVE = borderGradient(false);
    if (!ACTIVE || !INACTIVE)
        return; // no readable colours; leave the window alone rather than guess

    const auto tint = [](const Config::CGradientValueData& src) {
        Config::CGradientValueData out = src;
        for (auto& c : out.m_colors)
            c.a *= GOLEM_FLOAT_ALPHA;
        out.updateColorsOk(); // the shader reads this, not m_colors
        return out;
    };
    auto wantActive   = tint(*ACTIVE);
    auto wantInactive = tint(*INACTIVE);

    // Only when they actually changed — see the note on `m_tintActive`.
    if (m_tinted && wantActive == m_tintActive && wantInactive == m_tintInactive)
        return;

    PWINDOW->m_ruleApplicator->activeBorderColor().set(wantActive, Desktop::Types::PRIORITY_SET_PROP);
    PWINDOW->m_ruleApplicator->inactiveBorderColor().set(wantInactive, Desktop::Types::PRIORITY_SET_PROP);
    // ⭐ AND NO BORDER AT ALL WHILE IT FLOATS (Max, 2026-09-15: *"it IS the
    // border, i still see it on the top corners — get rid of it"*).
    //
    // The border and the bar are the same translucent colour, so wherever their
    // paint meets it doubles and reads as a bright line: along the top edge
    // (fixed by cutting the strip back), and then along the corner arc, where
    // the wedge fill runs beside the border's curve. Measured there: the strip
    // reads 87,103,109 and the arc 117,114,117. Chasing that alignment pixel by
    // pixel is a losing game — the decoration box, the border ring and the fill
    // bands all round independently.
    //
    // With no border, the whole class of seam goes away: the bar meets the
    // window's own edge directly and the fill's only neighbour is the OPAQUE
    // content, which cannot double-blend. Everything downstream adapts on its
    // own, because it all reads `getRealBorderSize()` — the corner radius, the
    // button centring, and where the strip stops.
    //
    // It is the whole ring, not just the top: Hyprland has no per-side border.
    // Floats therefore carry no frame line on any side; the bar is their chrome.
    PWINDOW->m_ruleApplicator->borderSize().set(0, Desktop::Types::PRIORITY_SET_PROP);
    m_tintActive   = std::move(wantActive);
    m_tintInactive = std::move(wantInactive);
    m_tinted       = true;
    refreshDecorationsLater();
}

/// Make the compositor pick the overrides up — NEVER inline.
///
/// ⚠️ `updateDecorationValues()` re-applies a window's rules and walks its
/// decorations. Everything that calls `syncFloatTint` reaches it from inside
/// the RENDER PASS (`renderPass`, and `syncHidden` by way of `draw`), so
/// calling it there re-enters the very list the pass is iterating — for every
/// window at once when Golem's float mode flips the whole desktop. That froze
/// the session (Max, 2026-09-15, tile→floating).
///
/// `doLater` runs it on the event loop between frames instead: same effect,
/// one frame later, with nothing re-entered. Guarded so a burst of frames
/// cannot queue a pile of them.
void CGolemBar::refreshDecorationsLater() {
    // Never during teardown, and always through a cancellable LOCK: the lock
    // dies with the bar, withdrawing the callback — a plain doLater's lambda
    // lives in this .so and outliving the unload is the 2026-09-15 crash.
    if (g_barsShuttingDown || !g_pEventLoopManager)
        return;
    if (m_refreshLock)
        return; // one pending refresh is enough for any burst of frames

    // ⚠️ `valid()` + `get()`, NOT `lock()`: the bars are UNIQUE pointers, and
    // hyprutils marks unique-backed impls `lockable = false` — lock() on
    // `m_self` is null by design, ALWAYS. The first cut locked here, so this
    // lambda silently did nothing (and the never-reset lock meant each bar
    // could queue exactly one refresh in its life). The raw pointer is safe:
    // valid() proves the bar alive, and nothing can destroy it between the
    // check and the use — both run in this same event-loop dispatch.
    m_refreshLock = g_pEventLoopManager->doLaterLock([self = m_self]() {
        if (!self.valid())
            return; // the bar (and its window) went away before we ran
        auto* const BAR = self.get();
        BAR->m_refreshLock.reset();
        if (!validMapped(BAR->m_pWindow))
            return;
        const auto PWINDOW = BAR->m_pWindow.lock();
        PWINDOW->updateDecorationValues();
        // ⭐ AND DAMAGE THE FRAME, or the change is invisible on a still
        // window: the border ring's OLD pixels stay on screen until something
        // else happens to repaint them. Seen live (2026-09-15): the first
        // windows after a reboot sat quietly with their border still drawn,
        // and it vanished only when they were MOVED — the move was just the
        // first full repaint. Windows that map mid-session never showed it
        // because the popin animation damages everything while the override
        // lands. Safe here: this runs on the event loop, not the render pass.
        g_pHyprRenderer->damageWindow(PWINDOW, true);
    });
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
    // Going back into the layout takes the bar out from under the pointer with
    // no motion to notice it — give the arrow back now, or the grab hand sticks
    // until the pointer happens to cross another bar.
    if (m_hidden)
        releaseCursorIfOwner();
    // The frame is translucent only while the window floats, so the tint
    // arrives and leaves with the bar itself.
    syncFloatTint();
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

/// The shape the BARS currently have on the pointer, "" when they have given it
/// back. One global rather than one per bar: only one bar can be under the
/// pointer, and the pointer is one thing — a per-bar cache would let a bar the
/// pointer has already left believe it still owns the shape.
static std::string g_barCursor;

/// WHICH bar put it there. Every bar hears every motion — `inputIsValid()`
/// passes for the window under the pointer OR the focused one — so without an
/// owner a bar the pointer is nowhere near would "helpfully" clear the shape
/// the hovered bar had just set, and which one won came down to listener order.
/// That is why the hint worked on some windows and not others (Max,
/// 2026-09-15). Only the owner may hand the pointer back.
// A RAW pointer compared by IDENTITY only, never dereferenced. NOT a weak
// pointer: the bars are held as UNIQUE pointers, and hyprutils' WP::lock() on
// a unique-backed weak always returns null — so `owner.lock().get() == this`
// was FALSE even one line after the assignment, the release branch was dead
// code, and the hand never let go (proven by the 2026-09-15 trace: owner=0 on
// every event). Identity stays safe because the destructor and shutdown clear
// it before the object can die.
static CGolemBar* g_barCursorOwner = nullptr;

/// The pending deferred apply, as a CANCELLABLE lock: destroying it (a new
/// assignment, or `Bars::shutdown`) withdraws the callback from the event
/// loop. Plain `doLater` has no way back, and a callback that outlives the
/// unload jumps into unmapped memory — the 2026-09-15 unload crash.
static UP<SEventLoopDoLaterLock> g_barCursorLock;
static void                      applyBarCursorLater();

/// One-shot settle timer. The immediate set and the `doLater` both run before
/// the CLIENT's own cursor request has crossed the wire — a terminal asks for
/// its text beam on pointer enter, the request arrives milliseconds later, and
/// the beam wins over whatever we set. That is the "unstable" hand (Max,
/// 2026-09-15): ours and the app's alternating, with the app's arriving last.
/// One re-assert ~60ms after we take the shape lands after any such request
/// and settles it; released (empty `g_barCursor`) it does nothing.
static SP<CEventLoopTimer> g_barCursorTimer;

void golemBarCursorTimerInit() {
    g_barCursorTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer> self, void*) {
            // Owner too, not just the shape: this timer is the thing that
            // stamped a STALE hand back over the app's cursor when the release
            // was broken. A shape without a live owner is by definition stale.
            if (g_barCursor.empty() || !g_barCursorOwner || !g_pCursorManager)
                return;
            if (g_pInputManager)
                g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
            g_pCursorManager->setCursorFromName(g_barCursor);
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_barCursorTimer);
}

void golemBarCursorTimerDrop() {
    // The callback's code lives in this .so — a timer that survives the unload
    // fires into unmapped memory.
    if (g_barCursorTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_barCursorTimer);
    g_barCursorTimer.reset();

    // The pending deferred apply dies here too, for the same reason.
    g_barCursorLock.reset();

    // And the cursor bookkeeping is emptied BEFORE the decorations are
    // destroyed (which happens after PLUGIN_EXIT returns), so the destructors'
    // releaseCursorIfOwner finds nothing held and queues nothing. A destructor
    // that queued a callback here is exactly how the 2026-09-15 unload crash
    // happened: pointer parked on a button, dtor released the shape, the
    // release queued a doLater, the doLater outlived the library.
    g_barCursorOwner = nullptr;
    g_barCursor.clear();
}

/// Put a shape on the pointer, or `nullptr` to hand it back to the arrow.
///
/// ⚠️ Hyprland has its OWN cursor state for "the pointer is on a window edge"
/// (`m_borderIconDirection`, re-asserted from the compositor's motion handling,
/// which runs BEFORE ours). It outranks whatever we set, so it has to be
/// cleared first or the resize arrow simply comes back the next frame — the
/// lesson the overview's edge cursors already cost (see `main.cpp`). Writing
/// the member is how it is cleared: `setBorderCursorIcon()` is not exported
/// from the Hyprland binary, and the plugin fails to LOAD on the undefined
/// symbol. It is reachable thanks to the privates hack in `golemBar.hpp`.
static void setBarCursor(const char* shape) {
    if (!shape) {
        if (g_barCursor.empty())
            return;
        g_barCursor.clear();
        if (g_pCursorManager)
            g_pCursorManager->setCursorFromName("left_ptr");
        applyBarCursorLater();
        return;
    }
    if (g_pInputManager)
        g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
    g_barCursor = shape;
    // Immediately AND deferred, and that redundancy is the stability. The
    // motion listeners run after the compositor's own motion handling, so the
    // immediate set usually lands last and the shape holds; the deferred one
    // catches the cases where something later in the same dispatch (a focus
    // change, the app's own cursor request) takes the pointer back — without
    // it the hand flickered against whatever kept re-setting the arrow.
    if (g_pCursorManager)
        g_pCursorManager->setCursorFromName(shape);
    applyBarCursorLater();
    // And once more after the client's enter-time cursor request has had time
    // to arrive — see `g_barCursorTimer`.
    if (g_barCursorTimer)
        g_barCursorTimer->updateTimeout(std::chrono::milliseconds(60));
}

/// ⭐ SET THE SHAPE *AFTER* THE COMPOSITOR HAS FINISHED THE MOTION, NOT DURING.
///
/// Our motion listener runs from inside Hyprland's own `mouseMoveUnified`,
/// which decides the cursor for itself once it knows what is under the pointer.
/// Setting the shape from the listener is therefore a write the compositor
/// overwrites moments later in the same event: verified with `grim -c` (which
/// DOES capture the cursor — plain `grim` does not), the pointer sat on the bar
/// wearing the arrow while our log showed `setBarCursor(grab)` firing every
/// motion with the manager present and the theme carrying a `grab` cursor.
///
/// `doLater` puts the set on the event loop, so it lands after the compositor
/// is done and ours is the last word. Coalesced by a flag: a burst of motion
/// events queues one apply, not one per event.
static void applyBarCursorLater() {
    // Never during teardown: the destructors run after `Bars::shutdown`, and a
    // callback queued from them survives the unload — see `g_barsShuttingDown`.
    if (g_barsShuttingDown || !g_pEventLoopManager)
        return;

    // Assigning over a pending lock CANCELS it — a burst of motion coalesces
    // to the newest request, and the callback re-reads the wanted shape anyway.
    g_barCursorLock = g_pEventLoopManager->doLaterLock([]() {
        if (!g_pCursorManager)
            return;
        // Re-read the wanted shape at apply time rather than capturing it: the
        // pointer may have crossed onto — or off — a bar since this was queued.
        const std::string WANT = g_barCursor.empty() ? "left_ptr" : g_barCursor;
        // ⛔ DO NOT reach into `g_pInputManager->m_cursorSurfaceInfo` to make
        // the compositor believe this shape is its own. Tried 2026-09-15 —
        // writing `.name`/`.wlSurface` from here **crashed the session**. That
        // struct is the compositor's live cursor state, torn down and rebuilt
        // on its own schedule; a plugin writing it from an event-loop callback
        // is writing under the compositor's feet. The privates hack makes it
        // reachable, not safe.
        if (g_pInputManager)
            g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
        g_pCursorManager->setCursorFromName(WANT);
    });
}

// Hand the pointer back, but only if this bar is the one holding it. Called
// from the paths where a bar stops being hoverable without a motion event to
// notice it: tiling (the bar hides) and teardown.
void CGolemBar::releaseCursorIfOwner() {
    if (g_barCursorOwner != this)
        return;
    g_barCursorOwner = nullptr;
    setBarCursor(nullptr);
}

// Where the pointer stands on this bar: on a button, on the strip between
// them, or not on the bar at all. One probe feeding both the cursor shape and
// the ownership logic, so they can never disagree about what is hovered.
CGolemBar::eBarHover CGolemBar::hoverZone() {
    if (m_hidden || !inputIsValid())
        return HOVER_NONE;

    // Only the window ACTUALLY UNDER the pointer may claim a hover.
    // `inputIsValid` passes for the hovered window OR the focused one — right
    // for clicks (a drag must keep working wherever focus sits), wrong for a
    // hover hint: a focused window whose bar rect lies BEHIND another window's
    // content would claim the hand while the pointer is somewhere else
    // entirely, and nothing under the pointer would ever release it.
    const auto WINDOWATCURSOR = g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                                     Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    if (WINDOWATCURSOR != m_pWindow)
        return HOVER_NONE;

    const auto COORDS = cursorRelativeToBar();
    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, GOLEM_BAR_HEIGHT - 1))
        return HOVER_NONE;

    for (size_t i = 0; i < g_pBarsState->buttons.size(); ++i) {
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, GOLEM_BAR_HEIGHT};
        Vector2D   currentPos = Vector2D{golemButtonX(i, 1.F), golemButtonY(BARBUF.y, borderBelow(), GOLEM_BUTTON_SIZE)}.floor();
        if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING, currentPos.y + GOLEM_BUTTON_SIZE))
            return HOVER_BUTTON;
    }
    return HOVER_STRIP;
}

void CGolemBar::onMouseMove(Vector2D coords) {
    damageOnButtonHover();

    // The bar says what it is by how the pointer looks on it (Max, 2026-09-15):
    // an OPEN HAND over the strip — the thing you pick the window up by — a
    // CLOSED one while it is actually held, and the INDEX FINGER over the
    // buttons, which are things you press, not things you drag.
    //
    // Re-asserted on every motion rather than only on the crossing: the
    // compositor recomputes its own cursor against whatever lies under the
    // pointer before this runs, so a shape set once quietly reverts.
    if (m_bDraggingThis || m_bDragPending) {
        // Held: the hand stays closed even when a fast drag outruns the strip.
        g_barCursorOwner = this;
        setBarCursor("grabbing");
    } else {
        switch (hoverZone()) {
            // Taking the shape also takes ownership, so whichever order the
            // bars' listeners run in, the one under the pointer ends up
            // holding it: if the old owner runs after us it sees the
            // ownership has moved and leaves the pointer alone.
            case HOVER_STRIP:
                g_barCursorOwner = this;
                setBarCursor("grab");
                break;
            case HOVER_BUTTON:
                g_barCursorOwner = this;
                setBarCursor("pointer");
                break;
            case HOVER_NONE:
                if (g_barCursorOwner == this) {
                    g_barCursorOwner = nullptr;
                    setBarCursor(nullptr);
                }
                break;
        }
    }

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
    // The hand closes ON THE PRESS, not when motion starts: taking hold is
    // the press, the drag is just where the hand goes afterwards.
    g_barCursorOwner = this;
    setBarCursor("grabbing");
}

void CGolemBar::handleUpEvent(Event::SCallbackInfo& info) {
    // ⚠️ The focus check guards ONLY the event-swallowing. It used to guard the
    // whole function — and when focus had shifted between press and release
    // (the daemon rearranges focus; a drag can land it elsewhere), the early
    // return skipped ALL cleanup: `m_bDragPending`/`m_bDraggingThis` stayed
    // true forever, and every later motion re-asserted the hand. That is the
    // "open hand gets stuck" (Max, 2026-09-15). State cleanup on button-up is
    // unconditional; a release ends the press NO MATTER where focus went.
    if (m_bCancelledDown && m_pWindow.lock() == Desktop::focusState()->window())
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        Log::logger->log(Log::DEBUG, "[golembar] drag ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;

    // Let go: the hand reopens over the strip, points over a button, or goes
    // back to the arrow if the release landed elsewhere.
    if (g_barCursorOwner == this) {
        switch (hoverZone()) {
            case HOVER_STRIP: setBarCursor("grab"); break;
            case HOVER_BUTTON: setBarCursor("pointer"); break;
            case HOVER_NONE:
                g_barCursorOwner = nullptr;
                setBarCursor(nullptr);
                break;
        }
    }
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
                // Swallows the press deliberately: the button is real, it just
                // has no job yet. Returning true below still consumes the
                // click, so it never falls through to a window drag.
                case GOLEM_BAR_UNWIRED: break;
            }
            return true;
        }

    }
    return false;
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

        // The buttons keep their OWN colour at full strength — they are the one
        // thing on the bar you aim at, and a target you can see through is a
        // worse target (Max, 2026-09-15: *"the buttons stay 100% opacity"*).
        // `GOLEM_FLOAT_ALPHA` tints the frame's colour, never these; `a` is the
        // window's own fade, which they do follow.
        auto       color = button.bgcol;
        color.a *= a;

        // Hover lifts the disc toward white — the same answer at the same
        // strength on all three, so the row reads as one control set. State
        // comes from `m_iButtonHoverState`, written on the motion path.
        if ((m_iButtonHoverState >> i) & 1u) {
            color.r += (1.0 - color.r) * GOLEM_BUTTON_HOVER_LIFT;
            color.g += (1.0 - color.g) * GOLEM_BUTTON_HOVER_LIFT;
            color.b += (1.0 - color.b) * GOLEM_BUTTON_HOVER_LIFT;
        }

        CBox buttonBox = {barBox->x + golemButtonX(i, scale), barBox->y + golemButtonY(barBox->h, BORDER * scale, scaledButtonSize), scaledButtonSize, scaledButtonSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(buttonBox, color, {.round = static_cast<int>(std::round(scaledButtonSize / 2.0)), .roundingPower = 2.F});
    }
}

void CGolemBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const auto visibleCount = getVisibleButtonCount(Vector2D{barBox->w, barBox->h}, scale);
    const auto BORDER       = borderBelow();

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pBarsState->buttons[i];
        const auto scaledButtonSize = GOLEM_BUTTON_SIZE * scale;

        if ((!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty())
            button.iconTex = g_pHyprRenderer->renderText(button.icon, button.fgcol, std::round(GOLEM_BUTTON_SIZE * 0.62 * scale), false, GOLEM_BAR_FONT, scaledButtonSize);

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

    // Keep the translucent frame in step with the colour pass. Guarded inside
    // to a no-op unless the colours actually moved, so this is a comparison
    // per frame and a real update only when the desktop's colour changes.
    syncFloatTint();

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

    // ⭐ STOP THE STRIP WHERE THE BORDER STARTS — they must not share a pixel.
    //
    // The bar and the border are the same colour at the same alpha, so on an
    // opaque frame an overlap of a pixel or two at their junction was invisible
    // and nobody noticed the decoration box and the border's outer edge round
    // independently. Translucent, that overlap DOUBLE-BLENDS: measured down a
    // column through the top edge, the strip reads 87,103,109 and the two rows
    // where they meet jump to 117,118,116 — a bright line along the top edge and
    // around the top corners, which is what reads as "the window's border"
    // showing through the frame (Max, 2026-09-15).
    //
    // So the strip is cut back to the border's OUTER edge, derived from the
    // window itself rather than from the decoration box, and FLOORED so a
    // rounding wobble can only ever leave the border a hair short — which is
    // invisible, because the border paints that hair itself. Overlapping is the
    // failure; abutting is not.
    const double BORDEROUT = std::floor((PWINDOW->m_realPosition->value().y + PWINDOW->m_floatingOffset.y - PWINDOW->getRealBorderSize() - pMonitor->m_position.y) * pMonitor->m_scale);
    if (BORDEROUT > clipBox.y)
        clipBox.h = std::min(clipBox.h, BORDEROUT - clipBox.y);

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
    // ⚠️ THE REGION HUGS THE CURVE — it is NOT two solid corner boxes, and it
    // must not reach a pixel PAST the curve either. Two different artefacts,
    // both of which Max caught by zooming in, and both caused by paint landing
    // somewhere it is not the only layer:
    //
    //   · under the CONTENT — content can be translucent (a terminal's alpha,
    //     an opacity rule), so solid corner boxes ghosted through it as a
    //     lighter square in each top corner: *"there is a square on each side,
    //     inside the content"*;
    //   · under the BORDER — the frame is translucent on floats now
    //     (`syncFloatTint`), so a fill tucked beneath it double-blends into a
    //     brighter arc tracing each corner. Measured: the border reads
    //     189,142,115 down the straight side and 236,175,140 where the old
    //     2px tuck sat under it.
    //
    // So the fill stops exactly AT the border's outer edge and nowhere beyond.
    // One band per physical row, each as wide as the curve's inset at the row's
    // BOTTOM — the narrowest the wedge gets anywhere in that row, so the paint
    // can never cross the curve. The shortfall is under a pixel and lands in
    // the border's own antialiased edge; the doubling was two full pixels.
    //
    // The curve is Hyprland's own corner: radius `ROUNDING`, exponent
    // `roundingPower()`. It falls away fast (~35 rows at Golem's radius), so
    // the loop ends itself once the wedge is thinner than a pixel — no bottom
    // cut, and nothing painted down at a short window's own corners.
    {
        const double P    = std::max(2.0, static_cast<double>(PWINDOW->roundingPower()));
        const double R    = ROUNDING * pMonitor->m_scale;
        const double seam = clipBox.y + clipBox.h;
        const double HMAX = std::min(R, PWINDOW->m_realSize->value().y * pMonitor->m_scale / 2.0);
        const double WMAX = clipBox.w / 2.0;
        const auto   inset = [&](double d) { // the curve's distance in from the side, at depth d below the seam
            const double t = std::clamp(1.0 - d / R, 0.0, 1.0);
            return R - R * std::pow(1.0 - std::pow(t, P), 1.0 / P);
        };
        for (double d = 0.0; d + 1.0 <= HMAX && R > 0.0; d += 1.0) {
            const double w = std::min(inset(d + 1.0), WMAX);
            if (w < 1.0)
                break; // thinner than a pixel — the border's own edge covers the rest
            barDamage.add(CBox{clipBox.x, seam + d, w, 1.0});
            barDamage.add(CBox{clipBox.x + clipBox.w - w, seam + d, w, 1.0});
        }
    }

    // Rounded on all four corners and clipped at the bar's bottom edge, which
    // leaves exactly a titlebar: curved on top, square where it meets the
    // window.
    g_pHyprOpenGL->renderRect(titleBarBox, color, {.damage = &barDamage, .round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // NO TITLE. The bar carries its buttons and nothing else (Max,
    // 2026-09-15: *"get rid of the title on the bar"*) — the window's name is
    // already on the OPTIONS bar at the top of the screen, and at Golem's
    // transparency a second copy of it read as clutter over the wallpaper.
    // Truncated to whole pixels (upstream's), stated as doubles so the box's
    // own type does not have to narrow them.
    CBox barBox = {titleBarBox.x, titleBarBox.y, static_cast<double>(static_cast<int>(BARBUF.x)), static_cast<double>(static_cast<int>(BARBUF.y))};

    renderBarButtons(&barBox, pMonitor->m_scale, a);
    m_bButtonsDirty = false;

    renderBarButtonsText(&barBox, pMonitor->m_scale, a);
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

// The ONE writer of `m_iButtonHoverState`: input-side tracking on the motion
// path, so the render functions only READ it. (Its predecessor kept a single
// bool for all three buttons and toggled it per button per motion — while the
// pointer sat on any button, two of the three comparisons disagreed with the
// bool every event, and the bar damaged itself continuously.)
void CGolemBar::damageOnButtonHover() {
    // Hidden or unhoverable, nothing may stay lit.
    unsigned int mask = 0;
    if (!m_hidden && inputIsValid()) {
        const auto COORDS = cursorRelativeToBar();
        for (size_t i = 0; i < g_pBarsState->buttons.size(); ++i) {
            const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, GOLEM_BAR_HEIGHT};
            Vector2D   currentPos = Vector2D{golemButtonX(i, 1.F), golemButtonY(BARBUF.y, borderBelow(), GOLEM_BUTTON_SIZE)}.floor();
            if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING, currentPos.y + GOLEM_BUTTON_SIZE))
                mask |= 1u << i;
        }
    }
    if (mask != m_iButtonHoverState) {
        m_iButtonHoverState = mask;
        damageEntire();
    }
}
