#pragma once

// Golem's titlebars — the small surface waveview drives them through.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md. This replaces upstream's `main.cpp`, which
// was mostly config registration and button parsing.

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>

/// One line to the waverunner daemon's socket (defined in `../main.cpp`).
/// A button on the bar is a GOLEM action — "back into the layout" has to go
/// through the daemon's own mode machinery, not the compositor's float toggle,
/// or nothing downstream (the solitary-pseudo rule, the bar's state pill) hears
/// about it.
void sendWaverunner(std::string msg);

namespace Bars {
    /// Create the state, define the buttons, and give every mapped window a bar
    /// (dormant until the window floats). Call once, from `PLUGIN_INIT`.
    void init(HANDLE handle);

    /// Give a newly mapped window its bar.
    void onWindowOpen(PHLWINDOW window);

    /// Drop what the bars are holding. Call from `PLUGIN_EXIT`.
    void shutdown();

    /// Snap `window`'s bar colour to its focus state, skipping the fade.
    void settleFocus(PHLWINDOW window);

    /// The card is on (or off) for the window at `addr` ("0x…"; "*" = every
    /// window): its bar's card button shows it. Called for waverunner.
    void setCard(const std::string& addr, bool on);

    /// One entry per bar holding a sticky input flag (drag pending/active, a
    /// cancelled press), for waveview's `debug_state` dump.
    std::string debugState();
}
