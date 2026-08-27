# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in the **Simtary engine**.

Simtary is the shared engine for every game in this workspace (`../Milistry`,
`../template`, and any new project). Nothing project-specific belongs here — if a
change only serves one game, it goes in that game's `src/`.

## Layout

```
Simtary/
├── Engine/         engine core. Its OWN git repo (DPSoftware-Technologies/Simtary4),
│                   a Wicked Engine fork: wi*.cpp/h, st* additions, Jolt, LUA, Utility,
│                   shaders/ (HLSL source), offlineshadercompiler
├── Framework/      the layer this workspace adds on top of the core (see below)
├── libs/           vendored third-party. ImGui/ImGuizmo/libzmq/SDL2 are git submodules,
│                   libgfx and faust are checked in
├── include/        vendored headers: faust ABI, stb_image
├── assets/         engine-side assets: ImGui + StLensFlare shaders, faust_arch.h
├── shaders/        COMPILED engine shader cache, committed. Staged into every game's
│                   output so no project pays the ~360-shader cold compile
├── deps/           gitignored git clones of sentry-native + openal-soft, reused by
│                   every project's FetchContent
├── crashreporter/  SimtaryCrashReporter — one reporter GUI for all games
├── cmake/          SimtaryBootstrap, SimtaryApp, SimtaryPlatform, IncrementBuild
├── tests/ tools/   nbt_test, make_player_descriptor
└── CMakeLists.txt
```

## Framework — what a game gets for free

Everything in `Framework/` is compiled INTO each game (not linked as a shared static
library) because each app needs its own generated `version.h` and `AppConfig`.

| File | Role |
|---|---|
| `stApp.h/.cpp` | `st::App` (a `wi::Application`) + `st::AppConfig` + `st::DevUIMode` + `st::LoadingState`. Owns render path, scene manager, ImGui, lens flare, Faust, ZMQ, settings. Project hook surface is listed below. |
| `stProject.h.in` | Template for the generated `stProject.h` (`ST_PROJECT_NAME` / `_ORGANIZATION` / `_COPYRIGHT` / `_VERSION`), filled from the `.stpd` manifest at configure time. |
| `stRun.h/.cpp` | `st::Run(argc, argv, config, app)` — window, splash, SDL event loop, input routing, shutdown. The project owns `main()`; this owns everything after it. |
| `stScene.h/.cpp` | `Scene` base class: `Load` / `Update` / `OnGUI` / `OnDevGUI` / `Unload`, plus `ReportProgress()`. |
| `SceneManager.*` | Named scene registry with deferred transitions + load/unload callbacks. |
| `ImguiHelper.cpp` | The hand-rolled ImGui backend and the per-frame UI ordering. |
| `devui.cpp`, `devui/` | **DevUI** — developer tooling, not game UI: menu bar, backlog, graphics settings, hierarchy/properties, scene manager, About. Gated by `AppConfig::devUI`. |
| `stLoading.cpp` | Default `RenderLoadingScreen` — the pipeline warm-up overlay plus whatever `LoadingState` carries. |
| `io/` | `Nbt` (the `.stad`/`.stcd` format), `NbtStore`, `SettingsManager`, `SaveGame`, `PlayerPrefs`, `UserData` (LocalLow path resolver). |
| `input/InputSystem.*` | Centralized action/axis keymap, refreshed once per frame. |
| `crash/CrashHandler.*` | sentry-native + Crashpad, offline only; launches `SimtaryCrashReporter`. |
| `render/LensFlare.*` | Procedural screen-space flare (`assets/shaders/StLensFlare*`). |
| `render/Projector.*` | `st::Projector` + `st::ProjectorSystem` — SQUARE (or rect/ellipse/rounded) image projection with projector optics: throw ratio, aspect, lens shift, keystone, barrel/pincushion, edge softness, vignette, plus a rectangular volumetric beam. Runs as a `RenderPath3D` custom post process (`assets/shaders/StProjectorCS.hlsl`), plus one depth-only pass per shadow-casting projector (`RenderShadows()`, driven from `st::App::Render()` BEFORE the render path so the command list is recorded ahead of the pass that samples it). Reach it anywhere via `st::ProjectorSystem::Get()`. |
| `render/ProjectorComponent.cpp` | The `"Projector"` NATIVE COMPONENT — attach `st::Projector` to a spot light from the editor (`NCI_0 = "Projector"`, optics as `NCA_0_*` args). Follows its own entity, takes the image off it (video / camera render / material base colour, pinned with `NCA_0_imageSource`), and zeroes that light by default since the light IS the circle. |
| `render/Framebuffer.*` | `st::gfx::Framebuffer` — an off-screen surface you draw into and hand to a material, a light mask or a projector. CPU mode wraps libgfx (`GFXcanvas`) and owns the staging texture, row pitch and flip; GPU mode is a render target you draw into with `wi::image`/`wi::font` between `Begin()`/`End()`. |
| `display/DisplaySettings.*` | Player-facing video options: window mode, monitor, resolution, refresh rate, v-sync, frame cap, render scale. NOT DevUI — `st::App::Display().GUI(app)` drops into a game's own options menu, and DevUI renders the same panel in its Display tab. Sole owner of v-sync and the frame cap; `GraphicsSettings` deliberately no longer carries them. |
| `audio/faust/` | `FaustManager` (OpenAL DSP host) + `FaustProcessor<T>`. Starts with no processors — games register their own AOT instruments. |
| `anim/`, `eventBus.*`, `ZmqHandler.*`, `SubWinStatus.*` | Animation descriptors, main-thread event bus, ZMQ bridge, native (Win32/X11) loading window. |

