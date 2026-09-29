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

/// Bar height in logical px. Started at the OPTIONS banner's height (Max,
/// 2026-09-15: *"match the title bar size (height) with the banner one"* —
/// waverunner's `config.options.height` = 28), then stepped down by eye with
/// the buttons: 28 → 26 → 24 → 25 → **26** (*"bar 26 buttons 17"*).
/// (Earlier: 2026-09-13 *"make the bar a little smaller"*, 2026-09-15 *"make
/// the bar thinner"* down to 22, then the banner match.)
///
/// ⚠️ The buttons are centred in the bar PLUS the window's top border
/// (`golemButtonY`), so 25 + 3 leaves 6px of air above and below a 16px
/// disc. Shrinking the bar means shrinking [`GOLEM_BUTTON_SIZE`] with it, or
/// the discs start touching the window.
///
/// ✗ Tried and rolled back the same day (2026-09-26): an 18px strip = button + one
/// pixel of air above and below, centred in the strip alone — Max: *"i dont like
/// it"*, then *"smaller buttons but the bar not as small as it was."* Then one
/// step down by eye: 26 → **25** (*"make the bar slightly smaller"*), 16px discs.
inline constexpr int GOLEM_BAR_HEIGHT = 25;
/// Air at each end of the bar.
inline constexpr int GOLEM_BAR_PADDING = 10;
/// Air between the buttons.
inline constexpr int GOLEM_BUTTON_PADDING = 7;
/// Button diameter. Started from the OPTIONS bar's control circles (Max,
/// 2026-09-15: *"match the button size on the title bar with the OPTIONS
/// ones"* — a glyph pill there is a perfect circle of pill height = bar 28 −
/// 2 × 2.5 margin = 23, waverunner's `options_pill_h`), then stepped down by
/// eye: 23 read too big on the strip (*"that is too big, make them little
/// smaller"*) → 21 → 20 → 19 → 16 → **17** (*"bar 25 buttons 17"*, settling
/// back up half a step). Earlier history: 14 → 16 → 18 → 15 → 17.
/// → **16** (2026-09-26: *"make the buttons slightly smaller"*, then after a
/// round trip through 17: *"i like it as it was, smaller buttons but the bar not
/// as small"*).
inline constexpr float GOLEM_BUTTON_SIZE = 16;
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

// ──────────────── The bar's colour: the window's own ────────────────
//
// The bar takes the colour of the content it sits on, the way the OPTIONS bar
// takes a flush window's (Max, 2026-09-26: *"i want the title bars to sample
// the color of the window as the option bar does (same color / one thing)"*).
// Same recipe as the daemon's `Backdrop::surface`: the sampled colour with the
// resting wash composited over it — a faint white wash on a dark window, a
// faint black one on a bright window — so the strip is "the window grown", and
// the ink measured against that same result. The numbers are the daemon's,
// copied so the two surfaces can never disagree.

/// How many logical px of the window's top edge the sample averages: the rows
/// right under the seam, which are what the strip has to continue. A toolbar
/// or tab strip there is exactly what the bar should match.
inline constexpr double GOLEM_SAMPLE_ROWS = 12.0;
/// Least time between two samples of one window (a busy terminal commits at
/// the refresh rate; the strip's colour does not need to).
inline constexpr int GOLEM_SAMPLE_MIN_MS = 120;
/// Resting washes, as the daemon's `rest_wash` (white 0.11 / black 0.10,
/// asymmetric because a white wash reads stronger than a black one at equal
/// alpha). **Both ZERO here**: the first cut carried the OPTIONS bar's lift and
/// on a near-black terminal that read as a lighter grey strip — Max,
/// 2026-09-26: *"it is not the same color."* The strip is the sampled colour,
/// exactly; the recipe stays so a lift is one number away if ever wanted.
inline constexpr float GOLEM_WASH_WHITE_A = 0.0F;
inline constexpr float GOLEM_WASH_BLACK_A = 0.0F;
/// The daemon's bright/dark flip point (WCAG: black and white contrast equally).
inline constexpr float GOLEM_BRIGHT_LUMA = 0.179F;
/// The strip's opacity is the content's own sampled alpha and nothing else:
/// the WINDOW's opacity (`decoration:active_opacity`, opacity rules, the fade)
/// arrives through the decoration's `a` — the compositor hands every
/// decoration the window's full alpha — and the strip multiplies by it once,
/// exactly as the content is. (A first cut also multiplied by a 0.95
/// constant and drew the strip at 0.95², a shade darker over any backdrop:
/// Max, 2026-09-26, *"its not the same color. is super close but its not"* —
/// 119 on screen for a grey the content showed at 125.)
/// How much an unfocused window is dimmed (`decoration:dim_strength`): the
/// strip dims by the same amount, so it stays one thing with the content
/// when the focus leaves.
inline constexpr float GOLEM_DIM_INACTIVE = 0.30F;

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

