# Builds bhs.dll once and installs it into Simtary/deps/bhs-install (bin/, lib/, include/, web/),
# the same way tools/build_gns.ps1 prepares GameNetworkingSockets. The engine links the prebuilt DLL.
# Needs deps/libdatachannel (git clone --recurse-submodules) and deps/openssl-msys (tools/setup_openssl_msys.ps1).
$ErrorActionPreference = "Stop"
$src = (Resolve-Path "$PSScriptRoot\..").Path
$deps = (Resolve-Path "$src\..\deps").Path
$build = Join-Path $deps "_build\bhs"
cmake -S $src -B $build -G "Visual Studio 18 2026" -A x64 "-DCMAKE_INSTALL_PREFIX=$deps\bhs-install"
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --build $build --config Release --target bhs bhs_stream_test -j 16
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --install $build --config Release --component Unspecified
if ($LASTEXITCODE -ne 0) { exit 1 }
Write-Host "bhs installed into $deps\bhs-install"
