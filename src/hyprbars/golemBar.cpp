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
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
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
#include <hyprland/src/protocols/core/Compositor.hpp>    // the surface tree (subsurface commits)
#include <hyprland/src/protocols/core/Subcompositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include "barsGlobals.hpp"
#include "BarPassElement.hpp"
#include "bars.hpp"

#include <chrono>
#include <climits>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

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

// sRGB ↔ linear, the daemon's own (`srgb_to_linear` / `linear_to_srgb`).
static float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
static float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
/// Relative luminance of a LINEAR colour.
static float linearLuma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/// Ink that reads on whatever the bar turns out to be — the daemon's `ink_on`
/// rule: linear luminance of the fill against the WCAG flip point, so the
/// strip and the OPTIONS bar choose their ink the same way.
static CHyprColor inkOn(const CHyprColor& bg) {
    const float L = linearLuma(srgbToLinear(bg.r), srgbToLinear(bg.g), srgbToLinear(bg.b));
    return L > GOLEM_BRIGHT_LUMA ? CHyprColor{0xFF14171CULL} : CHyprColor{0xFFE8E6E3ULL};
}

/// The strip's colour from the window's sampled top edge — the daemon's
/// `Backdrop::surface` for a matched window: the sample with the resting wash
/// composited over it in linear light (`box_fill` of `rest_wash`), back to
/// sRGB, at the content's sampled alpha (the window's own opacity is applied
/// by the draw, once — see `GOLEM_SAMPLE_ROWS`'s neighbours in the header).
/// `dim` darkens it the way Hyprland dims an unfocused window's content (a
/// straight multiply on the output colour).
static CHyprColor sampledBarColor(const float sample[4], float dim) {
    const float lr = srgbToLinear(sample[0]), lg = srgbToLinear(sample[1]), lb = srgbToLinear(sample[2]);
    const bool  bright = linearLuma(lr, lg, lb) > GOLEM_BRIGHT_LUMA;
    // the daemon's `wash(white, a)`: white = srgb_to_linear(a)/a per channel
    const float a = bright ? GOLEM_WASH_BLACK_A : GOLEM_WASH_WHITE_A;
    const float w = (bright || a <= 0.F) ? 0.F : srgbToLinear(a) / a;
    const float fr = lr * (1.F - a) + w * a, fg = lg * (1.F - a) + w * a, fb = lb * (1.F - a) + w * a;
    const float k = 1.F - dim;
    return CHyprColor{std::clamp(linearToSrgb(fr) * k, 0.F, 1.F), std::clamp(linearToSrgb(fg) * k, 0.F, 1.F), std::clamp(linearToSrgb(fb) * k, 0.F, 1.F), std::clamp(sample[3], 0.F, 1.F)};
}

/// A window on the STAGE owns the whole screen (task mode maximizes it into the
/// stage rect, the deck sitting in the gap below). A titlebar there is chrome on
/// a window that is meant to stand alone — and it would float orphaned over the
/// deck's band — so the bar stands down while the window is staged.
///
/// The daemon tags the staged window `golem-stage` (the SAME tag its
/// `golem-stage-frame` rule matches to hand back the border + rounding), so the
/// bar just reads that tag. Precise by construction: task mode's single staged
/// window is tagged and loses its bar; desk-mode floats are never tagged (the
/// tag only reaches a desk's window when it is alone and tiled) and keep theirs.
static bool windowStaged(PHLWINDOW w) {
    return w && w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged("golem-stage", true);
}

// Golem's browser (Firefox) carries its OWN window controls INSIDE its toolbar
// (the macOS "traffic lights"), so it must never wear a Golem titlebar — it
// floats bare, like a macOS window. Matched by class; the browser keeps the
// "firefox" WM class; Golem's Seam (2026-09-27) has its own class "seam".
static bool windowIsFirefox(PHLWINDOW w) {
    return w && (w->m_class == "firefox" || w->m_initialClass == "firefox" || w->m_class == "seam" || w->m_initialClass == "seam");
}

// THE STRAIGHT SEAM (Max, 2026-09-26: *"make the bottom of the bar straight…
// i want the top of the window to be square only when there is a bar. but not
// on floating with no bars, or non floating windows, etc."*).
//
// Hyprland rounds all four corners of a window from one radius, so under the
// bar the content curved away at each end and the strip's bottom edge read as
// two arcs. Golem's compositor patch (`hyprland-window-square-top.patch`)
// renders a window tagged `square-top` with its top two corners square —
// surface, border and blur — while the bottom two keep their rounding. The bar
// owns that tag: it is applied for exactly as long as the bar is showing and
// removed the moment it hides, so a tiled window, a staged one and the
// browser (none of which wear a bar) keep Hyprland's corners untouched.
static constexpr const char* GOLEM_SQUARE_TOP_TAG = "square-top";
static bool                  windowSquareTop(PHLWINDOW w) {
    return w && w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged(GOLEM_SQUARE_TOP_TAG, true);
}

// The browser's traffic lights show ONLY while it is a FLOATING window (not
// staged, not tiled) — the same condition that would raise a bar on any other
// window. The browser's own chrome can't see Hyprland's float state, so hand it
// across a 1-byte file it polls (golem-chrome.js `trafficLights`). Written only
// on CHANGE — this is reached from the per-window update path, and a compare is
// all an unchanged frame costs.
// `last` is THIS bar's memory: with one global, two browser windows in different
// states (a floating PiP over a tiled main window) rewrote the file on alternate
// frames and flickered the traffic lights.
static void writeFirefoxFloatSignal(int& last, bool floating) {
    const int v = floating ? 1 : 0;
    if (v == last)
        return;
    last = v;
    if (FILE* f = fopen("/tmp/golem-ff-float", "w")) {
        fputc(floating ? '1' : '0', f);
        fclose(f);
    }
}

CGolemBar::CGolemBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (PMONITOR)
        PMONITOR->m_scheduledRecalc = true;

    m_hidden = !pWindow->m_isFloating || windowStaged(pWindow) || windowIsFirefox(pWindow);
    syncSquareTop();
    if (windowIsFirefox(pWindow)) {
        writeFirefoxFloatSignal(m_ffFloatSignal, pWindow->m_isFloating && !windowStaged(pWindow));
        syncFirefoxBorder(); // mute the browser's 1px border to a faint hairline
    }

    // Every commit of the window's main surface may change the colour under
    // the seam; the sampler's tick picks the flag up off the render pass.
    if (pWindow->wlSurface() && pWindow->wlSurface()->resource())
        m_commitListener = pWindow->wlSurface()->resource()->m_events.commit.listen([this] { m_sampleDirty = true; });
    // ...and so may a commit of any of its subsurfaces (a Seam webapp's page is
    // one: the bar stayed the colour of the blank page it sampled at map).
    listenSubsurfaces();

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

    // Hand the window its rounded top corners back: the tag is the bar's, and
    // a window outliving this plugin must not keep it. A set erase, nothing
    // more — see the teardown rule above.
    if (validMapped(m_pWindow))
        m_pWindow.lock()->m_ruleApplicator->m_tagKeeper.applyTag(std::string("-") + GOLEM_SQUARE_TOP_TAG);

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

