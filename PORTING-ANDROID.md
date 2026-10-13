# Porting Simtary to Android

Status: **step 1 done (2026-10-13) - `libmain.so` builds and links for arm64-v8a.** Nothing
runs on a device yet (no Gradle project). What exists is a set of seams so the port is a
series of contained changes rather than a rewrite. This file records what was prepared, what is
still missing, and the order that keeps each step verifiable.

The engine is a Wicked Engine fork. Upstream Wicked has no Android support, so nothing
here can be pulled from upstream — every item below is work this fork has to do.

## What is already in place

| Seam | Where | What it does |
|---|---|---|
| Platform tag | `Engine/wiPlatform.h` | `__ANDROID__` defines `PLATFORM_ANDROID` **and** `PLATFORM_LINUX`, so POSIX paths keep compiling while Android-specific code has a tag to hang off |
| Build tag | `cmake/SimtaryPlatform.cmake` | `ANDROID` is tested before `UNIX`, giving `android_arm64` instead of colliding with `linux_arm64`; `CMAKE_ANDROID_ARCH_ABI` maps `arm64-v8a` and `armeabi-v7a` |
| Bring-up preset | `Milistry/CMakePresets.json` | `android_arm64` configures the NDK toolchain against the existing sources. Compile-only — no APK |
| GPU features | `Engine/wiGraphicsDevice_Vulkan.cpp` | `geometryShader`, `textureCompressionBC` and `shaderClipDistance` were hard asserts; they are now capabilities (`GEOMETRY_SHADER`, `TEXTURE_COMPRESSION_BC`, `TEXTURE_COMPRESSION_ASTC`, `TEXTURE_COMPRESSION_ETC2`) so a mobile GPU reports them missing instead of tripping an assert |
| Voxel GI without GS | `Engine/shaders/ShaderInterop_VXGI.h` | `VOXELIZATION_GEOMETRY_SHADER_ENABLED` is off for `__ANDROID__`, and can be forced off for shader builds with `SIMTARY_NO_GEOMETRY_SHADER`. The vertex-shader-replication path it falls back to already existed for Metal |
| Asset source | `Engine/wiHelper.h` — `AssetSourceOverride` | `FileRead`/`FileExists` can be redirected at a package instead of the filesystem, declining per path so save data and shader cache still come from real files |
| Position precision | `Engine/stWorldScalar.h`, `-DSIMTARY_LARGE_WORLD=OFF` | Drops absolute world positions from double to float, which matters on GPUs and CPUs where fp64 is emulated |

## What is still missing

### 1. Window, lifecycle and entry point

`st::Run` assumes a desktop SDL window that exists for the whole process. Android does
not work that way: the activity owns the surface and destroys it whenever the app is
backgrounded, and `SDL_main` is entered from `ANativeActivity` through the Java glue.

- Entry point through SDL's Android activity, with the Java side built by Gradle.
- Handle `SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERFOREGROUND`.
- **Swapchain surface loss.** `wiGraphicsDevice_Vulkan` has no recreate-on-surface-lost
  path; on Android it needs one, or the first app switch kills the device.

### 2. Assets

`Milistry/assets` is 77 MB and is loaded through relative `"assets/..."` paths. Inside an
APK those are not files.

- Implement an `AssetSourceOverride` backed by `AAssetManager` and install it before
  `wi::initializer` runs.
- Anything the engine *writes* — settings, save data, the shader cache — must move to the
  app's internal storage directory, so `GetCacheDirectoryPath` and `GetCurrentPath` need
  Android implementations.
- 77 MB argues for an AAB with asset packs rather than a monolithic APK.

### 3. Textures

Every compressed texture in the project is BC. No mainstream Android GPU exposes BC; they
ship ASTC and ETC2. The capability bits are reported now, but nothing acts on them.

- Re-encode the texture set to ASTC for the Android build.
- Teach the asset pipeline and `wiResourceManager` to pick the encoding per target.

This is the largest asset-side task and it is worth measuring before committing to it.

### 4. Render path

`RenderPath3D` is a desktop-class path: deferred-style passes, heavy post, and a shader
set with mesh-shader and raytracing permutations. On a tile-based mobile GPU this will
not hold frame rate.