### Project hook surface (`st::App`)

Content: `RegisterScenes`, `OnInitialize`, `OnExit`.
Frame: `OnUpdate(dt)`, `OnFixedUpdate()`.
UI: `RenderUI()` (game UI, always), `RenderDevUI()` + `OnDevUIMenu()` (dev only),
`RenderLoadingScreen(state)`.
Render: `OnRenderPathSetup(path)`, `OnRender()`, `OnPreCompose(cmd)`, `OnCompose(cmd)`.
Input: `OnEvent(SDL_Event) -> bool` (true = consumed).
Scenes: `OnSceneLoaded(name)`, `OnSceneUnloaded(name)`.
Instance methods: `Audio()`, `RequestQuit()`, `IsDevUIVisible()/SetDevUIVisible()/ToggleDevUI()`,
`Loading()`, `SetLoadingStatus(text, percent)`.

**DevUI vs game UI is the important distinction.** Anything in `devui/` is tooling and
must stay behind `IsDevUIVisible()`. `AppConfig::devUI` is `Visible` (dev default),
`Hidden` (compiled in, `devUIToggleKey` opens it) or `Disabled` (never drawn;
`SetDevUIVisible(true)` is a deliberate no-op so a shipped build cannot be opened up).
When adding a panel, ask whether a player should ever see it — if not, it belongs in
`devui/` and behind that gate, or in `Scene::OnDevGUI()` rather than `Scene::OnGUI()`.

Namespace is `st::` throughout (`st::App`, `st::InputSystem`, `st::nbt`,
`st::userdata`, `st::crash`). `st` is also the file prefix used by the engine core
for its own additions (`stAudio`, `stNativeComponent`).

## Build

```powershell
cmake --preset win_x86-64            # -> build/win_x86-64
cmake --build --preset win_x86-64
```

Also builds every sibling game with `-DSIMTARY_BUILD_PROJECTS=ON` — the fast way to
check a framework change still compiles everywhere.

Out-of-source only; in-source builds are blocked. Presets are in `CMakePresets.json`;
`SIMTARY_PLATFORM_ARCH` (`cmake/SimtaryPlatform.cmake`) derives the `win_x86-64` tag
that keys the build directory.

Tests: `ctest` in the build dir (currently `nbt_test`).

`build_number.txt` is project-level. The `SIMTARY_BUILD_PROJECTS` sweep forces
`SIMTARY_BUMP_BUILD_NUMBER=OFF` so a workspace compile-check never advances a game's
counter; only a build of that project does.

## The CMake API games use

`cmake/SimtaryBootstrap.cmake` is the single include a game needs; it pulls this
whole workspace in with `add_subdirectory(... EXCLUDE_FROM_ALL)` and exposes:

- `simtary_add_app(NAME ... ORGANIZATION ... ICON ... [SOURCE_DIR] [ASSETS_DIR] [CONTENT_SUBDIR] [EXTRA_SOURCES|INCLUDES|LIBS] [NO_SHADER_WARM] [NO_CRASH_REPORTER])`
- `simtary_compile_shader(TARGET ... SOURCE ... PROFILE ... [ENTRY] [OUTPUT_NAME])`
- `simtary_faust_regen(NAME ... CLASS ... DSP ... OUTPUT ...)`
- `Simtary::AppFlags` — the exceptions-off / RTTI-off contract as an INTERFACE target.
  Games are sibling directories, so they do NOT inherit this workspace's
  `add_compile_options`; `simtary_add_app` links the target for them. Any new app-side
  target must link it too, or its objects will not be ABI-compatible with the engine.

