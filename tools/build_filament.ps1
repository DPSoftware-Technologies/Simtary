# Builds Google Filament (libs/filament, submodule pinned to a release tag) once and installs it
# into Simtary/deps/filament-install (include/, lib/x86_64/, bin/ - matc, cmgen, ...), the way
# tools/build_gns.ps1 prepares GameNetworkingSockets. The Hurricanes renderer (EDITIONS.md,
# SIMTARY_RENDERERS containing "Hurricanes") links the installed static libraries.
#
# Its own CMake run with its own flags, on purpose: Filament is a large tree with its own
# compiler settings, and building it inside the workspace would put it under our /EHsc- /GR-
# and re-run it on every configure. What has to match us is set here:
#   USE_STATIC_CRT=OFF            the dynamic CRT (/MD), like every other target in the workspace
#   FILAMENT_ENABLE_EXCEPTIONS=OFF  a panic aborts instead of throwing - nothing here catches
# Backends: OpenGL (the Petrol target) only for now; -Vulkan adds the Vulkan backend.
#
#   powershell -ExecutionPolicy Bypass -File tools/build_filament.ps1 [-Vulkan] [-Clean]
param(
    [switch]$Vulkan,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$src = Join-Path $root "libs\filament"
$deps = Join-Path $root "deps"
$build = Join-Path $deps "_build\filament"
$install = Join-Path $deps "filament-install"

if (-not (Test-Path (Join-Path $src "CMakeLists.txt"))) {
    Write-Error "libs/filament is empty - git submodule update --init --depth 1 libs/filament"
}
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$vk = if ($Vulkan) { "ON" } else { "OFF" }
cmake -S $src -B $build -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_INSTALL_PREFIX=$install" `
    -DUSE_STATIC_CRT=OFF `
    -DFILAMENT_ENABLE_EXCEPTIONS=OFF `
    -DFILAMENT_SUPPORTS_OPENGL=ON `
    "-DFILAMENT_SUPPORTS_VULKAN=$vk" `
    -DFILAMENT_SKIP_SAMPLES=ON `
    -DFILAMENT_SKIP_SDL2=ON `
    -DFILAMENT_BUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { exit 1 }

cmake --build $build --config Release --target install -- /m
if ($LASTEXITCODE -ne 0) { exit 1 }
Write-Host "filament installed into $install"
