# Simtary

The shared engine for every game in this workspace.
```
Simtary/
├── Engine/         engine core
├── Framework/      the layer this workspace adds: st::App, st::Run, SceneManager,
│                   system UI, io/NBT, input, crash, lens flare, Faust, ZMQ
├── libs/           SDL2, ImGui, ImGuizmo, libzmq (submodules) + libgfx, faust
├── include/        vendored headers (faust ABI, stb_image)
├── assets/         ImGui + StLensFlare shaders, faust_arch.h
├── shaders/        compiled engine shader cache — shared, committed
├── deps/           sentry-native + openal-soft clones (gitignored, reused)
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
| `stApp.h` | `st::App` + `st::AppConfig`. Override `RegisterScenes`, `OnInitialize`, `OnUpdate`, `OnGUI`, `OnCompose`, `OnExit`. `Audio()` for Faust processors, `RequestQuit()` to exit. |
| `stScene.h` | `Scene` — `Load` / `Update` / `OnGUI` / `Unload`. |
| `SceneManager.h` | `Register` / `Load` / `Reload` / `Names`. Transitions are deferred to the next frame. |
| `io/PlayerPrefs.h`, `io/SaveGame.h`, `io/SettingsManager.h`, `io/UserData.h` | Per-user options and save games under `LocalLow/<organization>/<name>/`. |
| `input/InputSystem.h` | Action/axis keymap, refreshed once per frame. |
| `eventBus.h` | Main-thread publish/subscribe (`loading.progress`, `zmq.message`, …). |
| `anim/AnimationDescriptor.h` | NBT-backed animation descriptors. |
| `render/LensFlare.h` | Procedural screen-space flare. |
| `audio/faust/FaustProcessor.h` | `st::audio::FaustProcessor<T>` around an AOT Faust dsp. |
| `Engine/stNativeComponent.h` | The Unity-like native component model (engine core). |

Everything framework-side is in namespace `st::`.

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

## Shader cache

`shaders/` holds the compiled engine shader set and is staged into every game's
output before the incremental `offlineshadercompiler` pre-pass, so no project pays
the cold ~360-shader compile. After a shader-heavy change, publish the result back:

```
cmake --build <project>/build/win_x86-64 --target simtary_shadercache_update
```
