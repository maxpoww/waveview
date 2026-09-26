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
#include <hyprland/src/managers/CursorManager.hpp>

#include "barsGlobals.hpp"
#include "golemBar.hpp"

#include <algorithm>

namespace Bars {
    std::string debugState() {
        std::string out;
        size_t      bars = 0;
        for (auto& w : g_pCompositor->m_windows) {
            if (!w)
                continue;
            for (auto& d : w->m_windowDecorations)
                if (auto* bar = dynamic_cast<CGolemBar*>(d.get())) {
                    ++bars;
                    if (const auto line = bar->debugLine(); !line.empty())
                        out += line + " ";
                }
        }
        return std::to_string(bars) + " bars" + (out.empty() ? ", none sticky" : ": " + out);
    }

    void settleFocus(PHLWINDOW window) {
        if (!window)
            return;
        for (auto& d : window->m_windowDecorations)
            if (auto* bar = dynamic_cast<CGolemBar*>(d.get()))
                bar->settleColor();
    }


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

        golemBarCursorTimerInit();

        // GOLEM'S TRAFFIC LIGHTS (Max, 2026-09-15: *"three colors, macOS style
        // but different colors, that make sense"*): the familiar three-disc
        // row, recoloured to mean what the buttons actually DO here.
        //
        // Left to right: RED, YELLOW, GREEN — macOS's order, kept exactly
        // (Max, 2026-09-26: *"put the buttons on the right order, R Y G"*;
        // green had sat in the middle since the minimize button arrived).
        // Close is outermost, where every desktop has taught the hand to
        // expect it. The glyphs are dark on all three — every disc is a light
        // colour, and one ink keeps the row reading as a set.
        //
        // VIVID, not pastel (Max, 2026-09-15: *"super saturated colors that
        // outstand"*) — the first cut used soft Golem-family tones and read as
        // washed out against the muted bar. These are near-full-saturation and
        // deliberately louder than everything around them: the frame whispers,
        // the three controls shout.
        //
        // RED — destroy. The one meaning no desktop gets to reinvent.
        g_pBarsState->buttons.push_back(SGolemButton{
            .action = GOLEM_BAR_CLOSE,
            .bgcol  = CHyprColor{0xFFFF2E2EULL},
            .fgcol  = CHyprColor{0xFF1A1A1AULL},
            .icon   = "×",
        });
        // ORANGE — minimize to the dock, macOS-style: the card flies into
        // the dock and lives there as its own thumbnail until clicked back.
        // The glyph arrives in the same breath as the action, as promised.
        g_pBarsState->buttons.push_back(SGolemButton{
            .action = GOLEM_BAR_MIN,
            .bgcol  = CHyprColor{0xFFFF9500ULL},
            .fgcol  = CHyprColor{0xFF1A1A1AULL},
            .icon   = "–",
        });
        // GREEN — back into the layout. Where macOS's green means "grow to
        // fullscreen", Golem's means "grow back into the tiling": the same
        // gesture of order, Golem's own answer.
        g_pBarsState->buttons.push_back(SGolemButton{
            .action = GOLEM_BAR_TILE,
            .bgcol  = CHyprColor{0xFF21D758ULL},
            .fgcol  = CHyprColor{0xFF1A1A1AULL},
            .icon   = "▤",
        });

        for (auto& w : g_pCompositor->m_windows) {
            if (!w || w->isHidden() || !w->m_isMapped)
                continue;
            onWindowOpen(w);
        }
    }

    void shutdown() {
        // FIRST, before anything else: no deferred callback may be queued from
        // here on. The decorations are destroyed after PLUGIN_EXIT returns,
        // and a callback their destructors queue outlives the library — the
        // 2026-09-15 unload crash.
        g_barsShuttingDown = true;

        golemBarCursorTimerDrop();

        // Give the pointer its arrow back. A bar that owned the "grab" shape
        // when the plugin went away would leave the whole desktop wearing it,
        // with nothing left to take it off.
        if (g_pCursorManager)
            g_pCursorManager->setCursorFromName("left_ptr");

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