/// Read the top `rows` logical px of `w`'s main surface, averaged, as sRGB
/// 0..1 (defined in `../main.cpp`, which owns the capture plumbing). Renders,
/// so: event loop only.
extern bool golemSampleWindowTop(PHLWINDOW w, double rows, float out[4]);
/// A line into the plugin's trace file (/tmp/waveview-trace.log).
extern void golemTrace(const std::string& line);

/// Whether waveview's overview or spread owns the screen (defined in
/// `main.cpp`). While it does, pointer input belongs to their CARDS: a press
/// must never take hold of an invisible bar beneath the overlay, and any
/// hold from before the takeover ends immediately — a swallowed release used
/// to leak `m_bDragPending` and every later motion re-asserted the closed
/// hand ("the pointer is stuck on the closed hand", Max, 2026-09-16).
extern bool waveviewOwnsScreen();

/// Minimize `w` to the dock (defined in `../main.cpp` — it owns the capture
/// plumbing, the fly animation and the daemon protocol).
void golemMinimize(PHLWINDOW w);

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
    // The window's own colour once sampled (see `sampledBarColor`): one flat
    // stop for the focused window, the same stop dimmed like its content for
    // the unfocused one. The compositor animates between the two with focus,
    // exactly as it does for its own border colours. Until the first sample
    // lands the daemon's gradient stands in, as it always did.
    const auto flat = [&](const Config::CGradientValueData& structure, float dim) {
        Config::CGradientValueData out = structure;
        out.m_colors.clear();
        out.m_colors.push_back(sampledBarColor(m_sample, dim));
        out.updateColorsOk();
        return out;
    };
    auto wantActive   = m_hasSample ? flat(*ACTIVE, 0.F) : tint(*ACTIVE);
    auto wantInactive = m_hasSample ? flat(*INACTIVE, GOLEM_DIM_INACTIVE) : tint(*INACTIVE);

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

void CGolemBar::syncFirefoxBorder() {
    if (!validMapped(m_pWindow))
        return;
    const auto PWINDOW = m_pWindow.lock();

    // A fixed faint hairline. Alpha is the whole point of "subtle": a low-alpha
    // light edge over the dark window reads as a delicate macOS-style border, and
    // a FIXED colour stays quiet no matter what screen-matched colour the shell's
    // dynamic border takes. Copy the window's live border gradient only for its
    // STRUCTURE (angle, the ok-cache), then swap in a single faint stop.
    const auto make = [&](uint64_t argb) {
        Config::CGradientValueData g = PWINDOW->m_realBorderColor;
        g.m_colors.clear();
        g.m_colors.push_back(CHyprColor{argb});
        g.updateColorsOk(); // the shader reads this, not m_colors
        return g;
    };
    // ~9% white focused, ~4% unfocused. 1px is Hyprland's thinnest real border
    // (border_size is a whole number; 0 = none), so past here "thinner" is really
    // "fainter" — a lighter line reads as a more delicate edge. Tune HERE (needs a
    // rebuild) for fainter still or a warmer hue; thickness stays the config rule's.
    PWINDOW->m_ruleApplicator->activeBorderColor().set(make(0x16FFFFFFULL), Desktop::Types::PRIORITY_SET_PROP);
    PWINDOW->m_ruleApplicator->inactiveBorderColor().set(make(0x0BFFFFFFULL), Desktop::Types::PRIORITY_SET_PROP);
    // borderSize is deliberately NOT set here — the 1px comes from the
    // `firefox-subtle-border` window rule, so the thickness retunes without a
    // plugin rebuild. On the stage the stage rule drops size to 0 (no border),
    // and this colour override simply has nothing to paint.
    m_tinted = true; // routes through the same guarded dtor / teardown as the tint
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

// The compositor's `square-top` tag, kept in step with the bar (see
// `windowSquareTop`): on while the bar shows, off the moment it hides. A tag
// flip is a set insert or erase — no rule or decoration machinery — so it is
// safe from the render pass, where `syncHidden` runs, and from the destructor.
// No damage is needed: a bar arriving or leaving reserves or frees the strip,
// which resizes the window and repaints the corners with it.
void CGolemBar::syncSquareTop() {
    if (!validMapped(m_pWindow))
        return;
    m_pWindow.lock()->m_ruleApplicator->m_tagKeeper.applyTag(std::string(m_hidden ? "-" : "+") + GOLEM_SQUARE_TOP_TAG);
}

// The bar belongs to windows that have LEFT the layout — and NOT to a window on
// the stage (see `windowStaged`). Upstream drove this from a window rule; asking
// the window is the same mechanism with the conditions Golem actually wants, and
// it costs two bool compares per update.
void CGolemBar::syncHidden() {
    if (!validMapped(m_pWindow))
        return;

    const auto PWINDOW = m_pWindow.lock();

    // Refresh the browser's traffic-light signal on every float/tile flip. Done
    // BEFORE the early-return below: the browser's bar is always hidden, so WANT
    // never changes for it and the return would otherwise skip this.
    if (windowIsFirefox(PWINDOW))
        writeFirefoxFloatSignal(m_ffFloatSignal, PWINDOW->m_isFloating && !windowStaged(PWINDOW));

    const bool WANT = !PWINDOW->m_isFloating || windowStaged(PWINDOW) || windowIsFirefox(PWINDOW);
    if (WANT == m_hidden)
        return;

    m_hidden = WANT;
    syncSquareTop();
    m_sampleDirty = true; // a bar coming back looks at its window afresh
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
    if (!validMapped(m_pWindow) || (m_hidden && !bare()))
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
    if (waveviewOwnsScreen()) {
        standDown();
        return;
    }
    // ⭐ A RELEASE is handled whether or not the bar is still "valid". The press
    // itself can make it invalid before the fingers lift: the orange button
    // parks the window on the invisible minimized workspace, a strip drag can
    // end over the dock. Gated on validity, the release never reached
    // `handleUpEvent` and `m_bCancelledDown` stayed set — so the NEXT release
    // this bar saw, the real one closing a click INSIDE the restored window,
    // was swallowed; the compositor had recorded that press and now held a
    // PHANTOM BUTTON forever: pointer focus pinned to the window, and every
    // workspace swipe refocusing it and bouncing straight back (Max,
    // 2026-09-26, twice, always the window he had minimized and restored;
    // a scripted workspace switch — which releases all buttons — "fixed" it).
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        handleUpEvent(info, e.button, inputIsValid());
        return;
    }
    if (!inputIsValid())
        return;
    handleDownEvent(info, e.button);
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

// ─────────────────────────── The resize hand ───────────────────────────
//
// One global motion listener: while the compositor's drag controller is in
// MBIND_RESIZE — the bar's corner grip or Hyprland's own edge resize — the
// closed hand is (re)asserted on every motion, because the compositor re-sets
// its directional arrow on every motion too (`m_borderIconDirection`, which
// `setBarCursor` clears). On the first motion after the drag ends the hand is
// handed back unless a bar holds the shape (the grip's own release logic then
// takes over): the edge hover re-arms its arrow on the next motion by itself.
static bool                g_resizeHand = false;
static CHyprSignalListener g_resizeHandListener;
static void                setBarCursor(const char* shape); // defined below, with the cursor state

void golemResizeHandInit() {
    g_resizeHandListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
        if (g_barsShuttingDown || !g_layoutManager)
            return;
        const auto& dc       = g_layoutManager->dragController();
        const bool  resizing = dc && dc->target() && dc->mode() == MBIND_RESIZE;
        if (resizing) {
            g_resizeHand = true;
            setBarCursor("grabbing");
        } else if (g_resizeHand) {
            g_resizeHand = false;
            if (!g_barCursorOwner)
                setBarCursor(nullptr);
        }
    });
}

