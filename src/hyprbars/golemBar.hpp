#pragma once

// Golem's titlebar for floating windows.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md for the upstream tag and every change.

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include "barsGlobals.hpp"

// Upstream reaches into the input manager's privates; we inherit the hack. It
// is contained to this header and fails loudly (at compile time) if it ever
// stops being true.
#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private

namespace Event {
    struct SCallbackInfo;
}

// ─────────────────────────────── The look ───────────────────────────────
//
// Constants, not config. Golem has one titlebar; a knob here would be a knob
// nobody turns, and the values want to be read next to each other anyway.

/// Bar height in logical px (Max, 2026-09-13: *"make the bar a little
/// smaller"*, then 2026-09-15: *"make the bar thinner"* once the title left
/// and the strip had only the button row to hold).
///
/// ⚠️ The floor for an 18px button is **22**: the buttons are centred in the
/// bar PLUS the window's top border (`golemButtonY`), so 22 + 3 leaves 3.5px
/// of air above and below the disc. Going thinner means shrinking
/// [`GOLEM_BUTTON_SIZE`] with it, or the discs start touching the window.
inline constexpr int GOLEM_BAR_HEIGHT = 22;
/// Air at each end of the bar.
inline constexpr int GOLEM_BAR_PADDING = 10;
/// Air between the buttons.
inline constexpr int GOLEM_BUTTON_PADDING = 7;
/// Button diameter. Tuned by eye over two sessions: 14 → 16 → 18 (Max,
/// 2026-09-13: *"make them little bigger"*, then *"slightly bigger"*), then
/// 15 and back up to **17** on 2026-09-15 once the bar came down to 22px and
/// the title was gone (*"that is too small, make them 17"*). In a 22px bar
/// that leaves 2.5px of air above and below — the discs are the only thing in
/// the strip, so they set its rhythm.
inline constexpr float GOLEM_BUTTON_SIZE = 17;
/// How far the button row starts from the bar's left edge — its OWN inset, not
/// the bar's end padding, so the buttons can sit in from the corner without
/// moving the air at the bar's ends (Max, 2026-09-13: *"move them a little to
/// the right"*, and again 2026-09-15 once the title was gone and the row stood
/// alone).
inline constexpr int GOLEM_BUTTONS_LEFT = 7;

/// How much of its opacity a window's frame keeps while it FLOATS (Max,
/// 2026-09-15: *"make the border and the bar more transparent only on
/// floating"*). Tiled windows are untouched.
///
/// A FACTOR rather than a fixed alpha, so the difference the compositor
/// animates between the focused and unfocused border survives the tint —
/// Golem's inactive border is already translucent, and flattening both to one
/// value would throw that away. The bar wears the border's colour, so tinting
/// the border is all it takes: the strip and its corner fill follow for free.
///
/// The BUTTONS are deliberately outside this: they carry their own colours at
/// full strength (`renderBarButtons`), because they are what the hand aims at.
inline constexpr float GOLEM_FLOAT_ALPHA = 0.78F;

/// How far the `i`th button's left edge sits from the bar's left edge, at
/// `scale`. **The buttons live at the LEFT** (Max, 2026-09-13: *"move the
/// buttons of the bars to the left"*).
///
/// The one place the row's geometry is worked out. It used to be re-derived
/// from the right edge at four call sites — the rect, the icon, the icon's
/// hover box and the hit-test — which is how a button ends up drawn in one
/// place and clicked in another.
inline float golemButtonX(size_t i, float scale) {
    return (GOLEM_BUTTONS_LEFT + i * (GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING)) * scale;
}

/// How far below the bar's top edge a button sits.
///
/// Centred in the bar **plus the window's top border**, not in the bar alone:
/// the bar wears the border's own colour (see `barColor`), so the two are one
/// unbroken strip to the eye, and a button centred in the bar reads as sitting
/// high. Measured before this: 4.4px of air above, 7.5px below (Max,
/// 2026-09-13: *"move the buttons down (same padding on top than on the
/// button)"*).
///
/// All three arguments must be in the SAME space — scaled for the render path,
/// logical for the hit-test.
inline float golemButtonY(float barHeight, float border, float buttonSize) {
    return (barHeight + border - buttonSize) / 2.F;
}

/// The family the button glyphs are set in — the shell's own font rather than
/// the compositor's default. (It carried the title too, until the title was
/// dropped; the glyphs still need a name to render with.)
inline constexpr const char* GOLEM_BAR_FONT = "Sans";

