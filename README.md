# Simtary
A High Mobility Multipurpose Workspace Visualizator. Based on Wicked Engine
<img width="1920" height="1032" alt="Screenshot 2026-09-06 035900" src="https://github.com/user-attachments/assets/7c953d28-9f9c-42d9-b6e6-34aceb5f3fb5" />

The shared engine for every game in this workspace.
```
Simtary/
├── Engine/         engine core
├── Framework/      the layer this workspace adds: st::App, st::Run, SceneManager,
│                   system UI, io/NBT, input, crash, lens flare, projectors,
│                   lasers + optics, raycasts, Faust, ZMQ
├── libs/           SDL2, ImGui, ImGuizmo, libzmq (submodules) + libgfx, faust
├── include/        vendored headers (faust ABI, stb_image)
├── assets/         ImGui + StLensFlare + StProjector + StLaser shaders, faust_arch.h
├── shaders/        compiled engine shader cache — shared, committed
├── deps/           sentry-native + openal-soft + JSBSim clones (gitignored, reused)
├── crashreporter/  SimtaryCrashReporter — one reporter GUI for all games
├── cmake/          SimtaryBootstrap / SimtaryApp / SimtaryPlatform / IncrementBuild
└── tests/ tools/
```

## Using it from a game

```cmake
cmake_minimum_required(VERSION 3.19)
project(MyGame VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../Simtary/cmake/SimtaryBootstrap.cmake)

simtary_add_app(NAME MyGame ORGANIZATION "YourStudio" ICON assets/appicon.ico)
```

```cpp
// src/main.cpp
#include "stRun.h"
#include "scenes/MainScene.h"

class MyGame : public st::App {
protected:
    void RegisterScenes (SceneManager& scenes) override {
        scenes.Register("Main", std::make_unique<MainScene>());
    }
};

int main (int argc, char* argv[]) {
    st::AppConfig config;
    config.name         = "MyGame";
    config.organization = "YourStudio";
    config.startupScene = "Main";
    MyGame app;
    return st::Run(argc, argv, config, app);
}
```

`../template` is a working project laid out exactly like this — copy it.

## Public API

| Header | What |
|---|---|
| `stRun.h` | `st::Run(argc, argv, config, app)` — window, splash, event loop, shutdown. |
| `stApp.h` | `st::App` + `st::AppConfig` + `st::DevUIMode` + `st::LoadingState`. See the hook table below. |
| `stScene.h` | `Scene` — `Load` / `Update` / `OnGUI` / `OnDevGUI` / `Unload`, plus `ReportProgress()`. |
| `SceneManager.h` | `Register` / `Load` / `Reload` / `Names`. Transitions are deferred to the next frame. |
| `io/PlayerPrefs.h`, `io/SaveGame.h`, `io/SettingsManager.h`, `io/UserData.h` | Per-user options and save games under `LocalLow/<organization>/<name>/`. |
| `display/DisplaySettings.h` | `st::DisplaySettings` — window mode, monitor, resolution, refresh rate, v-sync, frame cap, render scale, graphics backend. `GUI()` has no `Begin/End`, so it drops into a game's own options menu. |
| `render/GraphicsAPI.h` | `st::GraphicsAPI` — DirectX 12 or Vulkan, chosen at startup and switchable on Windows. `ActiveGraphicsAPI()` is what came up, `AppConfig::graphicsAPI` is the project's default, and the player's own choice lives in the Display panel. |
| `input/InputSystem.h` | Action/axis keymap, refreshed once per frame. |
| `eventBus.h` | Main-thread publish/subscribe (`loading.progress`, `zmq.message`, …). |
| `anim/AnimationDescriptor.h` | NBT-backed animation descriptors. |
| `render/LensFlare.h` | Procedural screen-space flare. |
| `render/Projector.h` | `st::Projector` / `st::ProjectorSystem` — square, rectangular, elliptical or rounded image projection with projector optics (throw ratio, aspect, lens shift, keystone, distortion, softness, vignette) and a matching volumetric beam. `st::ProjectorSystem::Get()` from anywhere. |
| `render/ProjectorComponent.cpp` | `"Projector"` native component — the same thing attached from the editor: `NCI_0 = "Projector"` on a spot light, optics as `NCA_0_*` args. |
| `render/Laser.h` | `st::Laser` / `st::LaserSystem` — traced laser beams: millimetre-thin core plus halo, analytically integrated (no ray march), reflected off mirrors and bent through lenses, stopped on the first surface the ray hits. The impact spot leaves a fading **persistence trail**, so a moving beam draws a line instead of blinking a dot. `st::LaserSystem::Get().Path(id)->hit` is what the beam is on. |
| `render/LaserComponent.cpp` | `"sticLaser"` native component — the same thing attached from the editor. Picks up a `"sticRay"` on the same entity instead of casting twice. |
| `render/Optics.h` | `st::Mirror` / `st::Lens` / `st::OpticsSystem` — what laser beams reflect off and refract through: aperture, reflectance/transmittance, tint, scatter, beam scale, spread, and a `Lens::Type` of Spherical / Cylindrical / Toric / Aspheric / Axicon / Prism / Window. Draws nothing itself; `Trace()` is the walk that turns one ray into a list of legs. |
| `render/OpticsComponents.cpp` | `"sticMirror"` / `"sticLens"` native components. |
| `scene/Ray.h` | `st::Raycast` / `st::RayHit` / `st::RayQuery` — one raycast over either backend: mesh (`Scene::Intersects`, no physics body needed) or Jolt (`wi::physics::Intersects`), or both with the nearer hit kept. |
| `scene/RayComponent.h` | `"sticRay"` native component — a raycast bolted to an entity, re-cast every frame. The shared seam for a laser sight, a rangefinder, an interaction prompt and an “am I aiming at it” HUD. |
| `render/Framebuffer.h` | `st::gfx::Framebuffer` — draw off screen (libgfx canvas, or `wi::image`/`wi::font` on a render target) and bind the result to a material, a light mask or a projector. |
| `audio/faust/FaustProcessor.h` | `st::audio::FaustProcessor<T>` around an AOT Faust dsp. |
| `Engine/stNativeComponent.h` | The Unity-like native component model (engine core). |