void golemResizeHandDrop() {
    g_resizeHandListener.reset();
    g_resizeHand = false;
}

void golemBarCursorTimerInit() {
    g_barCursorTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer> self, void*) {
            // Owner too, not just the shape: this timer is the thing that
            // stamped a STALE hand back over the app's cursor when the release
            // was broken. A shape without a live owner is by definition stale.
            if (g_barCursor.empty() || (!g_barCursorOwner && !g_resizeHand) || !g_pCursorManager)
                return;
            if (g_pInputManager)
                g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
            g_pCursorManager->setCursorFromName(g_barCursor);
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_barCursorTimer);
}

// ─────────────────────────── The strip's texel ───────────────────────────
//
// One 1×1 texture shared by every bar, holding the colour of whichever strip is
// being drawn: `renderTexture` is the compositor's blurred-surface path and it
// draws textures, not colours. The texel is opaque; the strip's alpha rides the
// draw's `a`, so the result is premultiplied exactly like a window's pixels.
static SP<Render::ITexture> g_barSolidTex;
static uint8_t              g_barSolidPx[4] = {0, 0, 0, 0};

static bool ensureSolidTex(const CHyprColor& c) {
    const uint8_t px[4] = {(uint8_t)std::lround(std::clamp(c.r, 0.0, 1.0) * 255.0), (uint8_t)std::lround(std::clamp(c.g, 0.0, 1.0) * 255.0),
                           (uint8_t)std::lround(std::clamp(c.b, 0.0, 1.0) * 255.0), 255};
    if (!g_barSolidTex) {
        g_barSolidTex = makeShared<Render::GL::CGLTexture>(DRM_FORMAT_ABGR8888, (uint8_t*)px, 4, Vector2D(1, 1), false, false);
        std::copy(px, px + 4, g_barSolidPx);
        return g_barSolidTex && g_barSolidTex->m_texID != 0;
    }
    if (!std::equal(px, px + 4, g_barSolidPx)) {
        glBindTexture(GL_TEXTURE_2D, g_barSolidTex->m_texID);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::copy(px, px + 4, g_barSolidPx);
    }
    return g_barSolidTex->m_texID != 0;
}

void golemBarSolidTexDrop() {
    if (g_barSolidTex) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        g_barSolidTex.reset();
    }
}

// ─────────────────────────── The colour sampler ───────────────────────────
//
// One tick for all bars, on the event loop: each bar whose window committed
// since it last looked is re-sampled, at most every GOLEM_SAMPLE_MIN_MS. A
// sample is a tiny render + readback (`golemSampleWindowTop`), so it must
// never run from inside a render pass — hence a timer, not `draw()`.
static SP<CEventLoopTimer> g_barSampleTimer;

void CGolemBar::listenSubsurfaces() {
    m_resubscribe = false;
    m_subCommitListeners.clear();
    const auto w = m_pWindow.lock();
    if (!w || !w->wlSurface() || !w->wlSurface()->resource())
        return;
    const auto root = w->wlSurface()->resource();
    root->breadthfirst(
        [this, &root](SP<CWLSurfaceResource> s, const Vector2D&, void*) {
            if (!s || s == root)
                return;
            m_subCommitListeners.emplace_back(s->m_events.commit.listen([this] { m_sampleDirty = true; }));
            // a subsurface of a subsurface arrives on ITS parent's signal
            m_subCommitListeners.emplace_back(s->m_events.newSubsurface.listen([this](SP<CWLSubsurfaceResource>) {
                m_resubscribe = true;
                m_sampleDirty = true;
            }));
        },
        nullptr);
    m_newSubListener = root->m_events.newSubsurface.listen([this](SP<CWLSubsurfaceResource>) {
        m_resubscribe = true; // re-walked on the tick: never swap listeners mid-emission
        m_sampleDirty = true;
    });
}

void CGolemBar::syncSample() {
    if (m_resubscribe && validMapped(m_pWindow))
        listenSubsurfaces();
    if (m_hidden || !m_sampleDirty || !validMapped(m_pWindow))
        return;
    const auto now = std::chrono::steady_clock::now();
    if (m_hasSample && now - m_lastSample < std::chrono::milliseconds(GOLEM_SAMPLE_MIN_MS))
        return; // the flag stays up; the next tick gets it
    m_sampleDirty = false;
    m_lastSample  = now;

    float c[4] = {0.F, 0.F, 0.F, 0.F};
    if (!golemSampleWindowTop(m_pWindow.lock(), GOLEM_SAMPLE_ROWS, c) || c[3] < GOLEM_SAMPLE_MIN_ALPHA) {
        // a failed read (see GOLEM_SAMPLE_MIN_ALPHA): keep the last colour, look again
        if (m_sampleRetries == 0)
            golemTrace(std::format("bar-sample rejected (a={:.3f}), keeping the last colour; retrying", c[3]));
        if (++m_sampleRetries <= GOLEM_SAMPLE_RETRIES)
            m_sampleDirty = true;
        return;
    }
    m_sampleRetries = 0;
    // Only a visible change re-pushes the override — same threshold as the
    // daemon's border push (~1.5/255), so sampling noise stays off the window.
    constexpr float EPS = 0.006F;
    if (m_hasSample && std::abs(c[0] - m_sample[0]) < EPS && std::abs(c[1] - m_sample[1]) < EPS && std::abs(c[2] - m_sample[2]) < EPS &&
        std::abs(c[3] - m_sample[3]) < EPS)
        return;
    for (int i = 0; i < 4; ++i)
        m_sample[i] = c[i];
    m_hasSample = true;
    syncFloatTint(); // event loop: the deferred refresh inside is still the right shape
}