- A cut-down render path with the expensive passes off by default.
- A mobile shader permutation set. The HLSL → SPIR-V pipeline through DXC already works
  cross-platform, so this is about *which* permutations to build, not how.
- Shader compilation must be fully ahead-of-time: `dxcompiler.dll` is desktop-only.
- Check the bindless requirements (`descriptorIndexing`, `runtimeDescriptorArray`,
  `descriptorBindingPartiallyBound`, `shaderSampledImageArrayNonUniformIndexing`) against
  the device floor you intend to support. Adreno 6xx and Mali Valhall are fine; older
  parts are not. Vulkan 1.3 with `dynamicRendering` is also required today.

### 5. Input

`wiRawInput.cpp` and `wiXInput.cpp` are Windows-only and drop out of the build cleanly, so
nothing breaks — but the game is designed around mouse and keyboard. Touch input arrives
through SDL already; what does not exist is a touch control scheme or a UI laid out for it.
This is game-side work in `Milistry/src`, not engine work.

### 6. Crash reporting

`Simtary/crashreporter` is a separate SDL + ImGui executable launched after a crash.
Android has no equivalent model. `sentry-native` does support Android, through a different
initialisation path — use it directly and drop the external reporter for this target.

### 7. Dependency sweep

Known-good on Android: SDL2, ImGui, ImGuizmo, Jolt, Lua, openal-soft, libzmq, Faust.
Needs attention: `sentry-native` (different init), `dxcompiler` (desktop-only, must become
build-time only), the ZMQ bridge (developer tooling — decide whether it ships at all).

## Step 1 - what it took (2026-10-13)

Toolchain: NDK r28c (28.2.13676358) in `E:/HMMWV4/toolchains/android-sdk` (installed with
`sdkmanager --sdk_root`, plus platform-tools, platforms/android-36, build-tools/36.1.0).
`Milistry/CMakePresets.json` `android_arm64` points at it and at the HOST tools.

```
cmake --preset android_arm64
cmake --build --preset android_arm64        # -> build/android_arm64/Release/libmain.so
```

| Fix | Where |
|---|---|
| sentry `inproc` backend (Crashpad has no Android port); crash reporter GUI + crashpad_handler not staged | `CMakeLists.txt`, `cmake/SimtaryApp.cmake` |
| `stpack` / `stshaderc` are IMPORTED from a host build when cross-compiling (`SIMTARY_HOST_TOOLS_DIR`, default `<project>/build/win_x86-64/_simtary/Release`) - build the desktop preset first | `CMakeLists.txt` |
| Steam Audio `lib/android-armv8` | `CMakeLists.txt` |
| In-tree SDL2 target used instead of `find_package(SDL2)`; version check fixed | `Engine/CMakeLists.txt` |
| x86 `USE_SSE*/AVX*` options forced OFF on ARM | `Engine/CMakeLists.txt` |
| App = `add_library(SHARED)` named `main`, no SOVERSION, MODULE layout ignored, links `log android`; no offline shader warm (device binary) | `cmake/SimtaryApp.cmake` |
| `ST_APP_ENTRY` emits an exported `extern "C" SDL_main` | `Framework/stModule.h` |
| `WICKED_PIC` forced ON (static archives go into a .so) | `CMakeLists.txt` |
| `__aarch64__` spin-lock yield | `Engine/wiSpinLock.h` |
| DirectXMath: Android detected before `__linux__`; **scalar path** (`BUILD_INTRINSICS_LEVEL 0`) because this copy's NEON path is MSVC-only; XMVECTOR operator overloads kept for the scalar struct | `Engine/Utility/DirectXMath/DirectXMathCommon.h`, `DirectXMath.h` |
| zstd COVER: C90 qsort on Android (bionic has no `qsort_r` below API 36) | `Engine/Utility/zstd/zstd.c` |
| `sched_setaffinity(pthread_gettid_np())` instead of `pthread_setaffinity_np` | `Engine/wiJobSystem.cpp` |
| no CPUID off x86 | `Engine/Utility/cpuinfo.hpp` |
| `SubWinStatus` Android branch (no native window; thread idles) | `Framework/SubWinStatus.cpp` |
| clang-only C++ fixes: nested-struct default argument (`StreamPlayer::Config` -> `StreamConfig`), address of temporary `XMFLOAT3` | `Engine/stAudioStream.h`, `Framework/script/ScriptBindings.cpp` |

