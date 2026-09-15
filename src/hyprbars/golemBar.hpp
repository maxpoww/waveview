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
/// smaller"*). Still clears the 16px buttons centred in it with air to spare.
inline constexpr int GOLEM_BAR_HEIGHT = 26;
/// Air at each end of the bar.
inline constexpr int GOLEM_BAR_PADDING = 10;
/// Air between the buttons.
inline constexpr int GOLEM_BUTTON_PADDING = 7;
/// Button diameter (Max, 2026-09-13: *"make them little bigger"*, then *"slightly
/// bigger"* again). In a 26px bar that leaves 4px of air above and below.
inline constexpr float GOLEM_BUTTON_SIZE = 18;
/// How far the button row starts from the bar's left edge — its OWN inset, not
/// the bar's end padding, so the buttons can sit in from the corner without
/// moving the air at the bar's ends (Max, 2026-09-13: *"move them a little to
/// the right"*). The title starts after the row, so it follows this for free.
inline constexpr int GOLEM_BUTTONS_LEFT = 16;

/// How far the `i`th button's left edge sits from the bar's left edge, at
/// `scale`. **The buttons live at the LEFT** (Max, 2026-09-13: *"move the
/// buttons of the bars to the left"*), and the title starts after them.
///
/// The one place the row's geometry is worked out. It used to be re-derived
/// from the right edge at four call sites — the rect, the icon, the icon's
/// hover box and the hit-test — which is how a button ends up drawn in one
/// place and clicked in another.
inline float golemButtonX(size_t i, float scale) {
    return (GOLEM_BUTTONS_LEFT + i * (GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING)) * scale;
}

/// The width the whole button row occupies, padding on both sides of it
/// included — what the title has to start after.
inline float golemButtonsWidth(size_t count) {
    return count == 0 ? 0.F : count * (GOLEM_BUTTON_SIZE + GOLEM_BUTTON_PADDING);
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
/// Title size in logical px, and the family — the bar's own font, matching the
/// rest of the shell rather than the compositor's default.
inline constexpr int         GOLEM_TITLE_SIZE = 14;
inline constexpr const char* GOLEM_TITLE_FONT = "Sans";

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
    SP<Render::ITexture> m_pTextTex;

    bool                 m_bWindowSizeChanged = false;
    /// The bar is only for windows OUT of the layout. Upstream drove this from
    /// a `hyprbars:no_bar` window rule; Golem asks the window itself.
    bool                 m_hidden         = true;
    bool                 m_bButtonHovered = false;
    bool                 m_bButtonsDirty  = true;

    PHLANIMVAR<CHyprColor> m_cRealBarColor;

    /// Re-read whether this window is floating, and tell the positioner when
    /// the answer changes — a bar that appears or disappears changes how much
    /// space the window has.
    void     syncHidden();

    Vector2D cursorRelativeToBar();

    void     renderPass(PHLMONITOR, float const& a);
    void     renderBarTitle(const Vector2D& bufferSize, const float scale);
    void     renderBarButtons(CBox* barBox, const float scale, const float a);
    void     renderBarButtonsText(CBox* barBox, const float scale, const float a);
    void     damageOnButtonHover();
    /// The window's top border in logical px (see `golemButtonY`).
    float    borderBelow();

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

    std::string         m_szLastTitle;
    /// The ink the cached title texture was drawn in. The bar wears the border's
    /// colour, which changes with focus, so the title has to be re-rendered when
    /// the contrast flips — otherwise dark text stays on a dark bar.
    CHyprColor          m_lastInk = CHyprColor{0ULL};

    bool                m_bDraggingThis  = false;
    bool                m_bDragPending   = false;
    bool                m_bCancelledDown = false;

    /// Hover state per button, as a bitfield.
    unsigned int m_iButtonHoverState = 0;

    size_t       getVisibleButtonCount(const Vector2D& bufferSize, const float scale);

    friend class CBarPassElement;
};