void golemBarSampleTimerInit() {
    g_barSampleTimer = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(100),
        [](SP<CEventLoopTimer> self, void*) {
            if (g_barsShuttingDown || !g_pBarsState)
                return;
            // `m_self` is unique-backed: valid()/get(), never lock() — see the
            // 2026-09-15 note on the owner pointer.
            for (auto& b : g_pBarsState->bars)
                if (b.valid())
                    b.get()->syncSample();
            self->updateTimeout(std::chrono::milliseconds(100));
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_barSampleTimer);
}

void golemBarSampleTimerDrop() {
    // Same reason as the cursor timer: the callback's code lives in this .so.
    if (g_barSampleTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_barSampleTimer);
    g_barSampleTimer.reset();
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
    if (!inputIsValid())
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

    // The browser floating bare: only its two corner grips are ours.
    if (m_hidden) {
        switch (golemBareZone(cursorRelativeToWindow(), m_pWindow->m_realSize->value().x).kind) {
            case SGolemBarZone::CORNER_LEFT: return HOVER_CORNER_LEFT;
            case SGolemBarZone::CORNER_RIGHT: return HOVER_CORNER_RIGHT;
            default: return HOVER_NONE;
        }
    }

    const auto COORDS = cursorRelativeToBar();
    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, GOLEM_BAR_HEIGHT - 1))
        return HOVER_NONE;

    // One map for the hand, the lift and the press — see `golemBarZone`.
    switch (golemBarZone(COORDS, g_pBarsState->buttons.size(), assignedBoxGlobal().w, GOLEM_BAR_HEIGHT, borderBelow()).kind) {
        case SGolemBarZone::BUTTON:
        case SGolemBarZone::CARD: return HOVER_BUTTON;
        case SGolemBarZone::CORNER_LEFT: return HOVER_CORNER_LEFT;
        case SGolemBarZone::CORNER_RIGHT: return HOVER_CORNER_RIGHT;
        case SGolemBarZone::STRIP: return HOVER_STRIP;
        default: return HOVER_NONE;
    }
}

void CGolemBar::standDown() {
    if (m_bDraggingThis)
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID); // never leave a real drag running under the overlay
    m_bDraggingThis  = false;
    m_bDragPending   = false;
    m_bCancelledDown = false;
    releaseCursorIfOwner();
}

void CGolemBar::onMouseMove(Vector2D coords) {
    if (waveviewOwnsScreen()) {
        standDown();
        return;
    }
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
            case HOVER_CORNER_LEFT:
                g_barCursorOwner = this;
                setBarCursor("nw-resize");
                break;
            case HOVER_CORNER_RIGHT:
                g_barCursorOwner = this;
                setBarCursor("ne-resize");
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

void CGolemBar::handleDownEvent(Event::SCallbackInfo& info, uint32_t button) {
    const auto PWINDOW = m_pWindow.lock();

    // The browser floating bare (`golemBareZone`): a press on one of its two top
    // corners takes hold of the window for a resize, exactly like a bar's grip;
    // any other press in its strip is the browser's own and passes untouched.
    if (m_hidden) {
        const auto ZONE = golemBareZone(cursorRelativeToWindow(), PWINDOW->m_realSize->value().x).kind;
        if (ZONE != SGolemBarZone::CORNER_LEFT && ZONE != SGolemBarZone::CORNER_RIGHT)
            return;
        if (g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                 Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING) != PWINDOW)
            return;
        if (Desktop::focusState()->window() != PWINDOW)
            Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);
        g_pCompositor->changeWindowZOrder(PWINDOW, true);
        info.cancelled      = true;
        m_bCancelledDown    = true;
        m_pressedButton     = button;
        m_bDragPending      = true;
        m_bResizeFromCorner = true;
        g_barCursorOwner    = this;
        setBarCursor("grabbing");
        return;
    }

    const auto COORDS  = cursorRelativeToBar();

    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, GOLEM_BAR_HEIGHT - 1)) {
        if (m_bDraggingThis) {
            g_pKeybindManager->m_dispatchers["mouse"](m_bResizeFromCorner ? "0resizewindow" : "0movewindow");
            Log::logger->log(Log::DEBUG, "[golembar] drag ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        return;
    }
    // Only the bar UNDER the pointer takes a press. `inputIsValid` also lets
    // the FOCUSED window's bar through — a drag must keep its motion and its
    // release after the pointer outran the window — but a press that landed
    // on another window covering this bar's strip is that window's, not a
    // reason to raise this one (or hit its orange button).
    if (g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                             Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING) != PWINDOW)
        return;

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        g_pCompositor->changeWindowZOrder(PWINDOW, true);

    info.cancelled   = true;
    m_bCancelledDown = true;
    m_pressedButton  = button;

    if (doButtonPress(COORDS))
        return;

    m_bDragPending      = true;
    {
        const auto ZONE     = golemBarZone(COORDS, g_pBarsState->buttons.size(), assignedBoxGlobal().w, GOLEM_BAR_HEIGHT, borderBelow()).kind;
        m_bResizeFromCorner = ZONE == SGolemBarZone::CORNER_LEFT || ZONE == SGolemBarZone::CORNER_RIGHT;
    }
    // The hand closes ON THE PRESS, not when motion starts: taking hold is
    // the press, the drag is just where the hand goes afterwards — on the
    // corner grip too (the diagonal arrow is the hover hint, the closed hand
    // is the hold; see `golemResizeHandInit` for the edge resizes).
    g_barCursorOwner = this;
    setBarCursor("grabbing");
}