Follow-ups found on the way: update DirectXMath to an upstream release with clang ARM64
NEON (scalar math is slower); `libmain.so` is 265 MB with debug info (strip in the APK).

Target device facts (gpuinfo report 44864, Galaxy Tab S9 FE SM-X510, Mali-G68, driver
38.1.0, Vulkan 1.3.219, Android 16): full bindless / update-after-bind / dynamic rendering /
sync2 = yes; **textureCompressionBC = no** (ASTC/ETC2 yes), **sparseBinding = no** (terrain
needs `NOSPARSE`, whose tile copies are BC - needs an uncompressed or ASTC path),
**shaderInt64 = no** (`visibility_resolveCS.hlsl`, `globals.hlsli firstbitlow64` use uint64),
no ray query / mesh shader / VRS, no `shaderClipDistance`, no `fillModeNonSolid`,
`multiDrawIndirect` = no. The A51 (Mali-G72, Vulkan 1.1, no descriptor indexing) cannot run
this renderer at all - CPU benchmark only.

## Step 2 - on the tablet (2026-10-14)

`Milistry/android/` is the Gradle shell (AGP 8.6, Gradle 8.7 wrapper, SDK from
`local.properties`): it packages CMake's `libmain.so` + SDL2/openal/phonon with SDL's own
Java sources (used straight from the submodule) and `MilistryActivity`. The game data is
NOT in the APK yet - it is pushed once into the app's external files folder, which
`st::Run` chdirs into, so every relative path works:

```
cd Milistry/android && ./gradlew.bat assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
# Git Bash rewrites /sdcard/... - set MSYS_NO_PATHCONV=1, and give adb E:/ paths
adb push build/android_arm64/Release/assets       /sdcard/Android/data/com.platoonlabs.milistry/files/
adb push build/android_arm64/Release/shaders/spirv /sdcard/Android/data/com.platoonlabs.milistry/files/shaders/
adb shell am start -n com.platoonlabs.milistry/.MilistryActivity
adb logcat -s Simtary          # stdout/stderr are piped to logcat; log.txt is in the files folder
```