/// How far a hovered button's disc lifts toward white, 0..1. Strong enough to
/// answer the hand at a glance, weak enough that the disc keeps its identity —
/// the red stays red, the peach stays peach.
inline constexpr float GOLEM_BUTTON_HOVER_LIFT = 0.22F;

/// The cursor-hint settle timer's lifecycle — called from `Bars::init` and
/// `Bars::shutdown`. Defined in golemBar.cpp beside the state it re-asserts.
void golemBarCursorTimerInit();
void golemBarCursorTimerDrop();

class CGolemBar : public IHyprWindowDecoration {
  public:
    CGolemBar(PHLWINDOW);
    virtual ~CGolemBar();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(PHLMONITOR, float const& a);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();

    PHLWINDOW    getOwner();

    /// How far below the strip the notch fill reaches (logical px). Public
    /// because the pass element's occlusion box must cover the fill too.
    double       notchDepth();

    WP<CGolemBar> m_self;

  private:
    SBoxExtents          m_seExtents;
    PHLWINDOWREF         m_pWindow;
    CBox                 m_bAssignedBox;

    /// The bar is only for windows OUT of the layout. Upstream drove this from
    /// a `hyprbars:no_bar` window rule; Golem asks the window itself.
    bool                 m_hidden         = true;
    bool                 m_bButtonsDirty  = true;

    PHLANIMVAR<CHyprColor> m_cRealBarColor;

    /// True while this window carries our translucent-frame override.
    bool                       m_tinted = false;
    /// The tinted gradients as last applied. Kept so the tint is re-pushed
    /// ONLY when the colours actually change — the daemon's screen-following
    /// pass rewrites the config gradients, and re-applying every frame would
    /// restart the border's colour animation every frame and never settle.
    Config::CGradientValueData m_tintActive, m_tintInactive;

    /// Give a FLOATING window a translucent frame, or hand its own colours
    /// back. Reads the live `general:col.*_border` gradients — the very ones
    /// the desktop draws from, including whatever waverunner pushed a moment
    /// ago — scales their alpha by [`GOLEM_FLOAT_ALPHA`] and sets the result
    /// as a per-window override, so the tint follows the colour pass instead
    /// of freezing a snapshot of it.
    void     syncFloatTint();

    /// The pending deferred `updateDecorationValues()`, as a cancellable lock:
    /// destroyed with the bar, so the callback can never outlive it (or the
    /// plugin). Also the coalescing — while it exists, no second one queues.
    UP<SEventLoopDoLaterLock> m_refreshLock;

    /// Apply the overrides on the event loop, never inline — see the note on
    /// the definition. Public-ish only to the class; the lambda re-locks
    /// `m_self`, so a bar destroyed before it runs is a no-op.
    void     refreshDecorationsLater();

    /// Re-read whether this window is floating, and tell the positioner when
    /// the answer changes — a bar that appears or disappears changes how much
    /// space the window has.
    void     syncHidden();

    Vector2D cursorRelativeToBar();

    void     renderPass(PHLMONITOR, float const& a);
    void     renderBarButtons(CBox* barBox, const float scale, const float a);
    void     renderBarButtonsText(CBox* barBox, const float scale, const float a);
    void     damageOnButtonHover();
    /// The window's top border in logical px (see `golemButtonY`).
    float    borderBelow();

    /// Where the pointer stands on this bar. Feeds the cursor hint: the strip
    /// is draggable (open hand), a button is pressable (index finger).
    enum eBarHover : uint8_t {
        HOVER_NONE = 0,
        HOVER_STRIP,
        HOVER_BUTTON,
    };
    eBarHover hoverZone();

    /// Give the pointer its arrow back, if this bar is the one holding it.
    void     releaseCursorIfOwner();

    bool     inputIsValid();
    void     onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e);
    void     onMouseMove(Vector2D coords);

    void     handleDownEvent(Event::SCallbackInfo& info);
    void     handleUpEvent(Event::SCallbackInfo& info);
    void     handleMovement();
    bool     doButtonPress(Vector2D COORDS);

    CBox     assignedBoxGlobal();

    CHyprSignalListener m_pMouseButtonCallback;
    CHyprSignalListener m_pMouseMoveCallback;

    bool                m_bDraggingThis  = false;
    bool                m_bDragPending   = false;
    bool                m_bCancelledDown = false;

    /// Hover state per button, as a bitfield.
    unsigned int m_iButtonHoverState = 0;

    size_t       getVisibleButtonCount(const Vector2D& bufferSize, const float scale);

    friend class CBarPassElement;
};