void CGolemBar::handleUpEvent(Event::SCallbackInfo& info, uint32_t button, bool valid) {
    // Only the release of the press WE OWN — same button, swallowed by this bar
    // — is ours: swallowed too (the compositor never saw its press; it would
    // drop the release anyway) and it ends the press whatever happened to
    // focus or validity in between (a drag can land focus elsewhere, the
    // press can park the window; cleanup is unconditional — the "open hand
    // gets stuck" of 2026-09-15, and the phantom button of 2026-09-26). Any
    // other release passes through whole: the compositor is pairing it.
    if (!m_bCancelledDown || button != m_pressedButton)
        return;
    info.cancelled   = true;
    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        Log::logger->log(Log::DEBUG, "[golembar] drag ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;

    // Let go: the hand reopens over the strip, points over a button, or goes
    // back to the arrow if the release landed elsewhere (or off this bar's
    // world entirely).
    if (g_barCursorOwner != this)
        return;
    if (!valid) {
        releaseCursorIfOwner();
        return;
    }
    switch (hoverZone()) {
        case HOVER_STRIP: setBarCursor("grab"); break;
        case HOVER_BUTTON: setBarCursor("pointer"); break;
        case HOVER_CORNER_LEFT: setBarCursor("nw-resize"); break;
        case HOVER_CORNER_RIGHT: setBarCursor("ne-resize"); break;
        case HOVER_NONE:
            g_barCursorOwner = nullptr;
            setBarCursor(nullptr);
            break;
    }
}

void CGolemBar::handleMovement() {
    // A press on the corner grip resizes; anywhere else on the strip moves.
    // The compositor's resize picks its corner from the pointer's quadrant —
    // top-right here, by construction.
    g_pKeybindManager->changeMouseBindMode(m_bResizeFromCorner ? MBIND_RESIZE : MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[golembar] drag started on {:x}", (uintptr_t)m_pWindow.lock().get());
}

bool CGolemBar::doButtonPress(Vector2D COORDS) {
    // A press anywhere in a button's spot is that button's — the same spot
    // the hand and the hover lift answer to (`golemBarZone`).
    const auto ZONE = golemBarZone(COORDS, g_pBarsState->buttons.size(), assignedBoxGlobal().w, GOLEM_BAR_HEIGHT, borderBelow());
    const auto PWINDOW = m_pWindow.lock();
    // The card button: waverunner owns the card and which windows it is on
    // for; it answers with `hl.plugin.waveview.card(addr, on)`, which is what
    // turns the button orange (`Bars::setCard`).
    if (ZONE.kind == SGolemBarZone::CARD) {
        sendWaverunner(std::format("card toggle 0x{:x}\n", (uintptr_t)PWINDOW.get()));
        return true;
    }
    if (ZONE.kind != SGolemBarZone::BUTTON)
        return false;
    const int col = ZONE.button;

    auto&      b       = g_pBarsState->buttons[col];
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
        // Minimize to the dock — the plugin's own machinery (main.cpp):
        // it owns the card capture, the fly animation and the
        // special-workspace park, and tells the daemon when the card
        // lands so the dock entry appears as the window arrives.
        case GOLEM_BAR_MIN: golemMinimize(PWINDOW); break;
    }
    return true;
}

/// (Re)render the title texture, TIGHT — sized to the words, not to the room
/// they have — so the caller can truly centre it. `renderText` pads a
/// width-capped texture out to that width with the glyphs at the LEFT, which
/// is why the first cut of a centred title measured ~360px off centre; a
/// tight texture is the only one whose middle is the words' middle. Only a
/// title too wide for its room takes the capped (ellipsised, padded) render,
/// and that one FILLS the room, so its middle is the room's middle anyway.
///
/// The room is symmetric: the button row's span (its inset and trailing gap
/// included, `golemButtonX` one past the end) is kept clear on BOTH sides, so
/// the centre of the title is the centre of the BAR, not of the leftovers.
void CGolemBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const int scaledSize = static_cast<int>(std::round(GOLEM_TITLE_SIZE * scale));
    const int side       = static_cast<int>(std::round(std::max(golemButtonX(g_pBarsState->buttons.size(), scale), GOLEM_BAR_PADDING * scale)));
    const int maxWidth   = static_cast<int>(bufferSize.x) - 2 * side;

    if (m_szLastTitle.empty() || maxWidth < 1) {
        m_pTextTex = nullptr;
        return;
    }

    // `m_lastInk` is set by the caller, which is also what decides when this
    // has to run again.
    m_pTextTex = g_pHyprRenderer->renderText(m_szLastTitle, m_lastInk, scaledSize, false, GOLEM_BAR_FONT, 0);
    if (m_pTextTex && m_pTextTex->m_size.x > maxWidth)
        m_pTextTex = g_pHyprRenderer->renderText(m_szLastTitle, m_lastInk, scaledSize, false, GOLEM_BAR_FONT, maxWidth);
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

/// The `i`th disc's box as it is DRAWN: scaled, then rounded to whole
/// pixels. The glyph centres on this same box, so the two can never disagree
/// by the half pixel that `scaledButtonSize` (25.6 at Golem's scale) leaves.
static CBox golemDiscBox(const CBox& barBox, size_t i, float border, float scale) {
    const auto scaledButtonSize = GOLEM_BUTTON_SIZE * scale;
    CBox       box = {barBox.x + golemButtonX(i, scale), barBox.y + golemButtonY(barBox.h, border * scale, scaledButtonSize), scaledButtonSize, scaledButtonSize};
    box.round();
    return box;
}

void CGolemBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const auto visibleCount = getVisibleButtonCount(Vector2D{barBox->w, barBox->h}, scale);
    const auto BORDER       = borderBelow();

    for (size_t i = 0; i < visibleCount; ++i) {
        auto& button = g_pBarsState->buttons[i];

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

        CBox buttonBox = golemDiscBox(*barBox, i, BORDER, scale);

        // The hovered disc grows by GOLEM_BUTTON_HOVER_SCALE about its centre.
        // Drawn from a separate box so the glyph below keeps its whole-pixel
        // geometry (and its centre, which is the same point).
        CBox discDraw = buttonBox;
        if ((m_iButtonHoverState >> i) & 1u) {
            const double grow = buttonBox.w * (GOLEM_BUTTON_HOVER_SCALE - 1.0);
            discDraw.x -= grow / 2.0;
            discDraw.y -= grow / 2.0;
            discDraw.w += grow;
            discDraw.h += grow;
        }
        g_pHyprOpenGL->renderRect(discDraw, color, {.round = static_cast<int>(std::ceil(discDraw.w / 2.0)), .roundingPower = 2.F});

        // DRAWN glyphs (see `eGolemGlyph`): rectangles on the disc, in the
        // disc's parity so they centre exactly. The square is an outline: its
        // ink square, then the disc's own colour (hover lift and all) laid
        // back over the inside.
        if (button.glyph == GOLEM_GLYPH_TEXT)
            continue;
        auto ink = button.fgcol;
        ink.a *= a;
        const double w      = buttonBox.w; // whole pixels
        double       span   = std::round(w * GOLEM_GLYPH_SPAN);
        if (std::fmod(span, 2.0) != std::fmod(w, 2.0))
            span += 1.0; // same parity as the disc → equal whole margins
        const double stroke = std::max(1.0, std::round(GOLEM_GLYPH_STROKE * scale));
        const double sx     = buttonBox.x + (w - span) / 2.0;
        if (button.glyph == GOLEM_GLYPH_SQUARE) {
            const double sy = buttonBox.y + (w - span) / 2.0;
            // Slightly rounded corners (GOLEM_GLYPH_SQUARE_R); the inner cut-out's
            // radius is the outer's less the stroke, so the outline stays even.
            const int    rOut = static_cast<int>(std::round(GOLEM_GLYPH_SQUARE_R * scale));
            const int    rIn  = std::max(0, rOut - static_cast<int>(stroke));
            g_pHyprOpenGL->renderRect(CBox{sx, sy, span, span}, ink, {.round = rOut, .roundingPower = 2.F});
            if (span > 2.0 * stroke)
                g_pHyprOpenGL->renderRect(CBox{sx + stroke, sy + stroke, span - 2.0 * stroke, span - 2.0 * stroke}, color, {.round = rIn, .roundingPower = 2.F});
        } else if (button.glyph == GOLEM_GLYPH_DASH) {
            double th = stroke;
            if (std::fmod(th, 2.0) != std::fmod(w, 2.0))
                th += 1.0;
            g_pHyprOpenGL->renderRect(CBox{sx, buttonBox.y + (w - th) / 2.0, span, th}, ink, {});
        }
    }

    // The card button, at the other end (see GOLEM_CARD_RIGHT). Off: only
    // its glyph, in the bar's ink, with a faint disc under the hand. On: the
    // card's orange disc, lifted under the hand like the three on the left.
    if (barBox->w < GOLEM_CARD_MIN_BAR * scale)
        return;
    const auto scaledSize = GOLEM_BUTTON_SIZE * scale;
    CBox       box = {barBox->x + golemCardX(barBox->w / scale, scale), barBox->y + golemButtonY(barBox->h, BORDER * scale, scaledSize), scaledSize, scaledSize};
    box.round();
    CHyprColor disc = CHyprColor{GOLEM_CARD_ON};
    CHyprColor ink;
    if (m_cardOn) {
        if (m_cardHover) {
            disc.r += (1.0 - disc.r) * GOLEM_BUTTON_HOVER_LIFT;
            disc.g += (1.0 - disc.g) * GOLEM_BUTTON_HOVER_LIFT;
            disc.b += (1.0 - disc.b) * GOLEM_BUTTON_HOVER_LIFT;
        }
        ink = CHyprColor{0xFF15121AULL}; // the mockup's dark on orange
    } else {
        disc   = m_lastInk;
        disc.a = m_cardHover ? GOLEM_CARD_HOVER_BG : 0.F;
        ink    = m_lastInk;
        ink.a  = m_cardHover ? 1.F : GOLEM_CARD_OFF_INK;
    }
    disc.a *= a;
    ink.a *= a;
    if (disc.a > 0.F)
        g_pHyprOpenGL->renderRect(box, disc, {.round = static_cast<int>(std::ceil(box.w / 2.0)), .roundingPower = 2.F});
    // The glyph: the card as a frame with two dividers (the mockup's three
    // columns), drawn — whole pixels, the disc's parity, centred by
    // construction.
    const double w      = box.w;
    double       gw     = std::round(w * 0.62);
    if (std::fmod(gw, 2.0) != std::fmod(w, 2.0))
        gw += 1.0;
    double       gh     = std::round(w * 0.5);
    if (std::fmod(gh, 2.0) != std::fmod(w, 2.0))
        gh += 1.0;
    const double stroke = std::max(1.0, std::round(GOLEM_GLYPH_STROKE * scale));
    const double gx = box.x + (w - gw) / 2.0, gy = box.y + (w - gh) / 2.0;
    // Four sides, then the two dividers: strokes only, so nothing has to be
    // cut back out in the disc's colour (off, there is no disc to cut with).
    g_pHyprOpenGL->renderRect(CBox{gx, gy, gw, stroke}, ink, {});
    g_pHyprOpenGL->renderRect(CBox{gx, gy + gh - stroke, gw, stroke}, ink, {});
    g_pHyprOpenGL->renderRect(CBox{gx, gy + stroke, stroke, gh - 2.0 * stroke}, ink, {});
    g_pHyprOpenGL->renderRect(CBox{gx + gw - stroke, gy + stroke, stroke, gh - 2.0 * stroke}, ink, {});
    const double third = std::round((gw - stroke) / 3.0);
    for (int i = 1; i <= 2; ++i)
        g_pHyprOpenGL->renderRect(CBox{gx + third * i, gy + stroke, stroke, gh - 2.0 * stroke}, ink, {});
}

