#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <string>

// ---- Rust brain (FFI) --------------------------------------------------------
extern "C" {
int         waveview_grid_cols();
int         waveview_grid_rows();
const char* waveview_hello();
}

inline HANDLE PHANDLE = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static SDispatchResult onToggle(std::string) {
    HyprlandAPI::addNotification(
        PHANDLE,
        "[waveview] toggle -> Rust grid " + std::to_string(waveview_grid_cols()) + "x" +
            std::to_string(waveview_grid_rows()),
        CHyprColor(0.2, 0.8, 1.0, 1.0), 4000);
    return {};
}

// Built against the exact running Hyprland via nix (mkHyprlandPlugin), so the
// ABI is guaranteed to match -- no runtime version guard needed.
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    HyprlandAPI::addDispatcherV2(handle, "waveview:toggle", ::onToggle);
    HyprlandAPI::addNotification(handle, std::string("[waveview] loaded -- brain says: ") + waveview_hello(),
                                 CHyprColor(0.3, 1.0, 0.5, 1.0), 5000);
    return {"waveview", "Live 3x3 workspace overview (Rust brain + C++ shim)", "max", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
