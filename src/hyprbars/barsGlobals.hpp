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
    /// Reserved: a third control, drawn and clickable but doing nothing yet
    /// (Max, 2026-09-15: *"add another button (we are gonna wire it later)"*).
    /// It is a real button so the row's spacing, hover and hit-testing are
    /// settled now; give it an action in `doButtonPress` when we know what it
    /// should do.
    GOLEM_BAR_UNWIRED,
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

/// True from the first line of `Bars::shutdown`. Guards every path that queues
/// a deferred callback: a lambda handed to the event loop lives in THIS .so,
/// and one queued during teardown fires after the library unmaps — that is a
/// jump into freed memory, and it took the session down (2026-09-15, unload
/// with the pointer parked on a button: the destructors released the cursor,
/// the release queued a doLater, the doLater outlived the plugin).
inline bool g_barsShuttingDown = false;

inline UP<SGolemBarsState> g_pBarsState;