void CGolemBar::setCard(bool on) {
    if (m_cardOn == on)
        return;
    m_cardOn = on;
    damageEntire();
}

// The centre of a text texture's INK relative to the texture's centre, from
// its alpha (glyph coverage). Read back through an FBO of our own, mid-pass,
// with the compositor's framebuffer binding put back after — once per glyph
// texture, so the stall is paid three times a session, not per frame.
static bool measureInkOffset(const SP<Render::ITexture>& tex, Vector2D& offset) {
    if (!tex || tex->m_texID == 0 || tex->m_size.x < 1 || tex->m_size.y < 1)
        return false;
    const int            W = (int)tex->m_size.x, H = (int)tex->m_size.y;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    GLint                prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->m_texID, 0);
    while (glGetError() != GL_NO_ERROR) {}
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const bool readOk = ok && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glDeleteFramebuffers(1, &fbo);
    if (!readOk) {
        golemTrace(std::format("ink-measure: readback failed (fbo complete={}, tex {}x{})", ok, W, H));
        return false;
    }
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (px[((size_t)y * W + x) * 4 + 3] > 24) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (maxX < 0) {
        golemTrace(std::format("ink-measure: no ink (tex {}x{})", W, H));
        return false;
    }
    // pixel centres: the ink spans [minX, maxX] inclusive
    offset = Vector2D((minX + maxX + 1) / 2.0 - W / 2.0, (minY + maxY + 1) / 2.0 - H / 2.0);
    golemTrace(std::format("ink-measure: tex {}x{} ink x[{},{}] y[{},{}] → offset ({:.1f},{:.1f})", W, H, minX, maxX, minY, maxY, offset.x, offset.y));
    return true;
}

void CGolemBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const auto visibleCount = getVisibleButtonCount(Vector2D{barBox->w, barBox->h}, scale);
    const auto BORDER       = borderBelow();

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pBarsState->buttons[i];
        const auto scaledButtonSize = GOLEM_BUTTON_SIZE * scale;

        if (button.glyph != GOLEM_GLYPH_TEXT)
            continue; // drawn with its disc (renderBarButtons)

        if ((!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty()) {
            button.iconTex     = g_pHyprRenderer->renderText(button.icon, button.fgcol, std::round(GOLEM_BUTTON_SIZE * GOLEM_BUTTON_GLYPH * scale), false, GOLEM_BAR_FONT, scaledButtonSize);
            button.inkOffset   = {};
            button.inkMeasured = measureInkOffset(button.iconTex, button.inkOffset);
        }

        if (!button.iconTex || button.iconTex->m_texID == 0)
            continue;

        // Centred on the disc AS DRAWN (`golemDiscBox`, rounded) — and by its
        // INK, not its texture: the offset measured above moves the texture
        // so the glyph's own centre lands on the disc's centre (see
        // `SGolemButton::inkOffset`).
        const CBox disc  = golemDiscBox(*barBox, i, BORDER, scale);
        const auto iconX = disc.x + disc.w / 2.0 - button.iconTex->m_size.x / 2.0 - button.inkOffset.x;
        const auto iconY = disc.y + disc.h / 2.0 - button.iconTex->m_size.y / 2.0 - button.inkOffset.y;
        CBox       pos   = {std::round(iconX), std::round(iconY), button.iconTex->m_size.x, button.iconTex->m_size.y};

        // Never DIMMED: `decoration:dim_inactive` tints every texture drawn
        // for an unfocused window (`allowDim` defaults on), which greyed the
        // glyphs on every card but the focused one while their discs — plain
        // rects, untinted — stayed bright. The buttons are targets at full
        // strength (Max, 2026-09-25: *"the title bars buttons should not be
        // dimmed"*), same rule as their opacity above.
        g_pHyprOpenGL->renderTexture(button.iconTex, pos, {.a = a, .allowDim = false});
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
    // A translucent strip wants the window's blur behind it, and the pass has
    // to know before it draws anything. Same decision the compositor makes for
    // the content: a floating window blurs the live framebuffer; xray windows
    // and tiled ones use the precomputed blur of the layers beneath.
    if (m_cRealBarColor && m_cRealBarColor->value().a * a < 1.F) {
        const bool XRAY     = g_pHyprRenderer->shouldUseNewBlurOptimizations(nullptr, PWINDOW);
        data.liveBlur       = !XRAY;
        data.precomputeBlur = XRAY;
    }
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
    // window itself rather than from the decoration box.
    //
    // ⭐ ROUNDED THE WAY THE CONTENT IS, not floored (Max, 2026-09-26: *"there
    // is a tiny gap between the window and the titlebar"*). The compositor
    // rounds the content's box to the nearest device pixel (`CBox::round` in
    // the surface draw); a floor here left the strip a row short whenever the
    // window's top edge fell past .5 — and with no border on a float any more,
    // nothing painted that row. Same for the decoration box's own rounding:
    // the cut is SET, not min'd, so a short clip is stretched to the seam (the
    // slab behind it is drawn tall on purpose). The border's thickness is
    // rounded like `renderBorder` rounds it. Abutting, to the pixel.
    const double CONTENTTOP = std::round((PWINDOW->m_realPosition->value().y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y) * pMonitor->m_scale);
    const double BORDEROUT  = CONTENTTOP - std::round(PWINDOW->getRealBorderSize() * pMonitor->m_scale);
    if (BORDEROUT > clipBox.y)
        clipBox.h = BORDEROUT - clipBox.y;

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
    //
    // ⭐ SINCE THE STRAIGHT SEAM (2026-09-26) there is normally no wedge at all:
    // the compositor squares the window's top corners while the bar shows
    // (`windowSquareTop`), so the border and the content meet the strip flat
    // and this fill would only be ~70 rects of damage a frame for nothing. It
    // stays as the fallback for a window whose corners are still round.
    if (!windowSquareTop(PWINDOW)) {
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
    //
    // ⭐ THE SAME OPACITY AS THE WINDOW (Max, 2026-09-26: *"make it also the
    // same opacity"*). Equal alpha alone did not read as equal: the content
    // sits on Hyprland's blur, the strip sat on the sharp wallpaper, and 5% of
    // a sharp wallpaper reads as more see-through than 5% of a frosted one.
    // So a translucent strip is drawn the way the compositor draws the content
    // itself — a solid-colour texture through `renderTexture` with blur — so
    // the blurred backdrop, its rounding and the alpha come from the very code
    // path the window's own pixels take (`IElementRenderer::drawTex`,
    // mirrored here: live blur of the framebuffer for a float, the precomputed
    // blur for xray/tiled).
    if (color.a < 1.F && ensureSolidTex(color)) {
        const bool XRAY = g_pHyprRenderer->shouldUseNewBlurOptimizations(nullptr, PWINDOW);
        // `a` is the window's opacity × its fade. The content's blur takes the
        // FADE alone as `blurA` (its opacity is already in `.a`), so divide the
        // opacity back out — or the backdrop under the strip fades twice.
        const float opacity = PWINDOW->m_ruleApplicator->opaque().valueOrDefault() ? 1.F : PWINDOW->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
        const float fade    = opacity > 0.001F ? std::clamp(a / opacity, 0.F, 1.F) : 1.F;
        Render::GL::CHyprOpenGLImpl::STextureRenderData td;
        td.blur                  = true;
        td.blurA                 = fade;
        td.overallA              = 1.F;
        td.blockBlurOptimization = !XRAY;
        if (XRAY)
            td.blurredBG = pMonitor->resources()->m_blurFB->getTexture();
        else {
            // Only the strip (and only its damaged part) is blurred — the
            // overhang below the seam is under the content, which does its own.
            CRegion blurRegion = barDamage.copy().intersect(g_pHyprRenderer->m_renderData.damage);
            td.blurredBG       = g_pHyprRenderer->blurMainFramebuffer(color.a, &blurRegion);
        }
        td.damage        = &barDamage;
        td.a             = color.a;
        td.round         = scaledRounding;
        td.roundingPower = m_pWindow->roundingPower();
        td.discardMode   = 0;     // a solid texel: nothing to discard, no stencil pass
        td.allowDim      = false; // the strip dims itself (the inactive stop); the compositor's tint must not dim it twice
        g_pHyprOpenGL->renderTexture(g_barSolidTex, titleBarBox, td);
    } else
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.damage = &barDamage, .round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // ⭐ THE HAIRLINE (see GOLEM_CARD_HAIRLINE_*): Beam's border around the whole
    // card. The compositor's border shader draws a ring OUTSIDE the box it is
    // given, so the box is the card itself — strip top to content bottom — and
    // the ring hugs the card's outer edge, corners following the same radius
    // as the strip's top and the content's bottom. Focus picks the alpha the
    // way Beam's rule does; the window's own fade (`a`) rides along.
    {
        CBox card = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w, DECOBOX.h + PWINDOW->m_realSize->value().y};
        card.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();
        const bool                 FOCUSED = Desktop::focusState()->window() == PWINDOW;
        Config::CGradientValueData hair    = PWINDOW->m_realBorderColor; // structure only (angle, the ok cache)
        hair.m_colors.clear();
        hair.m_colors.push_back(CHyprColor{FOCUSED ? GOLEM_CARD_HAIRLINE_FOCUSED : GOLEM_CARD_HAIRLINE_UNFOCUSED});
        hair.updateColorsOk();
        g_pHyprOpenGL->renderBorder(card, hair, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .borderSize = GOLEM_CARD_HAIRLINE_PX, .a = a});
    }

    // THE TITLE, BACK AND CENTRED (Max, 2026-09-15: *"put the title on the
    // title bar, on the center"* — it left the bar earlier the same day, and
    // its old seat was the LEFT). Re-rendered only when what it shows or the
    // room it has changes: the name, the ink (the bar colour animates with
    // focus, and the ink flips at its threshold), or the bar's width.
    if (m_szLastTitle != PWINDOW->m_title || !m_pTextTex || m_pTextTex->m_texID == 0 || inkOn(color) != m_lastInk || m_lastBarW != static_cast<int>(BARBUF.x)) {
        m_szLastTitle = PWINDOW->m_title;
        m_lastInk     = inkOn(color);
        m_lastBarW    = static_cast<int>(BARBUF.x);
        renderBarTitle(BARBUF, pMonitor->m_scale);
    }

    // Truncated to whole pixels (upstream's), stated as doubles so the box's
    // own type does not have to narrow them.
    CBox barBox = {titleBarBox.x, titleBarBox.y, static_cast<double>(static_cast<int>(BARBUF.x)), static_cast<double>(static_cast<int>(BARBUF.y))};

    if (m_pTextTex) {
        // Centred on the bar's own middle — the texture is tight (see
        // `renderBarTitle`), so the texture's middle IS the words' middle —
        // and on the buttons' optical line: centred in the bar PLUS the top
        // border, like `golemButtonY`, or the words read as sitting high.
        const auto xOffset  = std::round((BARBUF.x - m_pTextTex->m_size.x) / 2.0);
        const auto yOffset  = std::round(golemButtonY(BARBUF.y, borderBelow() * pMonitor->m_scale, m_pTextTex->m_size.y));
        CBox       titleBox = {barBox.x + xOffset, barBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        // Same damage clip as the bar, so a long title's ellipsised texture
        // can never trail glyphs past the strip.
        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.damage = &barDamage, .a = a});
    }

    renderBarButtons(&barBox, pMonitor->m_scale, a);

    renderBarButtonsText(&barBox, pMonitor->m_scale, a);
}

