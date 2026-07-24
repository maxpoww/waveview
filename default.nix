{ pkgs ? import <nixpkgs> {} }:
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
