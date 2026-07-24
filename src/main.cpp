#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <string>

extern "C" {
#include <lua.h>
}

// ---- Rust brain (FFI) --------------------------------------------------------
struct Rect {
    double x, y, w, h;
};
extern "C" {
const char* waveview_hello();
int         waveview_workspace_tiles(double mw, double mh, Rect* out);
int         waveview_tile_for_workspace(int64_t ws_id);
void        waveview_map_window(double tx, double ty, double tw, double th, double mon_x, double mon_y, double mon_w,
                                double mon_h, double wx, double wy, double ww, double wh, Rect* out);
}

inline HANDLE              PHANDLE = nullptr;
static bool                g_active = false;
static CHyprSignalListener g_renderListener;

static void damageAll() {
    for (auto& m : g_pCompositor->m_monitors) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

static void renderRect(const CBox& box, const CHyprColor& color) {
    CRectPassElement::SRectData data;
    data.box   = box;
    data.color = color;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

// Draw the 9 workspace tiles (layout from Rust) onto the current monitor, then
// each live window schematically mapped into its workspace tile, then a border
// on the active workspace. Tiles are pixel-space; window boxes are logical —
// the brain reconciles the two. Real thumbnails (per-workspace framebuffers)
// replace the window rects next.
static void drawOverview(PHLMONITOR m) {
    if (!m)
        return;

    Rect tiles[9];
    const int n = waveview_workspace_tiles(m->m_transformedSize.x, m->m_transformedSize.y, tiles);
    if (n < 9)
        return;

    // Base tiles.
    for (int i = 0; i < n; ++i)
        renderRect(CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}, CHyprColor(0.0, 0.0, 0.0, 0.35));

    // Live windows on this monitor, mapped into their workspace's tile.
    for (auto& w : g_pCompositor->m_windows) {
        if (!w || !w->m_isMapped || w->isHidden() || w->monitorID() != m->m_id)
            continue;
        const int ti = waveview_tile_for_workspace(w->workspaceID());
        if (ti < 0)
            continue;

        const CBox wb = w->getWindowMainSurfaceBox();
        Rect       mini;
        waveview_map_window(tiles[ti].x, tiles[ti].y, tiles[ti].w, tiles[ti].h, m->m_position.x, m->m_position.y,
                            m->m_size.x, m->m_size.y, wb.x, wb.y, wb.w, wb.h, &mini);
        if (mini.w <= 0.0 || mini.h <= 0.0)
            continue;

        const bool active = g_pCompositor->isWindowActive(w);
        renderRect(CBox{mini.x, mini.y, mini.w, mini.h},
                   active ? CHyprColor(0.40, 0.70, 1.0, 0.90) : CHyprColor(0.85, 0.85, 0.90, 0.80));
    }

    // Active-workspace border (drawn on top).
    const int ai = waveview_tile_for_workspace(m->activeWorkspaceID());
    if (ai >= 0) {
        const Rect&      t  = tiles[ai];
        const double     bw = 2.0;
        const CHyprColor c(0.40, 0.70, 1.0, 1.0);
        renderRect(CBox{t.x, t.y, t.w, bw}, c);            // top
        renderRect(CBox{t.x, t.y + t.h - bw, t.w, bw}, c); // bottom
        renderRect(CBox{t.x, t.y, bw, t.h}, c);            // left
        renderRect(CBox{t.x + t.w - bw, t.y, bw, t.h}, c); // right
    }
}

static void onRender(eRenderStage stage) {
    if (!g_active)
        return;
    if (stage == eRenderStage::RENDER_POST_WINDOWS)
        drawOverview(g_pHyprRenderer->m_renderData.pMonitor.lock());
}

static int luaToggle(lua_State*) {
    g_active = !g_active;
    damageAll();
    return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    HyprlandAPI::addLuaFunction(handle, "waveview", "toggle", luaToggle);
    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage s) { onRender(s); });
    HyprlandAPI::addNotification(handle, std::string("[waveview] loaded -- ") + waveview_hello(),
                                 CHyprColor(0.3, 1.0, 0.5, 1.0), 3000);
    return {"waveview", "Live 3x3 workspace overview (Rust brain + C++ shim)", "max", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_renderListener.reset();
}
