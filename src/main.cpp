#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
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

// Draw the 9 workspace tiles (layout from Rust) onto the current monitor, in
// pixel coords. Placeholder fill for now; live windows go here next.
static void drawOverview(PHLMONITOR m) {
    if (!m)
        return;
    const double mw = m->m_transformedSize.x;
    const double mh = m->m_transformedSize.y;

    Rect tiles[9];
    const int n = waveview_workspace_tiles(mw, mh, tiles);
    for (int i = 0; i < n; ++i)
        renderRect(CBox{tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h}, CHyprColor(0.0, 0.0, 0.0, 0.35));
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