Changing the flags means changing them in ONE place: `SIMTARY_APP_OPTIONS` /
`SIMTARY_APP_DEFINITIONS` in `CMakeLists.txt`, which feeds both the directory-scope
`add_compile_options` and `Simtary::AppFlags`.

## Design details worth knowing before changing things

**Third-party build order is load-bearing.** sentry, libzmq and openal-soft are added
*before* the project-wide `/EHsc- /GR- /_HAS_EXCEPTIONS=0` flags, because they use
exceptions/RTTI internally and must build with their own defaults. libgfx is added
*after*, because it uses neither and must stay ABI-consistent with the engine.

**A blocking `Scene::Load()` is why there are two loading screens.** No ImGui frame
can be drawn while the main thread is inside `Load()`, so `st::App::Update` raises the
native `SubWinStatus` window (its own thread, Win32/X11, no SDL) around any scene
transition, and `Scene::ReportProgress()` writes into it. The ImGui
`RenderLoadingScreen` overlay only covers the non-blocking cases — chiefly the
first-launch pipeline warm-up. Do not "simplify" one into the other.

**Assets sync on a custom target, not on POST_BUILD.** `<APP>_Assets` copies
`assets/` to `<build>/assets` and `assets/contents/` to `<exe>/assets`, and the app
target depends on it. It has to be a custom target: a POST_BUILD command only runs
when the executable is actually relinked, so editing nothing but a scene or a texture
left the stale copy in the output and the build reported success. Custom targets are
always out of date, so `cmake --build` re-syncs content either way.

The copy is `copy_directory_if_different` — it adds and overwrites, it never deletes.
After removing or renaming content, build `<APP>_AssetsResync`, which wipes both
output copies first; otherwise the old file is still sitting next to the exe and the
game happily keeps loading it.

**The `.stpd` manifest is build-time only, and identity only.** `assets/project.stpd`
is read by CMake at configure time and never at runtime; it lives in `assets/` rather
than `assets/contents/` precisely because `contents/` is what ships next to the exe.
It carries name/organization/copyright/version/icon and nothing else — a window size
or a DevUI mode in there would make every tweak a rebuild, so those stay in
`st::AppConfig`. Adding a manifest field means: a flag plus an `EmitCMake` line in
`tools/make_project_descriptor.cpp`, a hoist in `simtary_read_project_descriptor`, and
a use in `simtary_add_app`.

**The descriptor reader is bootstrapped, not a normal target.** CMake cannot parse NBT
and needs the values *while* a project is configuring, before its targets exist — so
`SimtaryProject.cmake` configures and builds `tools/descriptor-bootstrap` into
`<build>/_descriptor` and caches the path in `SIMTARY_DESCRIPTOR_TOOL`. That bootstrap
deliberately links only `Nbt.cpp`, which depends on nothing beyond the standard
library; keep it that way or configure time balloons.

**Presets pin the generator on purpose.** Leaving `generator` unset in
`CMakePresets.json` lets the CLI and the IDE configure the same `build/<arch>` with
different toolchains — one Visual Studio, one Ninja+mingw — and the second poisons the
cache (`CMAKE_C_COMPILE_OBJECT` errors on the next regenerate). The Windows preset is
pinned to the Visual Studio generator; `win_x86-64-ninja` is the separate opt-in.

**Scene update runs exactly once per frame.** `st::App::Initialize` calls
`renderPath.setSceneUpdateEnabled(false)`; scenes call `scene.Update(dt)` themselves
from `SceneManager::Update`. A second update per frame swaps `MeshComponent`'s
`so_pos`/`so_pre` streamout views twice, which cancels out — skinned meshes then get
garbage velocity (broken motion blur, TAA, FSR2).

**ImGui integration is hand-rolled** on the engine's graphics device.
`ImGui::Render()` runs in `Update()` (not `Compose()`) so draw data is ready before
any GPU pass; `Compose()` submits it with `device->AllocateGPU()` transient buffers.
Its shaders live in `assets/shaders/ImGui{VS,PS}.hlsl` and are compiled by
`simtary_add_app`.

**Lens flare shaders are named `StLensFlare*`, not `LensFlare*`** — the engine ships
its own `lensFlareVS/PS.hlsl` and both land in the same output folder, which is
case-insensitive on Windows.

