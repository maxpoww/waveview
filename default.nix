{ pkgs ? import <nixpkgs> {
    overlays = [
      # Build against the SAME Hyprland the system actually runs. /etc/nixos
      # overlays patch it, and a patch that touches a header changes struct
      # layout: vfr-hold (2026-09-10) added a CMonitor member, every member
      # after it shifted 8 bytes, and this plugin — compiled against stock
      # headers — read m_layerSurfaceLayers as empty (the overview lost its
      # wallpaper backdrop) and WROTE m_solitaryClient.reset() at the wrong
      # offset. Reading the patch directory keeps the two package sets in
      # lockstep automatically: a new system patch lands here on the next
      # rebuild, and a patch that no longer applies fails this build loudly.
      (final: prev: {
        hyprland = prev.hyprland.overrideAttrs (old: {
          patches = (old.patches or [ ])
            ++ map (n: /etc/nixos/patches + "/${n}")
              (builtins.filter (n: builtins.match ".*\\.patch" n != null)
                (builtins.attrNames (builtins.readDir /etc/nixos/patches)));
        });
      })
    ];
  }
}:
pkgs.hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "waveview";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = with pkgs; [ pkg-config cargo rustc ];
  preBuild = ''
    export CARGO_HOME=$TMPDIR/cargo
    export HOME=$TMPDIR
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib
    cp waveview.so $out/lib/libwaveview.so
    runHook postInstall
  '';
  meta.description = "Live 3x3 workspace overview (Rust brain + C++ shim)";
}