Fixed on the way: stdout/stderr -> logcat and chdir to the data folder (`stRun.cpp`); the
shader path re-derived after the chdir (it is built at static init, when cwd was "/");
no crash-reporter process on Android; **only 7 descriptor sets bound** when ray tracing is
off (`descriptor_set_count` - Mali-G68 `maxBoundDescriptorSets` = 7, and 8 smashed the
driver's stack inside vkCreatePipelineLayout); **BC textures decoded on the CPU**
when the device has no BC (`Engine/stTextureFallback.*` + vendored `Utility/bcdec.h`,
called from Vulkan `CreateTexture`); swapchain `preTransform` = IDENTITY (the panel reports
ROTATE_90 with a landscape currentExtent; the compositor rotates).

The Android 16 "16 KB page size" dialog lists every .so for a debuggable APK; ours are
0x4000-aligned (ELF and zip, `zipalign -c -P 16`), only Valve's prebuilt `libphonon.so` is
0x1000 - needs a newer Steam Audio or Steam Audio off on Android before a 16 KB device.

## Step 3 - first 3D frames on Mali-G68 (2026-10-14)

Each item below was a separate wall; the order is the order they were hit.

| Problem on Mali-G68 / driver r38.1 | Fix |
|---|---|
| 8 bound descriptor sets, limit 7 - stack smash inside vkCreatePipelineLayout | `descriptor_set_count`: the acceleration-structure set is not bound without ray tracing |
| No BC textures (built-ins and content) | CPU decode on create (`Engine/stTextureFallback.*` + `Utility/bcdec.h`), and sRGB/typeless BC views remapped in `CreateSubresource` |
| Bindless arrays sized to the exact limit (500000) + set 0's own bindings = over the limit | 1024 headroom per bindless array |
| `compositeAlpha` OPAQUE unsupported; panel reports ROTATE_90 | first supported alpha mode; preTransform IDENTITY |
| No `shaderInt64` (134 shaders) | `ShaderMeshInstance::uid`, `SurfelData::uid`, `VisibilityTile` mask = two uints in HLSL, uint64 in C++ (same layout) |
| No `shaderClipDistance` | **`spirv_mobile` shader set** (`offlineshadercompiler spirv_mobile`, define `SIMTARY_MOBILE`, loaded on Android): `ST_CLIP_DISTANCE` becomes a plain varying |
| maxComputeWorkGroupInvocations 512 | mobile: filterEnvMapCS THREAD_OFFLOAD 8, LUMINANCE_BLOCKSIZE 16 (C++ via `__ANDROID__`) |
| No `vertexPipelineStoresAndAtomics`; tessellation pipelines crashed the Mali compiler | HS/DS pipelines refused on such a device |
| No `fillModeNonSolid` | wireframe -> solid |
| **Opaque `objectPS` miscompiled -> VK_ERROR_DEVICE_LOST on the first frame** | an always-true early `return color` before the (dead for opaque) alpha-test block, `SIMTARY_MOBILE` only - found by bisecting the shader on the device |
| GUI background blur chain faults intermittently | off on Android (`wiRenderPath3D.cpp`), not root-caused |
| ~3-4 min of pipeline compiles every launch | Vulkan pipeline cache on Android, saved right after renderer init (`SavePipelineCache`) -> ~10 s from the 2nd launch |

Bring-up tools left in on purpose (all off unless asked for):

- `gpu_breadcrumbs` - every command buffer submitted and waited on alone; the first one that
  loses the device is logged with its EventBegin labels ("GPU BREADCRUMB: ..."). This is what
  found the faulting pass when the driver reports nothing else.
- `mobile_minimal` - every optional render pass and weather effect off.
- `debug_nosky`, `debug_nomeshes`, `debug_skynodraw`, `debug_skyclip` - single-draw switches.
- `pso begin/end` and `shader begin/end` timing lines in logcat (Android only).
- Launch arguments: `adb shell "am start -n com.platoonlabs.milistry/.MilistryActivity --es args '--scene=dronesim mobile_minimal'"`.
- Validation layer: `libVkLayer_khronos_validation.so` is in the debug APK (`app/src/debug/jniLibs`);
  enable with `settings put global enable_gpu_debug_layers 1`, `gpu_debug_app com.platoonlabs.milistry`,
  `gpu_debug_layers VK_LAYER_KHRONOS_validation`; messages appear in logcat under `VALIDATION`.

Found on the desktop side on the way: `offlineshadercompiler` had no dxcompiler beside it
(multi-config `Release/`), so every incremental shader warm silently failed and edited shaders
never reached the cache - fixed in `Engine/CMakeLists.txt`. The shader metadata still records
`../../Simtary/shaders/...` dependency paths that do not resolve from a game's build folder,
so "outdated" detection is broken; `rebuild` is the reliable way after a header edit.
`disable_optimization` SPIR-V corrupts the heap inside spirv_reflect - do not use it.

Memory on the tablet in Scene1: ~1.4 GB process, ~830 MB of it graphics - render scale, ASTC
content and a smaller shadow atlas are the mobile budget items.

## Suggested order

Each step should end with something that builds or runs, so a regression has an obvious
cause.

1. ~~`cmake --preset android_arm64` and fix compile errors until the engine links.~~ Done.
2. ~~Gradle project and SDL activity glue — get a black window on a device.~~ Done (engine draws its init screen).
3. `AAssetManager` asset source and writable paths — get the engine past initialisation.
4. Vulkan surface loss and recreate — survive an app switch.
5. ASTC texture set — see the scene.
6. Mobile render path — see it at a playable frame rate.
7. Touch controls — play it.

## Things that will bite

- **`PLATFORM_LINUX` means "desktop Linux" in most of the engine.** It stays defined on
  Android on purpose, so POSIX code compiles, but every branch guarded by it — file
  dialogs in `wiHelper.cpp`, thread affinity in `wiJobSystem.cpp`, `wiNetwork_Linux.cpp` —
  needs reading before it can be trusted on a phone.
- **Do not enable `SetRenderOriginFollowsCamera` before physics is rebased.** See
  `Engine/stWorldScalar.h`.
- **`-DSIMTARY_LARGE_WORLD` changes `TransformComponent`'s layout.** Engine and game must
  be compiled with the same setting; mixing them is an ODR violation that shows up as
  garbled positions, not as a link error.