**A framework shader that includes `globals.hlsli` needs `ENGINE_ENV`.**
`simtary_compile_shader(... ENGINE_ENV ...)` adds the engine's include path plus the
DX12 default root signature / the Vulkan binding shifts, which the bare dxc call the
other framework shaders use does not have. Runtime compilation is not a fallback
here: `dxcompiler.dll` is not shipped next to the exe, so `wi::renderer::LoadShader`
can only ever load a `.cso` that the build already produced. `StProjectorCS.hlsl` is
the first shader in this category.

**Why the projector is not a `LightComponent`.** `light_spot()` in
`Engine/shaders/lightingHF.hlsli` clips to a circular cone, so a mask texture, a
video or a camera render on a spot light always lands as a CIRCLE with the image
cropped inside it. Squaring that means changing the cone test in the engine core
repo. `st::ProjectorSystem` gets the same picture without touching `Engine/`, at the
cost of being screen space: no shadow map (there is a screen-space approximation), no
lighting of transparent surfaces, no contribution to reflections or GI. Note also
that a spot light projects along its entity's local **-Y** (see `SHCAM::init` in
`wiRenderer.cpp`) while `LightComponent::direction` stores the opposite vector —
`Projector::Forward::MinusY` is the setting that matches a spot light.

**The projector renders its own shadow map, and it has to.** Without one the image
passes through walls: it is a screen-space pass, so the only depth it can otherwise
consult is the CAMERA's, which says nothing about what stands between the lens and a
lit point. `ProjectorSystem::RenderShadows()` builds a `CameraComponent` from the
projector frustum, runs `wi::renderer::UpdateVisibility` + `DrawScene(RENDERPASS_SHADOW)`
into a D16 depth map, and the shader compares against it (reverse-Z, 2x2 PCF). The
gate matrix comes from that same CameraComponent, so the two can never drift apart.
`Projector::occlusion` is the old screen-space march, kept only as the fallback when
`shadows` is off — it cannot see a blocker the camera does not render.

**Shader cache.** `Simtary/shaders/` is staged into every game's output before the
incremental `offlineshadercompiler` pre-pass, so first launch is never cold. After a
shader-heavy change, publish the result back with the
`simtary_shadercache_update` target.

**Exceptions and RTTI are off everywhere** (`/EHsc-`, `/GR-`; `-fno-exceptions`,
`-fno-rtti`). Do not introduce `throw`, `dynamic_cast` or `typeid` in engine or app
code. Native components use address-based `GetNativeTypeID<T>()` for type identity
for exactly this reason.

**Native Components** (`Engine/stNativeComponent.{h,cpp,_inl.h}`) are an engine-core
feature, not a framework one: a Unity-like C++ component model attached data-driven
through the engine `MetadataComponent`, so it stays editable in the Wicked Editor.

- `NCI_<LocalID>` (string) = `"ComponentName"` — attaches the registered component.
  `LocalID` lets you stack the same component several times on one entity.
- `NCA_<LocalID>_<ArgName>` (bool/int/float/string) = a parameter for that instance.

```cpp
struct Spinner : wi::scene::NativeComponent {
    float speed = 45.0f;
    void Start() override { Bind(speed, "speed"); }       // reads NCA_<id>_speed
    void Update(float dt) override {
        if (auto* t = GetComponent<TransformComponent>())
            t->RotateRollPitchYaw(XMFLOAT3(0, wi::math::DegreesToRadians(speed)*dt, 0));
    }
};
ST_REGISTER_NATIVE_COMPONENT(Spinner)                     // self-registers at static init
```

Runtime state lives in `Scene::nativeComponents` (not serialized — rebuilt from
metadata each `Update`); the system runs single-threaded via
`Scene::RunNativeComponentUpdateSystem`, right after the script system.

**Graphics API**: DirectX 12 on Windows, Vulkan on Linux (the SDL2 window is created
with `SDL_WINDOW_VULKAN`). Pass `vulkan` as a command-line argument to force Vulkan on
Windows. Other useful args: `debugdevice`, `gpuvalidation`, `igpu`, `nvidiagpu`,
`amdgpu`.

## Git

`Engine/` is a separate repository with its own remote — commits there go upstream to
Simtary4, so keep workspace-specific changes in `Framework/` unless the change is
genuinely an engine-core fix. `libs/ImGui`, `libs/ImGuizmo`, `libs/libzmq` and
`libs/SDL2` are submodules of THIS repository.