Everything framework-side is in namespace `st::`.

### st::App hooks

Every one is optional and does nothing by default.

| Hook | When |
|---|---|
| `RegisterScenes(SceneManager&)` | once at startup, before the start scene loads |
| `OnInitialize()` | framework up, start scene loaded |
| `OnExit()` | before teardown |
| `OnUpdate(dt)` | each frame, after the scene manager |
| `OnFixedUpdate()` | the engine's fixed tick — physics-rate logic |
| `RenderUI()` | the game's ImGui. Drawn every frame, whatever `devUI` says |
| `RenderDevUI()` | extra developer panels; only while DevUI is visible |
| `OnDevUIMenu()` | add a menu to the DevUI main menu bar |
| `RenderLoadingScreen(const LoadingState&)` | replace the loading overlay with your own art |
| `OnRenderPathSetup(RenderPath3D&)` | configure the path — or activate one of your own |
| `OnRender()` | the game's own GPU work, after the engine renders |
| `OnPreCompose(cmd)` | draw under the composed 3D frame |
| `OnCompose(cmd)` | draw over the 3D frame, under the UI |
| `OnEvent(const SDL_Event&) -> bool` | raw SDL before the engine; `true` consumes it |
| `OnSceneLoaded(name)` / `OnSceneUnloaded(name)` | around a scene transition |

Plus, on the instance: `Audio()` (the Faust host), `RequestQuit()`,
`IsDevUIVisible()` / `SetDevUIVisible()` / `ToggleDevUI()`, `Loading()` and
`SetLoadingStatus(text, percent)`.

### DevUI

The menu bar, backlog, graphics settings, hierarchy/properties, scene manager and
Faust panel are **developer tooling**, not game UI. A project chooses how much of it
a build exposes:

```cpp
config.devUI          = st::DevUIMode::Hidden;   // Visible | Hidden | Disabled
config.devUIToggleKey = SDL_SCANCODE_F1;         // 0 = no toggle
```

`Visible` opens it at startup (the development default). `Hidden` compiles it in but
starts closed. `Disabled` never draws it, never calls `RenderDevUI()` or
`Scene::OnDevGUI()`, and makes `SetDevUIVisible(true)` a no-op — a shipped build
stays shut. Panels live in `Framework/devui/`.

#### Saving a scene from the editor

`Scene > Save` writes a `.stsd`: the map's entities, plus the LIST of resources it
references — the bytes stay in the asset package. So a save has to put those bytes
somewhere, and it writes them twice, for two different times:

| Written | Where | Read by |
|---|---|---|
| sidecar package | `<name>.strd` beside the map | this session, mounted immediately, so the map reloads now |
| loose resources | the project's `assets/resources/` | the next build — `stpack pack --resource-dir` merges the folder into the game package |

The second is what carries a model imported in the editor into the shipped game. Files
already in the folder are left alone, so a repeat save costs a stat() per resource;
`Scene > Export resources on save` turns it off. The destination is
`AppConfig::editorResourceDir`, or the project's own `assets/resources` when that is
empty — the build bakes the path in, because the running game sits in the build output
and the folder the packer reads is beside the project's sources.

