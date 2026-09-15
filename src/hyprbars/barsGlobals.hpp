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
};

/// One button: what it does, what it looks like, and the glyph texture once it
/// has been rendered.
struct SGolemButton {
    eGolemBarButton      action;
    CHyprColor           bgcol;
    CHyprColor           fgcol;
    std::string          icon;
    SP<Render::ITexture> iconTex;
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

inline UP<SGolemBarsState> g_pBarsState;