/// Which button's COLUMN a bar-local logical point falls in, or -1.
///
/// The button end of the bar is ONE exclusive zone (Max, 2026-09-26: *"i want
/// the buttons to be easier to aim… the whole spots where they are should be
/// exclusive… only index on their part of the bar, not grab"*): each button
/// owns a full-height column — its disc plus half the gap on either side, the
/// first one starting at the bar's very edge — so a pointer heading for the
/// row never drops between two discs into the strip and flips the hand from
/// index to grab and back. The hover hint, the hover lift and the press all
/// read this same test, so they can never disagree about whose spot it is.
inline int golemButtonColumn(const Vector2D& p, size_t count, double barHeight) {
    if (count == 0 || p.y < 0 || p.y >= barHeight)
        return -1;
    const double half = GOLEM_BUTTON_PADDING / 2.0;
    for (size_t i = 0; i < count; ++i) {
        const double x0 = i == 0 ? 0.0 : golemButtonX(i, 1.F) - half;
        const double x1 = golemButtonX(i, 1.F) + GOLEM_BUTTON_SIZE + half;
        if (p.x >= x0 && p.x < x1)
            return static_cast<int>(i);
    }
    return -1;
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

/// The family the title and the button glyphs are set in — the shell's own
/// font rather than the compositor's default.
inline constexpr const char* GOLEM_BAR_FONT = "Sans";

/// Title text size (pt at scale 1). The title is back on the bar and CENTRED
/// (Max, 2026-09-15: *"put the title on the title bar, on the center"*) after
/// a spell away earlier the same day — and its old seat was the LEFT, so the
/// centring is new, not restored. 14 → 16 (*"bigger text"*) → **15**.
inline constexpr int GOLEM_TITLE_SIZE = 15;

/// A FONT glyph's point size as a fraction of the disc (the ×): 0.62 read
/// small once the discs settled at 16 (Max, 2026-09-26: *"make the icon on
/// the buttons bigger"*) → 0.8 → **1.02** (from the browser mockup,
/// `~/titlebar-mockup/buttons.html`, 2026-09-26 late).
inline constexpr double GOLEM_BUTTON_GLYPH = 1.02;
/// A DRAWN glyph's span as a fraction of the disc (the square's side, the
/// dash's length), its stroke in logical px, and the square's corner radius
/// in logical px. Snapped to the disc's parity at draw time so the margins
/// come out whole and equal; the stroke and the radius round to whole device
/// pixels (at Golem's 1.6×, a stroke of 1 or 1.5 both land on 2px). Values
/// from the mockup: span 0.5 → **0.6**, stroke 1.5 → **1**, corner **1.5**.
inline constexpr double GOLEM_GLYPH_SPAN     = 0.6;
inline constexpr double GOLEM_GLYPH_STROKE   = 1.0;
inline constexpr double GOLEM_GLYPH_SQUARE_R = 1.5;

/// The glyph's ink is its own disc's colour, shifted in lightness: a pale red
/// × on red, a pale orange – on orange, a pale green □ on green. Positive =
/// LIFTED toward white by this fraction (Max, 2026-09-26: *"lets see the
/// icons lighter than the buttons"*); negative = darkened, each channel
/// scaled by (1 + value). The ink was one near-black for all three before;
/// −0.55 read fine, +0.55 (*"lighter"*) *"looks like shit"* — settled in the
/// browser mockup: −0.22 → −0.04 → −0.11 → **−0.16** (the glyph rests a little
/// darker than its disc, #d62727 on #ff2e2e, and comes forward as the disc lifts).
inline constexpr float GOLEM_GLYPH_TONE = -0.16F;

/// The bar's top corners are resize grips: this many logical px in from each
/// side, full bar height (the left one minus the red disc — see
/// `golemBarZone`). Floats wear no border, so their whole top edge is the bar
/// and every press there used to move — the frame's corners had nowhere to be
/// grabbed (Max, 2026-09-26: *"can we change the resizing to the top corner
/// of the title bars?"*, then *"i want to resize also on the buttons
/// corner"*). A drag from a grip starts the compositor's own resize, which
/// takes the corner from the pointer's quadrant.
inline constexpr int GOLEM_BAR_CORNER_GRAB = 20;

/// What a bar-local logical point is ON: a button (by index), one of the two
/// corner resize grips, the strip, or nothing. THE one map for the hand, the
/// hover lift and the press. Order matters and is the design:
///  · the right grip is the last GOLEM_BAR_CORNER_GRAB px, full height;
///  · the LEFT grip shares the corner with the red button (Max, 2026-09-27:
///    *"i want to resize also on the buttons corner"*): within the first
///    GOLEM_BAR_CORNER_GRAB px, the red DISC itself (its circle, one px
///    generous) is the button and everything around it — the rounded corner,
///    the column to its left, the air above and below it — is the grip. A
///    near miss on close therefore resizes instead of closing, which is the
///    safer miss;
///  · otherwise the button columns (`golemButtonColumn`), then the strip.
struct SGolemBarZone {
    enum eKind : uint8_t { NONE = 0, STRIP, BUTTON, CORNER_LEFT, CORNER_RIGHT } kind = NONE;
    int button = -1; // for BUTTON
};
inline SGolemBarZone golemBarZone(const Vector2D& p, size_t count, double barWidth, double barHeight, float border) {
    if (p.x < 0 || p.y < 0 || p.x >= barWidth || p.y >= barHeight)
        return {};
    if (p.x >= barWidth - GOLEM_BAR_CORNER_GRAB)
        return {SGolemBarZone::CORNER_RIGHT, -1};
    const int col = golemButtonColumn(p, count, barHeight);
    if (col == 0 && p.x < GOLEM_BAR_CORNER_GRAB) {
        const double cx = golemButtonX(0, 1.F) + GOLEM_BUTTON_SIZE / 2.0, cy = golemButtonY(barHeight, border, GOLEM_BUTTON_SIZE) + GOLEM_BUTTON_SIZE / 2.0;
        const double r  = GOLEM_BUTTON_SIZE / 2.0 + 1.0;
        if ((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) > r * r)
            return {SGolemBarZone::CORNER_LEFT, -1};
    }
    if (col >= 0)
        return {SGolemBarZone::BUTTON, col};
    return {SGolemBarZone::STRIP, -1};
}

/// THE HAIRLINE: a faint light border around the whole card — bar and
/// content as one rounded shape — the same one Beam (the browser) wears
/// (Max, 2026-09-27: *"i want a border around the floating windows (the same
/// border that beam (the browser) have"*). Beam's values exactly:
/// ~9% white focused, ~4% unfocused, 1 logical px (2 device px at Golem's
/// 1.6×). Drawn by the bar with the compositor's own border shader around
/// the card box, because a Hyprland border wraps only the content and would
/// cut a line across the seam.
inline constexpr uint64_t GOLEM_CARD_HAIRLINE_FOCUSED   = 0x16FFFFFFULL;
inline constexpr uint64_t GOLEM_CARD_HAIRLINE_UNFOCUSED = 0x0BFFFFFFULL;
inline constexpr int      GOLEM_CARD_HAIRLINE_PX        = 1;

/// How far a hovered button's disc lifts toward white, 0..1. Strong enough to
/// answer the hand at a glance, weak enough that the disc keeps its identity —
/// the red stays red, the peach stays peach.
inline constexpr float GOLEM_BUTTON_HOVER_LIFT = 0.44F; // 0.22 → 0.24 → 0.44, from the mockup
/// How much a hovered disc grows, about its centre (Max, 2026-09-26: *"scale
/// the disk at hover at 1.03"*). The glyph keeps its whole-pixel geometry on
/// the same centre; only the disc breathes.
inline constexpr double GOLEM_BUTTON_HOVER_SCALE = 1.03;

/// The cursor-hint settle timer's lifecycle — called from `Bars::init` and
/// `Bars::shutdown`. Defined in golemBar.cpp beside the state it re-asserts.
void golemBarCursorTimerInit();
void golemBarCursorTimerDrop();

/// The colour sampler's tick — same lifecycle. Every 100ms it re-samples the
/// windows whose surfaces committed since their bar last looked.
void golemBarSampleTimerInit();
void golemBarSampleTimerDrop();

/// The resize hand: while ANY resize drag runs — from the bar's corner grip
/// or from a window edge — the pointer is the closed hand, held by the same
/// three-layer machinery the bar's own shapes use (Max, 2026-09-27: *"i like
/// the close hand on resizing. make it happen everywhere else when i
/// resize"*). Hyprland's own edge resize keeps its directional arrow; this
/// outranks it for the length of the drag and lets go on release.
void golemResizeHandInit();
void golemResizeHandDrop();

/// Free the strip's solid-colour texture (the blurred draw's source) — a GL
/// resource that must not outlive the plugin. Called from `Bars::shutdown`.
void golemBarSolidTexDrop();

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


    /// How far below the strip the notch fill reaches (logical px). Public
    /// because the pass element's occlusion box must cover the fill too.
    double       notchDepth();

    /// The whole CARD — bar plus window content — in global logical px: what
    /// the hairline wraps, so the pass element's box and `damageEntire` cover
    /// its outline on every side, not only the strip.
    CBox         cardBoxGlobal();

    /// Jump the bar's colour to where the window's focus state says it is
    /// going (no fade) — the overview settles a clicked window's look under
    /// its cover so the landing shows it already focused.
    void         settleColor();

    /// The bar's sticky input flags, or "" when none is set (see Bars::debugState).
    std::string  debugLine() const;

    /// Re-sample the window if its surface committed since the last look
    /// (event loop only — it renders; the sampler's timer calls this). Applies
    /// the result through `syncFloatTint` when the colour actually moved.
    void         syncSample();

    WP<CGolemBar> m_self;

  private:
    PHLWINDOWREF         m_pWindow;
    CBox                 m_bAssignedBox;

    /// The bar is only for windows OUT of the layout. Upstream drove this from
    /// a `hyprbars:no_bar` window rule; Golem asks the window itself.
    bool                 m_hidden         = true;

    /// The centred title texture, and the state it was rendered from: the
    /// title, the ink it was set in, and the bar width that capped it. Any of
    /// the three changing re-renders it; nothing else does.
    SP<Render::ITexture> m_pTextTex;
    std::string          m_szLastTitle;
    CHyprColor           m_lastInk  = CHyprColor{0ULL};
    int                  m_lastBarW = -1;

    PHLANIMVAR<CHyprColor> m_cRealBarColor;

    /// True while this window carries our translucent-frame override.
    bool                       m_tinted = false;

    /// The window's sampled top-edge colour (sRGB 0..1), once one has landed.
    /// Until then the bar wears the daemon's border gradient, as before.
    bool                       m_hasSample   = false;
    float                      m_sample[4]   = {0.F, 0.F, 0.F, 1.F}; // sRGB + the content's mean alpha
    /// Set by the surface's commit listener; cleared when the sampler looks.
    bool                       m_sampleDirty = true;
    std::chrono::steady_clock::time_point m_lastSample{};
    CHyprSignalListener        m_commitListener;

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

    /// Firefox floats with a real (thin, config-set 1px) border rather than the
    /// bar-as-frame the other floats get. Mute that border to a faint fixed
    /// hairline so it reads subtle (Max, 2026-09-17: "subtler") — a FIXED
    /// low-alpha colour, not the shell's dynamic border, so it stays quiet
    /// whatever screen-matched colour the borders take. Touches colour only; the
    /// 1px size is the `firefox-subtle-border` window rule's. Set once at map;
    /// unset by the same `m_tinted` teardown as the float tint.
    void     syncFirefoxBorder();

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

    /// Keep the compositor's `square-top` tag in step with the bar: on while
    /// the bar shows, off the moment it hides (see `windowSquareTop`).
    void     syncSquareTop();

    Vector2D cursorRelativeToBar();

    void     renderPass(PHLMONITOR, float const& a);
    void     renderBarTitle(const Vector2D& bufferSize, const float scale);
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
        HOVER_CORNER_LEFT,  // the top-left resize grip (GOLEM_BAR_CORNER_GRAB, minus the red disc)
        HOVER_CORNER_RIGHT, // the top-right resize grip
    };
    eBarHover hoverZone();

    /// Give the pointer its arrow back, if this bar is the one holding it.
    void     releaseCursorIfOwner();

    bool     inputIsValid();
    void     onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e);
    void     onMouseMove(Vector2D coords);

    /// End any press/drag/pointer-hint this bar holds — the overview or the
    /// spread has taken the screen, and their cards own the pointer now.
    void     standDown();

    void     handleDownEvent(Event::SCallbackInfo& info, uint32_t button);
    void     handleUpEvent(Event::SCallbackInfo& info, uint32_t button, bool valid);
    void     handleMovement();
    bool     doButtonPress(Vector2D COORDS);

    CBox     assignedBoxGlobal();

    CHyprSignalListener m_pMouseButtonCallback;
    CHyprSignalListener m_pMouseMoveCallback;

    bool                m_bDraggingThis  = false;
    bool                m_bDragPending   = false;
    bool                m_bResizeFromCorner = false; // the pending/running drag is a RESIZE (pressed on the corner grip)
    bool                m_bCancelledDown = false; // the current press is OURS (the compositor never saw it)
    uint32_t            m_pressedButton  = 0;     // …and which button, so only ITS release is paired with it
    int                 m_ffFloatSignal  = -1;    // the browser float signal as last written by THIS bar

    /// Hover state per button, as a bitfield.
    unsigned int m_iButtonHoverState = 0;

    size_t       getVisibleButtonCount(const Vector2D& bufferSize, const float scale);

    friend class CBarPassElement;
};