std::string CGolemBar::debugLine() const {
    if (!m_bDragPending && !m_bDraggingThis && !m_bCancelledDown)
        return {};
    const auto w = m_pWindow.lock();
    return std::format("[{:#x} hidden={} dragPending={} dragging={} cancelledDown={}]", (uintptr_t)w.get(), m_hidden,
                       m_bDragPending, m_bDraggingThis, m_bCancelledDown);
}

void CGolemBar::settleColor() {
    const auto PWINDOW = m_pWindow.lock();
    if (!PWINDOW || !m_cRealBarColor)
        return;
    *m_cRealBarColor = barColor(PWINDOW);
    m_cRealBarColor->warp();
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
    // The whole card plus the hairline outside it (see `cardBoxGlobal`), so a
    // focus flip repaints the ring on every side.
    CBox box = cardBoxGlobal();
    box.h += notchDepth();
    box.expand(GOLEM_CARD_HAIRLINE_PX + 1);
    g_pHyprRenderer->damageBox(box);
}

CBox CGolemBar::cardBoxGlobal() {
    CBox box = assignedBoxGlobal();
    if (const auto PWINDOW = m_pWindow.lock())
        box.h += PWINDOW->m_realSize->value().y;
    return box;
}

// How far below the seam the notch fill reaches: the corner's full span, where
// the window's outer curve meets its side. In logical px, like the boxes here.
// Zero while the compositor squares the corner (`windowSquareTop`): the fill
// is not drawn, so damage and occlusion stop at the strip.
double CGolemBar::notchDepth() {
    const auto PWINDOW = m_pWindow.lock();
    return PWINDOW && !windowSquareTop(PWINDOW) ? PWINDOW->rounding() + PWINDOW->getRealBorderSize() : 0.0;
}

Vector2D CGolemBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

bool CGolemBar::bare() {
    const auto W = m_pWindow.lock();
    return m_hidden && W && windowIsFirefox(W) && W->m_isFloating && !W->isFullscreen() && !windowStaged(W);
}

Vector2D CGolemBar::cursorRelativeToWindow() {
    const auto W = m_pWindow.lock();
    return W ? g_pInputManager->getMouseCoordsInternal() - W->m_realPosition->value() : Vector2D{-1, -1};
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
    bool         card = false;
    if (!m_hidden && inputIsValid()) {
        const auto COORDS = cursorRelativeToBar();
        // The whole column lights its disc (`golemButtonColumn`), so the lift
        // answers the hand as it arrives, not only once it is on the disc.
        const auto ZONE = golemBarZone(COORDS, g_pBarsState->buttons.size(), assignedBoxGlobal().w, GOLEM_BAR_HEIGHT, borderBelow());
        if (ZONE.kind == SGolemBarZone::BUTTON)
            mask = 1u << ZONE.button;
        card = ZONE.kind == SGolemBarZone::CARD;
    }
    if (mask != m_iButtonHoverState || card != m_cardHover) {
        m_iButtonHoverState = mask;
        m_cardHover         = card;
        damageEntire();
    }
}
