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
| `stApp.h/.cpp` | `st::App` (a `wi::Application`) + `st::AppConfig`. Owns render path, scene manager, ImGui, lens flare, Faust, ZMQ, settings. Project hooks: `RegisterScenes`, `OnInitialize`, `OnUpdate`, `OnGUI`, `OnCompose`, `OnExit`. |
| `stRun.h/.cpp` | `st::Run(argc, argv, config, app)` — window, splash, SDL event loop, input routing, shutdown. The project owns `main()`; this owns everything after it. |
| `stScene.h` | `Scene` base class: `Load` / `Update` / `OnGUI` / `Unload`. |
| `SceneManager.*` | Named scene registry with deferred transitions. |
| `ImguiHelper.cpp`, `sysui.cpp`, `sysui/` | The hand-rolled ImGui backend and the system UI: menu bar, backlog, graphics settings, hierarchy/properties, scene manager, About. |
| `io/` | `Nbt` (the `.stad`/`.stcd` format), `NbtStore`, `SettingsManager`, `SaveGame`, `PlayerPrefs`, `UserData` (LocalLow path resolver). |
| `input/InputSystem.*` | Centralized action/axis keymap, refreshed once per frame. |
| `crash/CrashHandler.*` | sentry-native + Crashpad, offline only; launches `SimtaryCrashReporter`. |
| `render/LensFlare.*` | Procedural screen-space flare (`assets/shaders/StLensFlare*`). |
| `audio/faust/` | `FaustManager` (OpenAL DSP host) + `FaustProcessor<T>`. Starts with no processors — games register their own AOT instruments. |
| `anim/`, `eventBus.*`, `ZmqHandler.*`, `SubWinStatus.*` | Animation descriptors, main-thread event bus, ZMQ bridge, native (Win32/X11) loading window. |

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
