// Golem's titlebars — setup and teardown.
//
// A stripped fork of hyprbars (BSD-3-Clause, Copyright (c) 2023 Hypr
// Development) — see PROVENANCE.md. Upstream's `main.cpp` was 279 lines, most
// of it registering seventeen config values and parsing user-defined buttons.
// Golem has one look and two buttons, so what is left is the part that matters:
// give each window a bar, and let the bar decide whether it is wanted.

#include "bars.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "barsGlobals.hpp"
#include "golemBar.hpp"

#include <algorithm>

namespace Bars {

    void onWindowOpen(PHLWINDOW window) {
        if (!window || window->m_X11DoesntWantBorders)
            return;

        // A window gets AT MOST one.
        if (std::ranges::any_of(window->m_windowDecorations, [](const auto& d) { return d->getDisplayName() == "Golembar"; }))
            return;

        // Every window gets one, tiled or not, and it hides itself while the
        // window is in the layout (`CGolemBar::syncHidden`). Attaching only to
        // floats would mean catching the moment a window leaves the layout —
        // and this compositor emits no event for that (measured 2026-09-12),
        // which is exactly the trap the state pill had to route around.
        auto bar = makeUnique<CGolemBar>(window);
        g_pBarsState->bars.emplace_back(bar);
        bar->m_self = bar;
        HyprlandAPI::addWindowDecoration(g_barsHandle, window, std::move(bar));
    }

    void init(HANDLE handle) {
        g_barsHandle = handle;
        g_pBarsState = makeUnique<SGolemBarsState>();

        // Right to left, the way a titlebar is read: close is outermost, where
        // every desktop has taught the hand to expect it.
        g_pBarsState->buttons.push_back(SGolemButton{
            .action = GOLEM_BAR_CLOSE,
            .bgcol  = CHyprColor{0xFFEB6F6FULL},
            .fgcol  = CHyprColor{0xFF1A1A1AULL},
            .icon   = "×",
        });
        // Back into the layout — a floating window's way home. Golem has no
        // minimise and no maximise: the tiling is the thing a float left.
        g_pBarsState->buttons.push_back(SGolemButton{
            .action = GOLEM_BAR_TILE,
            .bgcol  = CHyprColor{0xFF5A5F6AULL},
            .fgcol  = CHyprColor{0xFFE8E6E3ULL},
            .icon   = "▤",
        });

        for (auto& w : g_pCompositor->m_windows) {
            if (!w || w->isHidden() || !w->m_isMapped)
                continue;
            onWindowOpen(w);
        }
    }

    void shutdown() {
        for (auto& m : g_pCompositor->m_monitors) {
            m->m_scheduledRecalc = true;
        }
        // Flush queued bar draws while this .so is still mapped — their vtables
        // live in it.
        g_pHyprRenderer->m_renderPass.removeAllOfType("CBarPassElement");
        // The state is NOT reset here, and that is deliberate: Hyprland removes
        // the plugin's window decorations *after* PLUGIN_EXIT returns, so every
        // `~CGolemBar` still runs — and one that finds the state already gone
        // crashed the session (2026-09-13). It goes away on its own when the
        // library unmaps, by which point the decorations are long destroyed.
    }

}
