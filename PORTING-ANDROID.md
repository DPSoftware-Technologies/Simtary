# Porting Simtary to Android

Status: **not ported.** What exists today is a set of seams so the port is a series of
contained changes rather than a rewrite. This file records what was prepared, what is
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

## Suggested order

Each step should end with something that builds or runs, so a regression has an obvious
cause.

1. `cmake --preset android_arm64` and fix compile errors until the engine links. Nothing
   runs yet; this is what turns the port from unknown into a finite list.
2. Gradle project and SDL activity glue — get a black window on a device.
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