### Project descriptor

`<project>/assets/project.stpd` is the project's **build-time manifest** — the
equivalent of Unreal's `.uproject`. It is NBT (same container as `.stad` options and
`.stcd` saves) and CMake reads it **at configure time only**. It never ships: note
that it sits in `assets/`, not `assets/contents/`, which is the folder copied next to
the executable.

```
project : name, organization, copyright, version
build   : icon, target_name
```

Identity only. Runtime properties — window size, startup scene, DevUI mode — are
deliberately not in it: baking them into the exe would mean a rebuild to change them,
so they stay in `st::AppConfig` in `src/main.cpp`, and user-facing ones end up in
`options.stad`.

What the build does with it:

- fills the executable's version resource (`CompanyName`, `ProductName`,
  `LegalCopyright`, `ProductVersion`) and compiles in `build.icon`
- generates `stProject.h` beside the generated `version.h`, so `main.cpp` states the
  identity once:

```cpp
#include "stProject.h"
config.name         = ST_PROJECT_NAME;
config.organization = ST_PROJECT_ORGANIZATION;
config.copyright    = ST_PROJECT_COPYRIGHT;
```

Explicit `simtary_add_app()` arguments still win over the file, and a project with no
descriptor keeps the CMake defaults.

Being NBT it is binary, so it is authored with the generator:

```
make_project_descriptor assets/project.stpd --name "My Game" --version 1.2.0
make_project_descriptor assets/project.stpd --dump
```

The tool reads the existing file first and changes only the flags you pass, so
regenerating never drops keys the build does not know about. `--help` lists them all.

CMake cannot parse NBT, and the values are needed *while* a project is configuring —
before any of its targets can be built. So `SimtaryProject.cmake` configures and
builds the reader from `tools/descriptor-bootstrap` (two files, no engine, no
third-party) into `<build>/_descriptor` on the first configure, then runs it with
`--cmake` and `include()`s the `set()` lines it prints. Later configures reuse the
cached exe.

### Video options

Resolution, window mode and the rest are **player-facing**, so they are a framework
module rather than DevUI. `st::App::Display()` hands back the live
`st::DisplaySettings`; its `GUI()` draws the whole panel without opening a window, so
a game renders it inside its own menu:

```cpp
void MyGame::RenderUI () {
    ImGui::Begin("Options");
    Display().GUI(*this);   // window mode, monitor, resolution, v-sync, FPS cap, render scale
    ImGui::End();
}
```

DevUI shows the same panel in the Graphics Settings window's **Display** tab.

| Option | Notes |
|---|---|
| Window Mode | Windowed · Borderless Fullscreen · Fullscreen (exclusive, changes the display mode) |
| Monitor | only shown when more than one display is attached |
| Resolution | window size, or the exclusive mode. Disabled for borderless, which is always desktop-sized |
| V-Sync | `wi::eventhandler::SetVSync` |
| Limit Frame Rate / Target FPS | `wi::Application::setFrameRateLock` / `setTargetFrameRate` |
| Render Scale | renders at a fraction of the output and upscales, via `wi::Application::SetRenderResolution` |
| Standby: unfocused FPS | caps the frame rate while the window has no input focus |
| Standby: idle FPS + idle-after | caps it after N seconds with no keyboard/mouse/controller input |
| Backend | Auto · DirectX 12 · Vulkan — which graphics API the NEXT launch uses (Windows; Linux is Vulkan only) |

The standby options are the exception to the staging below: they apply live, because
they change nothing a player has to confirm. `UpdateStandby()` reads them every frame
from `st::App::Update`, and `st::Run` resets the idle timer on real input.

Worth knowing: the engine has its own inactive path (`wiApplication.cpp` sleeps and
skips the frame entirely when `is_window_active` is false), but nothing in this fork
ever assigns that flag, so it never fires — and a hard stop would leave the window
unrepainted. Capping the frame rate keeps the window alive while still handing the GPU
back.

