# Builds GameNetworkingSockets (Valve) into Simtary/deps/gns-install, once per machine.
# The engine's CMake imports it from there (SimtaryGNS) when it exists; see MULTIPLAYER.md.
#
#   protobuf v21.12  - the last release without abseil; static, linked INTO the GNS DLL
#   GNS v1.6.1       - shared library, USE_CRYPTO=BCrypt (Windows' own; no OpenSSL),
#                      ENABLE_ICE=OFF (dedicated servers, no peer-to-peer)
#
# GNS uses exceptions; it stays behind the DLL boundary, the engine only calls its
# plain virtual interfaces. Run from PowerShell (MSBuild switches break under Git Bash).

$ErrorActionPreference = "Stop"
$deps = Join-Path $PSScriptRoot "..\deps" | Resolve-Path
$install = Join-Path $deps "gns-install"
$gen = "Visual Studio 18 2026"

if (-not (Test-Path "$deps\protobuf")) {
    git clone --depth 1 --branch v21.12 https://github.com/protocolbuffers/protobuf.git "$deps\protobuf"
}
if (-not (Test-Path "$deps\gns")) {
    git clone --depth 1 --branch v1.6.1 https://github.com/ValveSoftware/GameNetworkingSockets.git "$deps\gns"
}

cmake -S "$deps\protobuf" -B "$deps\_build\protobuf" -G $gen -A x64 `
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_MSVC_STATIC_RUNTIME=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF `
    -Dprotobuf_WITH_ZLIB=OFF -DCMAKE_INSTALL_PREFIX="$install"
cmake --build "$deps\_build\protobuf" --config Release --target install -- /m:16

cmake -S "$deps\gns" -B "$deps\_build\gns" -G $gen -A x64 `
    -DBUILD_SHARED_LIB=ON -DBUILD_STATIC_LIB=OFF -DENABLE_ICE=OFF -DUSE_CRYPTO=BCrypt `
    -DProtobuf_USE_STATIC_LIBS=ON -DCMAKE_PREFIX_PATH="$install" -DCMAKE_INSTALL_PREFIX="$install"
cmake --build "$deps\_build\gns" --config Release --target install -- /m:16

Write-Host "GNS installed to $install - re-run cmake --preset <yours> so the engine picks it up."
