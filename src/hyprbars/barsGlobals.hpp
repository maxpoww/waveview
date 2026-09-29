#pragma once

// Golem's titlebar — shared state and the two buttons it wears.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md. Upstream's `globals.hpp` carried a
// seventeen-value config struct and a user-defined button list; both are gone.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <string>
#include <vector>

/// What a button on the bar does when pressed.
enum eGolemBarButton : uint8_t {
    /// Close the window.
    GOLEM_BAR_CLOSE = 0,
    /// Put it back in the layout — Golem's answer to "unmaximize", since a
    /// floating window's way home is the tiling it left.
    GOLEM_BAR_TILE,
    /// Minimize to the DOCK, macOS-style (Max, 2026-09-16: *"the yellow
    /// button to minimize the window to dock. same as macos. (not to the
    /// icon) to the dock"*): the window's decorated card flies into the
    /// dock, the window itself parks on `special:minimized`, and waverunner
    /// shows a per-window thumbnail entry that restores it on click.
    GOLEM_BAR_MIN,
};

/// How a button's glyph is made. A font glyph rasterised at 13px lands its
/// strokes on different sub-pixel phases on each side, and no amount of
/// measuring centres what the eye reads as lopsided (Max, 2026-09-26: *"they
/// dont look well.. not centered… the worse is the square"*). The square and
/// the dash are therefore DRAWN — rectangles centred on the disc by
/// construction, with sizes of the disc's parity so the margins are whole and
/// equal. Only the × stays a font glyph (diagonals are not rectangles).
enum eGolemGlyph : uint8_t {
    GOLEM_GLYPH_TEXT = 0,
    GOLEM_GLYPH_SQUARE,
    GOLEM_GLYPH_DASH,
};

/// One button: what it does, what it looks like, and the glyph texture once it
/// has been rendered.
struct SGolemButton {
    eGolemBarButton      action;
    CHyprColor           bgcol;
    CHyprColor           fgcol;
    std::string          icon;
    eGolemGlyph          glyph = GOLEM_GLYPH_TEXT;
    SP<Render::ITexture> iconTex;
    /// Where the glyph's INK sits in `iconTex`: its centre relative to the
    /// texture's centre, in texture px. A text texture carries the font's
    /// ascent, descent and side bearings around the glyph, so centring the
    /// texture on the disc left every glyph a little off (Max, 2026-09-26:
    /// *"the icons are not centered on the circles"*). Measured once from
    /// the texture's alpha after it is rendered.
    Vector2D             inkOffset;
    bool                 inkMeasured = false;
};

class CGolemBar;

/// The plugin handle, needed to attach decorations. waveview's own `PHANDLE`
/// lives in its main.cpp and is not visible here, so the bars keep their own
/// copy — set once in `Bars::init`.
inline HANDLE g_barsHandle = nullptr;

struct SGolemBarsState {
    /// Right to left, the way a titlebar is read.
    std::vector<SGolemButton> buttons;
    std::vector<WP<CGolemBar>> bars;
};

/// True from the first line of `Bars::shutdown`. Guards every path that queues
/// a deferred callback: a lambda handed to the event loop lives in THIS .so,
/// and one queued during teardown fires after the library unmaps — that is a
/// jump into freed memory, and it took the session down (2026-09-15, unload
/// with the pointer parked on a button: the destructors released the cursor,
/// the release queued a doLater, the doLater outlived the plugin).
inline bool g_barsShuttingDown = false;

inline UP<SGolemBarsState> g_pBarsState;