Edits are staged behind **Apply**/**Revert** — a resolution must never change while
someone is scrolling the dropdown. Applied values persist to `options.stad` under the
`display` compound and are re-applied at startup, before the render path is built.

**Backend** is the other exception, in the opposite direction: a graphics device owns
every texture, buffer, pipeline and shader the engine has, so it cannot be swapped in a
running process. The choice is saved as edited and read back by `st::ResolveGraphicsAPI()`
on the next start; the panel shows what is running now (`Running DirectX 12 on NVIDIA
GeForce ...`) and offers **Restart Now** — `st::RequestRestart()`, which shuts the game
down normally, writes `options.stad`, and starts the same command line again. See
[Graphics backend](#graphics-backend) for the full resolution order.

The engine-quality knobs (AO, shadows, post, tonemapping, FSR/FSR2, MSAA) stay on the
Graphics Settings **Engine** tab and are DevUI-only — expose your own curated subset
from the game if players should reach them.

### Loading progress

`Scene::Load()` blocks the main thread, so no ImGui frame can be drawn during a
transition. The framework raises the native `SubWinStatus` window (its own thread)
for the duration; `Scene::ReportProgress(percent, status)` writes into it:

```cpp
void MyScene::Load() {
    ReportProgress(0,  "creating sky");
    ReportProgress(40, "building ground");
    ReportProgress(100, "ready");
}
```

`st::App::RenderLoadingScreen(state)` covers the non-blocking cases — chiefly the
first-launch shader/pipeline warm-up — and is overridable for custom art.

## CMake API

- `simtary_add_app(NAME ... [ORGANIZATION] [ICON] [SOURCE_DIR] [ASSETS_DIR] [CONTENT_SUBDIR] [EXTRA_SOURCES|INCLUDES|LIBS] [NO_SHADER_WARM] [NO_CRASH_REPORTER])`
- `simtary_compile_shader(TARGET ... SOURCE ... PROFILE ... [ENTRY] [OUTPUT_NAME])`
- `simtary_faust_regen(NAME ... CLASS ... DSP ... OUTPUT ...)`
- `Simtary::AppFlags` — the exceptions-off / RTTI-off contract. `simtary_add_app`
  links it; any other app-side target must link it too.

## Building the engine on its own

```
cmake --preset win_x86-64
cmake --build --preset win_x86-64
```

Add `-DSIMTARY_BUILD_PROJECTS=ON` to build every sibling game in the same tree — the
fast way to check a framework change still compiles everywhere. `ctest` runs the
framework tests.

`build_number.txt` is project-level, so only a build of that project advances it: the
sweep above forces `SIMTARY_BUMP_BUILD_NUMBER=OFF` and leaves every game's counter
untouched. Pass `-DSIMTARY_BUMP_BUILD_NUMBER=OFF` by hand for CI or throwaway builds.

## Graphics backend

Windows builds contain both backends and pick one at startup; Linux is Vulkan only.
The choice is resolved by `st::ResolveGraphicsAPI()` (`Framework/render/GraphicsAPI.h`),
highest source first:

| # | Source | Set by |
|---|---|---|
| 1 | command line | `vulkan` / `dx12` |
| 2 | `options.stad`, `display` -> `graphicsAPI` | the player, in the Display panel |
| 3 | `AppConfig::graphicsAPI` | the project, in its `main.cpp` |
| 4 | the platform default | DirectX 12 on Windows, Vulkan elsewhere |

A backend whose runtime is not installed falls back to the other one rather than
failing to start, which is also why the command line outranks the saved choice: a
machine that saved a backend it cannot run is still startable with `dx12`.

The device is created by `st::App::CreateGraphicsDevice()` from `st::Run`, before
`Application::SetWindow` — the seam the engine documents for a custom device — and it
points `wi::renderer`'s shader path at `shaders/hlsl6/` or `shaders/spirv/` to match.
Switching backends is a restart, because the device owns every resource the engine has;
`st::RequestRestart()` is what the Display panel's **Restart Now** calls.

Other useful arguments: `debugdevice`, `gpuvalidation`, `gpu_verbose`, `igpu`,
`nvidiagpu`, `amdgpu`, `intelgpu`.

## Shader cache

`shaders/` holds the compiled engine shader set — `hlsl6/` for DirectX 12 and `spirv/`
for Vulkan — and is staged into every game's output before the incremental
`offlineshadercompiler` pre-pass, so no project pays the cold ~360-shader compile. On
Windows both are warmed, because either backend can be the one that launches. After a
shader-heavy change, publish the result back:

```
cmake --build <project>/build/win_x86-64 --target simtary_shadercache_update
```

The framework's own shaders (ImGui, lens flare, projector, laser) are built by
`simtary_compile_shader`, which compiles each one for every backend the platform can
run. It uses `stshaderc` (`tools/stshaderc.cpp`) rather than `dxc.exe`: the dxc that
ships with the Windows SDK cannot emit SPIR-V at all, so a Windows machine without the
Vulkan SDK could not build a game that runs on Vulkan. `stshaderc` drives the
`dxcompiler` vendored in `Engine/` through `wi::shadercompiler`, which also means the
DX12 root signature and the Vulkan binding shifts come from the engine's own constants
instead of a copy of them in CMake.

# New game from Template
Please Contact to `contact@platoonlabs.com` for get template project.
