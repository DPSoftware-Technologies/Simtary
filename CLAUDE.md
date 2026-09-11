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
├── deps/           gitignored git clones of sentry-native, openal-soft, tinygltf and
│                   ufbx, reused by every project's FetchContent
├── crashreporter/  SimtaryCrashReporter — one reporter GUI for all games
├── cmake/          SimtaryBootstrap, SimtaryApp, SimtaryPlatform, IncrementBuild
├── tests/ tools/   nbt_test, asset_pack_test, model_import_test, stpack, stshaderc,
│                   make_player_descriptor
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
| `SceneManager.*` | Named scene registry with deferred transitions + load/unload callbacks. Also `DiscoverScenes(folder)` (register every map in the scene folder as a scene of its own), `NewScene(name)` (an empty `st::RuntimeScene`) and `OriginOf(name)` (C++ / folder / runtime). |
| `scene/SceneFile.*` | `st::LoadSceneFileFlat()` - load a `.stsd` / `.wiscene` so its own top-level nodes land at the ROOT of the hierarchy instead of under one wrapper entity, and hand back every entity it created so `st::UnloadSceneFile()` can remove the lot. `ResolveSceneFile()` picks the packed `.stsd` over the source `.wiscene`. |
| `scene/FileScene.*` | `st::FileScene` - a scene that is nothing but a map file, loaded flat. What `DiscoverScenes()` registers for each map it finds. |
| `scene/RuntimeScene.*` | `st::RuntimeScene` - the empty scene the Scene Manager's "New scene" creates: a sun and a sky, and an `Unload()` that removes everything added while it was active. |
| `ImguiHelper.cpp` | The hand-rolled ImGui backend and the per-frame UI ordering. |
| `devui.cpp`, `devui/` | **DevUI** — developer tooling, not game UI: menu bar, backlog, graphics settings, hierarchy/properties, scene manager, About. Gated by `AppConfig::devUI`. |
| `devui/imassets.*` | **Resource Explorer** — browse a mounted package (which part, what offset, which codec, what hash), and edit one: add / remove / rename / recompress assets and write it back. Lives ONLY in Editor mode, docked along the bottom. Reached through `App::Resources()`. |
| `devui/imcomponentinspectors.*` | **The Properties field editors** — one function per `wi::scene::Scene` component manager, every option each component has. All 38 managers are covered, so the inspector's "no inline editor" list is a missing-table report, not a normal state. `imcomponents.*` stays the ADD/REMOVE catalogue; this is the EDIT surface. Name and Transform are drawn by `imhierarchy.cpp` instead, because they carry panel behaviour (shared row, euler cache, drift check) rather than a field list. |
| `stLoading.cpp` | Default `RenderLoadingScreen` — the pipeline warm-up overlay plus whatever `LoadingState` carries. |
| `io/` | `Nbt` (the `.stad`/`.stcd` format), `NbtStore`, `SettingsManager`, `SaveGame`, `PlayerPrefs`, `UserData` (LocalLow path resolver). |
| `io/model/` | **Model import**: `.gltf`/`.glb` through tinygltf v3, `.fbx` and `.obj`/`.mtl` through ufbx. `st::model::Import()` builds nodes, meshes, PBR materials, skins and animations straight into a `wi::scene::Scene`. Both loaders are C — this workspace is `_HAS_EXCEPTIONS=0`, and a throwing C++ parser would need its own exception settings behind a C boundary. Every byte reaches them through `wi::helper::FileRead`, so a model inside a mounted asset package imports like one on disk. |
| `io/asset/` | **The asset package**: `.strd` index + `.stafp<N>` payload parts + `.stsd` maps. `AssetFormat.h` is the on-disk layout, `AssetPack` reads it, `AssetPackWriter` builds it, `SceneDescriptor` splits and rebuilds `.wiscene`, `StHash.h` is XXH64. `AssetSystem` is the only engine-aware file: it mounts packages and installs `wi::helper::SetAssetSourceOverride` so every engine read resolves through them. Reach it anywhere via `st::AssetSystem::Get()`, or `App::Assets()`. |
| `input/InputActions.*` | **The action-map input model**, in the shape Unity's Input System uses: `Registry` (every map the game registered) → `ActionMap` ("Player", "UI") → `Action` (a name plus a control type: Button / Axis / Vector2) → `Binding` (one control, optionally a composite arm, with scale/invert processors). `ActionRuntime` evaluates one map for one player index. Bindings cover keyboard, mouse (button, delta, wheel), gamepad (button, axis, stick) and touch. |
| `input/InputComponent.*` | The `"stInput"` NATIVE COMPONENT — Simtary's `PlayerInput`. Attach it to the entity that should receive input (`NCA_0_actionMap`, `NCA_0_playerIndex`), and sibling components read the actions off it as a local component instead of out of a singleton. |
| `input/InputSystem.*` | Owns the `Registry` (`Actions()`), the ImGui/editor device gating, and SDL relative-mouse ownership. Also still carries the ORIGINAL flat keymap (`Down("Sprint")`, `Axis("MoveX")`, `MoveVector()`) unchanged, for the scenes written against it. |
| `crash/CrashHandler.*` | sentry-native + Crashpad, offline only; launches `SimtaryCrashReporter`. |
| `render/LensFlare.*` | Procedural screen-space flare (`assets/shaders/StLensFlare*`). Hidden by whatever stands in front of the sun: the vertex shader takes nine taps of the render path's depth copy at the sun's screen position and the fraction that is still sky fades the whole flare. |
| `render/Projector.*` | `st::Projector` + `st::ProjectorSystem` — SQUARE (or rect/ellipse/rounded) image projection with projector optics: throw ratio, aspect, lens shift, keystone, barrel/pincushion, edge softness, vignette, plus a rectangular volumetric beam. `opticBounces` reflects the image off `st::Mirror` and images it through `st::Lens` by uploading a virtual projector clipped to that element's aperture. Runs as a `RenderPath3D` custom post process (`assets/shaders/StProjectorCS.hlsl`), plus one depth-only pass per shadow-casting projector (`RenderShadows()`, driven from `st::App::Render()` BEFORE the render path so the command list is recorded ahead of the pass that samples it). Reach it anywhere via `st::ProjectorSystem::Get()`. |
| `render/ProjectorComponent.cpp` | The `"Projector"` NATIVE COMPONENT — attach `st::Projector` to a spot light from the editor (`NCI_0 = "Projector"`, optics as `NCA_0_*` args). Follows its own entity, takes the image off it (video / camera render / material base colour, pinned with `NCA_0_imageSource`), and zeroes that light by default since the light IS the circle. |
| `render/Laser.*` | `st::Laser` + `st::LaserSystem` — traced laser beams. Millimetre-thin core plus a wide halo, both integrated ANALYTICALLY per pixel (`assets/shaders/StLaserCS.hlsl`) rather than ray marched. Walks the beam through the optics below and stops it on the first surface `st::Raycast` reports, so one laser can be several straight legs. The impact spot leaves a fading persistence trail, which is what makes a moving beam draw a line instead of blinking a dot. `LaserArray` turns one laser into a grid/ring/fan/cross/spiral of rays (off by default — each ray is a full trace). `st::LaserSystem::Get().Path(id)->hit` is what the beam is on. |
| `render/LaserComponent.cpp` | The `"sticLaser"` NATIVE COMPONENT. Follows its own entity; picks up a sibling `"sticRay"` (mode/range/axis) every frame instead of casting a second time. |
| `render/Optics.*` | `st::Mirror` + `st::Lens` + `st::OpticsSystem` — flat apertures a beam reflects off (`d' = d - 2(d·n)n`) or bends through. `Lens::Type` covers Spherical / Cylindrical / Toric / Aspheric / Axicon / Prism / Window, all one paraxial ray transfer with a different deviation term. `Mirror::dichroic` splits the beam in two (reflect one band, transmit the rest), which is why `Trace()` walks a stack of branches rather than a single chain. `Trace()` is the sequential walk that turns one ray into a list of legs; it lives on the CPU because leg N+1 depends on where leg N landed, which a per-pixel pass cannot discover. Draws nothing, and carries its own “is a beam reaching me” diagnostics because a silent element is otherwise undebuggable. |
| `render/OpticsComponents.cpp` | The `"sticMirror"` / `"sticLens"` native components. |
| `scene/Ray.*` | `st::Raycast` / `st::RayHit` / `st::RayQuery` — one raycast over either backend: `Mesh` (`Scene::Intersects`, hits anything drawn, no body needed), `Physics` (`wi::physics::Intersects`, Jolt bodies only), `Both` (nearer wins) or `None`. Also `st::LocalAxes` — the ONE forward-axis table, shared by the projector, laser, ray and optics. |
| `scene/DayNight.*` | `st::DayNight` - the daylight / time-of-day system. One clock drives one directional light plus the scene's weather: the sun's arc, its colour and intensity, the ambient level, the stars and the sky exposure. The sun is placed ASTRONOMICALLY by default (NOAA solar position from latitude, longitude, time zone and day of year), so it rises in the east and the day is the right length for the place and season; `geographic` off falls back to the simple overhead hoop a hand-rolled cycle usually is. `AppConfig::dayNight` (Off / Adopt / Create) decides what a scene load binds it to. Reach it anywhere via `st::DayNight::Get()`, panel in `Simtary > Day / Night & Weather`. It also owns the WEATHER: `WeatherState` is the continuous half (cloudiness, cloud darkness, fog density/start/band, wind speed/heading/turbulence, rain, exposure and ambient scaling) and seven named `WeatherPreset`s expand into it. `SetWeather(preset)` eases from what is on screen into the new state over `transitionSeconds`; `Weather()` is what the scene is showing this frame and is what gameplay should read. `autoWeather` re-rolls a preset every `autoIntervalHours` of GAME time from a seeded LCG, so the same seed gives the same weather everywhere. |
| `scene/PlayControl.*` | `st::PlayControl` - the scene transport. `st::App::Update` hands it the real frame delta and passes what comes back to the scene and the daylight system, so ONE place decides how much time the world sees: `Playing` (realtime x `timeScale`), `PlayingNoPhysics` (animation and logic run, `wi::physics::SetSimulationEnabled(false)`), `Paused` (dt = 0, physics off, `Step()` lets single frames through at `stepDelta`). `Reset()` is a real `SceneManager::Reload()` - Unload + Load - because a scene made of native components holding their own state cannot be rewound. The transport row is drawn on the EDITOR TOOLBAR, beside the gizmo buttons - `GUI(compact)` is the SmallButton form that fits a menu-bar row. It is not on the DevUI menu bar: that is the application's menus, and five buttons plus a slider across it pushed Help and the fps readout off the end. `Simtary > Play/Pause/...` and F5/F6/F7/F8 reach the same transport with no editor open. |
| `scene/DayNightComponent.cpp` | The `"sticDayNight"` NATIVE COMPONENT - the same system attached from the editor, so place, date, clock speed, look and weather are scene data that saves with the map and needs no game code. `NCA_*_weatherPreset` names the sky, so a map opens in the weather it was designed for. |
| `scene/RayComponent.*` | The `"sticRay"` native component: a raycast bolted to an entity, re-cast every frame. The shared seam for a laser sight, a rangefinder, an interaction prompt and an “am I aiming at it” HUD. |
| `render/Framebuffer.*` | `st::gfx::Framebuffer` — an off-screen surface you draw into and hand to a material, a light mask or a projector. CPU mode wraps libgfx (`GFXcanvas`) and owns the staging texture, row pitch and flip; GPU mode is a render target you draw into with `wi::image`/`wi::font` between `Begin()`/`End()`. |
| `render/GraphicsAPI.*` | `st::GraphicsAPI` (Auto / DirectX12 / Vulkan) - which backend the process runs on, resolved once at startup from the command line, `options.stad`, `AppConfig::graphicsAPI` and the platform default, with a fallback when the chosen runtime is not installed. `CreateGraphicsDevice()` builds the device and points the shader path at the matching cache folder; `ActiveGraphicsAPI()` is what actually came up. |
| `display/DisplaySettings.*` | Player-facing video options: window mode, monitor, resolution, refresh rate, v-sync, frame cap, render scale, and the graphics backend for the next launch. NOT DevUI — `st::App::Display().GUI(app)` drops into a game's own options menu, and DevUI renders the same panel in its Display tab. Sole owner of v-sync and the frame cap; `GraphicsSettings` deliberately no longer carries them. |
| `audio/faust/` | `FaustManager` (OpenAL DSP host) + `FaustProcessor<T>`. Starts with no processors — games register their own AOT instruments. |
| `SubWinStatus.*` | The native (Win32/X11) loading window, on its own thread, because a blocking `Scene::Load()` means no ImGui frame can be drawn. Two text lines - a phase and a dimmed, middle-elided detail line - plus a progress bar. Every setter is thread-safe; the detail line is written from loading workers. |
| `anim/`, `eventBus.*`, `ZmqHandler.*` | Animation descriptors (`.staod`, NBT — read through `wi::helper::FileRead`, so they resolve out of a mounted asset package), main-thread event bus, ZMQ bridge, native (Win32/X11) loading window. |

### Project hook surface (`st::App`)

Content: `RegisterScenes`, `OnKeyRegister`, `OnInitialize`, `OnExit`.
Frame: `OnUpdate(dt)`, `OnFixedUpdate()`.
UI: `RenderUI()` (game UI, always), `RenderDevUI()` + `OnDevUIMenu()` (dev only),
`RenderLoadingScreen(state)`.
Render: `OnRenderPathSetup(path)`, `OnRender()`, `OnPreCompose(cmd)`, `OnCompose(cmd)`.
Input: `OnEvent(SDL_Event) -> bool` (true = consumed).
Scenes: `OnSceneLoaded(name)`, `OnSceneUnloaded(name)`.
Instance methods: `Audio()`, `Assets()`, `RequestQuit()`,
`IsDevUIVisible()/SetDevUIVisible()/ToggleDevUI()`, `Loading()`,
`SetLoadingStatus(text, percent)`.
Also `HandleDroppedFile(path)` — `st::Run` calls it for `SDL_DROPFILE` after `OnEvent`
declines, and the default hands it to the DevUI Asset Explorer.

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

- `simtary_add_app(NAME ... ORGANIZATION ... ICON ... [SOURCE_DIR] [ASSETS_DIR] [CONTENT_SUBDIR] [SCENE_SUBDIR] [RESOURCE_SUBDIR] [EXTRA_SOURCES|INCLUDES|LIBS] [NO_SHADER_WARM] [NO_CRASH_REPORTER] [PACK_ASSETS] [PACK_ONLY] [PACK_NAME] [PACK_PART_SIZE] [PACK_LEVEL] [PACK_ON_CONFLICT] [MODULE] [MODULE_NAME])`
- `simtary_pack_assets(TARGET ... CONTENT_DIR ... [SCENE_SRC_DIR] [RESOURCE_DIR ...] [ON_CONFLICT] [NAME] [PART_SIZE] [LEVEL] [PACK_SUBDIR] [SCENE_SUBDIR])`
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

**A project's `assets/` has two content folders, and the split is what keeps the rules
simple.** `assets/contents/` is "everything in here becomes a packed resource" — no
exception list, no per-extension carve-out. `assets/scenes/` holds the `.wiscene`
SOURCES, which are not resources at all: they are what a `.stsd` is converted FROM, and
a map left inside `contents/` would be packed AND copied loose, ~37 MB of duplicate each.
`simtary_add_app(SCENE_SUBDIR ...)` names the folder, `stpack pack --scene-src` is what
reads it, and its maps' textures still land in the same package. Maps left inside
`contents/` are still converted, so an older layout keeps working.

**A `.stsd` among the sources is passed through, never converted.** It already IS the
converted form — the resource block a `.wiscene` carries was lifted out when it was
made — so the packer copies it to `<exe>/assets/scenes/` byte for byte and reads only
its reference list, to report which of the resources it names the package will not
hold. This is what makes "drop a new map in and rebuild" a swap of one map rather than
a build that silently ships a map with no textures.

**`assets/resources/` is where a swapped map's resources come from.** A `.stsd` is
metadata plus an entity blob; the textures and meshes it references live in the package
its `.wiscene` was converted alongside, so a map brought in from elsewhere references
things this project has never seen. Files under `assets/resources/` are merged into the
package under their relative names — `assets/resources/textures/wall.dds` becomes
`textures/wall.dds`, exactly the name the map asks for. `resource/` (singular) is
accepted as the folder name too. `stpack pack --resource-dir` is the flag;
`--on-conflict` decides who wins when such a file collides with a resource a map already
embedded:

| policy | what happens |
| --- | --- |
| `override` (default) | the loose file is added FIRST, so the map's embedded copy is skipped by the existing dedup and every reference to that path — in every map — resolves to the new bytes |
| `add` | both are kept; the loose file is packed as `<name>.<n>.<ext>`. Existing objects keep pointing at the embedded copy, because the names inside a map's entity blob cannot be rewritten from outside the engine |
| `keep` | the loose file is ignored |

`simtary_add_app(PACK_ON_CONFLICT add)` sets it per project. `stpack pack --strict`
turns a passed-through map's missing resources from a warning into a failed build.

**Swapping in a map the editor saved.** A `.stsd` references resources by name and holds
none of them — anything the mounted packages already held stays in those packages. An
editor save now writes the map's own resources into `assets/resources/` as it goes (see
"A native save ALSO writes the map's raw resources" below), so a map saved by THIS editor
carries its dressing into the next build on its own. A map that arrived from somewhere
else does not: copying only that map into `assets/scenes/` and deleting the `.wiscene`
leaves the pack with nothing to resolve against, and the map loads white. The recipe:

```
stpack resources assets/scenes/s1map.wiscene --out assets/resources
```

which writes the resource block straight out of the map's own source, under the names it
uses — the layout `--resource-dir` reads. Then the `.wiscene` is free to go. If the
source has already left, the same command takes the `.stsd` plus the package it was
packed alongside instead:

```
stpack resources assets/scenes/s1map.stsd \
    --pack <exe>/assets/resources/content.strd --out assets/resources
```

Since the packer MIRRORS as it converts (below), a project that has built once already
has these files and the recipe is only for a map that arrived from elsewhere.

**Every build mirrors a converted map's resources into `assets/resources/`, and so does
every editor save.** `simtary_pack_assets(MIRROR_DIR ...)` - wired to the same folder as
`RESOURCE_DIR` by `simtary_add_app`, which now CREATES `assets/resources/` rather than
merely detecting it. The editor writes into the same folder when it saves a `.stsd` (see
"A native save ALSO writes the map's raw resources" below), which is what covers the
resources the packer never sees because they came from an in-editor model import.
A resource of a `.wiscene` exists in exactly one place, inside that file, so a project
that never copies it out is one `move` away from a package that cannot dress its own map.
Mirroring costs a stat() per resource on a normal build - the descriptor lists the names,
a file already present is never rewritten (it may be a deliberate override), and the
`.wiscene` is re-read only when something is genuinely missing. The first build that
mirrors a new map writes under a `CONFIGURE_DEPENDS` glob, so CMake re-configures and
packs once more; the second pass writes nothing and produces the same package.

Moving the `.wiscene` out of the project without either of those is what empties the map. The
packer builds the package from what it can see; those resources were only ever inside
that one file, and nothing else in the project holds a copy. The build says so —
`stpack: ... needs 14 resources the package does not have` — and `--strict` turns that
warning into a failed build rather than a shipped map with no textures. Keeping both the `.wiscene`
and the `.stsd` in `assets/scenes/` also works: the map is converted for its resources
and the hand-placed `.stsd` then passes through over the converted one, so the editor's
map is what ships.

**A pack sweeps the part files an older, larger pack left behind.** `AssetPackWriter::
Finish` deletes `<name>.stafp<N>` above the part count it just wrote. Without it a build
whose content shrank left megabytes of orphan next to a smaller index - no reader would
ever open them, but an output folder that still looks full is exactly how a package that
has LOST assets goes unnoticed.

**Assets sync on a custom target, not on POST_BUILD.** `<APP>_Assets` copies
`assets/` to `<build>/assets` and `assets/contents/` to `<exe>/assets`, and the app
target depends on it. It has to be a custom target: a POST_BUILD command only runs
when the executable is actually relinked, so editing nothing but a scene or a texture
left the stale copy in the output and the build reported success. Custom targets are
always out of date, so `cmake --build` re-syncs content either way.

`assets/scenes/` is copied to `<exe>/assets/scenes/` only when the project does NOT
pack — with `PACK_ASSETS` the packer writes the converted `.stsd` there instead, and the
`.wiscene` never ships.

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

**Input is registered in code, evaluated per entity, and disambiguated rather than
summed.** `st::App::OnKeyRegister(st::input::Registry&)` runs once at Initialize,
before `RegisterScenes`, so a scene's `stInput` components find their map already
built. Three things about the model are deliberate:

- **Maps are switched, not merged.** "Player" and "UI" can both bind A/Enter without
  fighting, because one component reads one map at a time (`SwitchMap`).
- **A Vector2 action picks ONE candidate, it does not sum its bindings.** Each binding
  group proposes a value and the largest magnitude wins - Unity calls this control
  disambiguation. Without it, a stick half over plus W held reads as 1.5. A composite
  (the four keys of a WASD 2D vector) counts as one group and is normalized, so
  walking diagonally on the keyboard is not 1.41x faster than walking forwards.
- **Sampling happens in `Compute()`, not `Update()`.** `Compute` is a barriered stage:
  every component's `Compute` finishes before any component's `Update` starts, so a
  sibling reading `stInput` in its `Update` always sees a fully evaluated frame.
  Sampling in `Update` would race whichever component the job system ran first.

The frame's pointer delta, wheel and touch list are latched ONCE on the main thread by
`InputSystem::Update` (`st::input::BeginFrame`) because SDL's relative-mouse delta is a
single-consumer read - a binding that called `SDL_GetRelativeMouseState` itself would
both race and steal the delta from every other reader. Device gating crosses the same
seam as `st::input::Gate()`: the action model knows nothing about ImGui and just asks
the table per binding.

`LoadDefaults()` seeds a default "Player" and "UI" map before the hook runs, so a
project that registers nothing still has something to attach. `Map()`/`Action()` return
the EXISTING entry when the name is taken, so re-registering over a default appends
bindings - call `input.Clear()` first to start from empty.

**A map loaded AS THE SCENE goes in flat; a model IMPORTED into one keeps its root.**
`wi::scene::LoadModel(..., attached = true)` parents everything it reads to one fresh
entity, which is right for the editor's import path (it is the thing you then select,
move or undo) and wrong for a scene's own map - the Hierarchy showed the whole world as
a single nameless "Entity 3" row that had to be expanded before anything was visible.
`st::LoadSceneFileFlat` loads with `attached = false` instead, so the map's own top-level
nodes ARE the hierarchy's top-level rows. The cost is that there is no single handle to
delete on the way out, which is why it returns EVERY entity the file created rather than
just the roots: a map also brings in mesh, material and animation-data entities that have
no transform and therefore no parent.

**A map in the scene folder is a scene, unless C++ already speaks for it.**
`SceneManager::DiscoverScenes()` runs after `RegisterScenes()` and registers one
`st::FileScene` per `.stsd` / `.wiscene` it finds, named after the file, so dropping a map
into `assets/scenes/` makes it loadable with no code. Two things stop it: a scene of that
name already exists (case-insensitively), so the C++ class overrides the file; or a
registered scene declares it through `Scene::SceneFiles()`, so the map a C++ scene already
loads is not offered a second time under its own name. Declaring is by STEM, which is why
`"assets/scenes/s1map"` covers both the packed `.stsd` and the source `.wiscene`.
`AppConfig::sceneFolder` / `sceneAutoDiscover` control it; the Scene Manager window can
rescan on demand.

**The daylight system runs BEFORE the scene update, and that ordering is the feature.**
`Scene::Update` is what runs the engine's light update system, and that is where a
directional light's `direction` is recomputed from its world matrix and copied into
`WeatherComponent::sunDirection`. So `st::DayNight::Update` aims the sun in
`st::App::Update` ahead of `sceneManager.Update`; aiming it afterwards leaves the sky
lit by the previous frame's sun, which is invisible at noon and obvious at dawn. It
writes the light's TRANSFORM, not `LightComponent::direction`, for the same reason -
the direction field is overwritten from the transform on the next scene update.

A directional light shines along its entity's local **+Y** (`RunLightUpdateSystem` in
`wiScene.cpp`), so the rotation that puts the sun at an elevation and an azimuth is
`RollPitchYaw(90 deg - elevation, azimuth, 0)`. World axes are the engine's: +Y up, and
the system treats +Z as north, with `northYaw` to turn the compass for a map that was
built facing some other way.

**`AppConfig::dayNight` defaults to Off on purpose.** Adopt would take over the first
directional light of every scene that loads - including a map whose sun was aimed by
hand in the editor, which would be silently re-aimed to whatever the clock said. A game
opts in (Milistry does), a scene can call `Create()`/`Adopt()` itself, or a
`"sticDayNight"` component in the map can bind the system with the map's own settings.

**Weather is a blend between two states, and the flags are deliberately outside it.**
`WeatherState` holds only floats, because a weather change is a lerp: `SetWeather()`
records where the sky is NOW as the start, so changing your mind half way through a
transition carries on from what is on screen instead of snapping back. The switches
that cannot be half-applied - volumetric clouds on, cloud shadows, height fog - live in
`Settings` next to it. Two mappings in `ApplyWeather` are worth knowing before touching
them: cloud cover needs BOTH `coverageAmount` and `coverageMinimum` (amount alone never
closes the sky, there are always holes where the weather-map noise is low, so only the
top of the cloudiness range lifts the floor), and `WeatherComponent::windDirection` is a
vector whose LENGTH is part of the force - emitters use `direction * speed` and springs
use its magnitude - so the heading becomes a unit vector scaled by `speed / 20`.

**One pad, one player slot - `wi::input::GamepadBackend` decides which backend gets
it.** On Windows every backend is compiled in and they overlap. XInput and SDL overlap
is resolved by matching user indices (`sdlinput::GetControllerXInputUserIndex`), but
RawInput has no handle to match on - it is a generic HID parser that cannot know which
SDL joystick is the same physical device - so a plain HID pad registers TWICE, and
RawInput takes the lower slot because its registration loop runs first. Player 0 then
reads an unmapped HID parse with its own axis signs and deadzones while SDL's mapped
copy sits at player 1. `GamepadBackend::Auto` (the default, `AppConfig::gamepadBackend`)
allows RawInput only while XInput and SDL have found nothing, which is the case RawInput
is actually good for. `All` restores the original behaviour; `SdlOnly` / `RawInputOnly`
force one. Changing it clears the slot list so it rebuilds in backend order the same
frame - marking the dead slot disconnected would leave the good pad stranded at player 1,
which is the bug. The combo is in `Simtary > Gamepad Analog`, saved under `input`.

**The right stick is DOWN-positive on purpose, and that is a trap worth knowing.**
Every backend stores `thumbstick_L` up-positive and `thumbstick_R` down-positive, so the
same physical push reads with opposite signs on the two sticks. The reason is camera
code: mouse delta Y is down-positive, so a look routine can add stick and mouse into one
pitch with one sign (`PlayerMovement.cpp` does exactly that). Anything that treats the
right stick as a DIRECTION instead gets it backwards - which is what
`GAMEPAD_ANALOG_THUMBSTICK_R_AS_BUTTON_UP` did, being a copy of the left stick's test:
"Right Stick Up" fired when the stick was pushed down. `AnalogSettings::
right_stick_up_positive` (off by default, `AppConfig::rightStickUpPositive`, saved under
`input`) flips it at the `GetAnalog` boundary for every backend at once; turn it on and
look code that mixes stick with mouse needs its pitch sign flipped. The Gamepad Analog
gates always draw thumb-side up whatever the stored sign is, and say which sign that is.

**Pause is dt = 0, not a skipped update, and the DevUI keeps the real delta.**
`PlayControl::Apply()` returns the scene's delta; `st::App::Update` gives that to
`DayNight::Update` and `SceneManager::Update` and nothing else. Input, the standby cap,
the loading screen and every DevUI window stay on the real frame time, so a paused world
is one you can still look around, click on and drive the editor camera through - a scene
that moves a camera from its own `Update()` should use
`PlayControl::Get().RealDelta()` for it (Scene1's fly camera does). Stepping runs the
same update path a played frame does, at a fixed `stepDelta`, because a step that took
the real frame time would advance the world by however long the paused frame happened to
take. The physics switch is global (`wi::physics::SetSimulationEnabled`), so PlayControl
writes it only when its own answer changes and a game that turns physics off for its own
reasons keeps that until the transport actually moves.

**Scene update runs exactly once per frame.** `st::App::Initialize` calls
`renderPath.setSceneUpdateEnabled(false)`; scenes call `scene.Update(dt)` themselves
from `SceneManager::Update`. A second update per frame swaps `MeshComponent`'s
`so_pos`/`so_pre` streamout views twice, which cancels out — skinned meshes then get
garbage velocity (broken motion blur, TAA, FSR2).

**A vehicle is a wheel ARRAY, and `rigidbodies` is serialization version 7.**
`RigidBodyPhysicsComponent::Vehicle` used to be four wheels hardcoded inside
`wiPhysics_Jolt.cpp`; it is now `wi::vector<Wheel>` plus `differentials` and
`anti_roll_bars` vectors that index into it, so a 6x6 is data. The `chassis_*` /
`wheel_*` / `*_suspension` fields survive as the LAYOUT INPUT `Vehicle::GenerateWheels()`
lays an axle set out from — plus `chassis_half_height`, which still sets how far the
centre of mass is dropped and therefore still matters to a hand-placed vehicle. A scene
written before version 7 carries no array, so `Serialize` calls
`Vehicle::MigrateLegacyWheels()` on load and rebuilds the two-axle layout the old fields
described; the legacy `wheel_entity_*` slots and `car.four_wheel_drive` are kept for
exactly that and are read nowhere else. Car and motorcycle now share one builder — a
motorcycle is the same code with `MotorcycleControllerSettings`, and it is the one shape
that is still fixed at two wheels, because Jolt's controller indexes wheel 0 and 1
directly to balance the bike.

Steering has no count limit either, and never did — `WheeledVehicleController::PreCollide`
walks every wheel and sets `-mRightInput * mMaxSteerAngle`, so a wheel steers if and only
if its own `max_steer_angle` is non-zero. A **negative** angle is legal (Jolt asserts on
`abs(angle) > 90°`, not on the sign) and steers that wheel against the input, which is
what rear-axle counter-steering is. `Vehicle::SteeringLayout` only decides what a freshly
generated vehicle starts with; the per-wheel angles stay editable afterwards.

**Collision geometry does not have to be what is drawn, and the loader is a HOOK.**
`RigidBodyPhysicsComponent::mesh_source` picks between the mesh on the entity, a
`ProxyEntity` holding a low-poly mesh, and an `ExternalFile` collision model. Reading a
model file is `Framework/io/model`'s job and the engine sits below it, so the engine
publishes `wi::physics::SetCollisionMeshLoader()` and `st::App::Initialize` fills it in
(`LoadCollisionModel` in `stApp.cpp`, dispatching `.stsd` / `.wiscene` / the interchange
formats). With no loader installed an `ExternalFile` body logs and is skipped.

The loader is **main thread only** and that is not incidental: importing builds render
data and registers resources, neither of which survives being called from a physics job.
`RunPhysicsUpdateSystem` therefore walks the rigid bodies and primes the model cache
BEFORE it dispatches any body-creation job, and `AddRigidBody` only ever reads the cache
— a miss re-requests a refresh and lets the next frame build the body. Loaded geometry is
held by `shared_ptr` so its address is stable, which is what lets the existing shape cache
keep keying on the vertex/index pointer.

**`AddRigidBody` must never assume it got a shape.** Every shape path can legitimately
fail — a mesh shape on an entity with no mesh, a collision model that would not load, an
entity scaled to zero on one axis, a mesh whose subset offsets outrun its index buffer.
The vehicle and character branches then dereferenced `physicsobject.shape` unconditionally
and the whole engine went down with a read from address 0 on a job thread, which
symbolizes as a stack with no obvious cause. A failed shape is now a logged skip; a
vehicle additionally falls back to a plain box built from its own `chassis_half_*`, on the
grounds that a car driving on a box is easier to diagnose than one that never appears. A
failed shape is also never written into the shape cache — caching the null made every
later body sharing that geometry fail too, permanently.

**ImGui integration is hand-rolled** on the engine's graphics device.
`ImGui::Render()` runs in `Update()` (not `Compose()`) so draw data is ready before
any GPU pass; `Compose()` submits it with `device->AllocateGPU()` transient buffers.
Its shaders live in `assets/shaders/ImGui{VS,PS}.hlsl` and are compiled by
`simtary_add_app`.

**Lens flare shaders are named `StLensFlare*`, not `LensFlare*`** — the engine ships
its own `lensFlareVS/PS.hlsl` and both land in the same output folder, which is
case-insensitive on Windows.

**A framework shader that includes `globals.hlsli` needs `ENGINE_ENV`.**
`simtary_compile_shader(... ENGINE_ENV ...)` adds the engine's shader include paths.
The rest of that environment - the DX12 default root signature, the Vulkan binding
shifts, the descriptor set numbers - comes from `wi::shadercompiler` itself, per format,
built out of `GraphicsDevice_Vulkan`'s own constants; CMake used to carry a hand-written
copy of them that could drift. Runtime compilation is not a fallback here:
`dxcompiler.dll` is not shipped next to the exe, so `wi::renderer::LoadShader` can only
ever load a `.cso` that the build already produced. `StProjectorCS.hlsl` and
`StLaserCS.hlsl` are the shaders in this category.

**Framework shaders are compiled by `stshaderc`, not by `dxc.exe`, and on Windows they
are compiled TWICE.** Twice because a Windows build can start on either backend, so
every framework shader has to exist as DXIL in `shaders/hlsl6/` and as SPIR-V in
`shaders/spirv/` - a Vulkan run with only the DX12 set comes up with no ImGui, which is
no UI at all. And by `stshaderc` (`tools/stshaderc.cpp`, a CLI over `wi::shadercompiler`)
because dxc.exe cannot be relied on for the SPIR-V half: the copy that ships with the
Windows 10/11 SDK is DXIL only ("SPIR-V CodeGen not available"), so the build needed a
Vulkan SDK installed to produce a complete game. The `dxcompiler` vendored in `Engine/`
has both, and is what the engine's own shaders already go through. It links the engine,
so its runtime DLLs are staged beside it and its build steps run from its own directory
- on Linux `wi::shadercompiler` loads `./libdxcompiler.so`, which is relative to the
working directory rather than to the executable.

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

**The lens flare is occluded on the GPU, in the VERTEX shader, and both halves of that
matter.** A flare is light that reached the LENS, so it must not survive the object
standing in front of the sun - drawn additively over the composed frame with no test, it
painted a sun straight through a vehicle. The test is nine `Load()` taps of
`RenderPath3D::depthBuffer_Copy` in a small aspect-corrected disc at the sun's screen
position: depth is reverse-Z, so the far plane and therefore the sky read exactly 0 and
anything greater is geometry in the way, which is why this needs no camera matrices and no
linearisation. Nine taps rather than one because a single tap pops the entire flare on and
off as an edge crosses it.

It happens in the vertex shader and arrives at the pixel shader as a `nointerpolation`
interpolant, so the taps cost three invocations a frame instead of one set per pixel of a
fullscreen triangle. The depth copy rests in `SHADER_RESOURCE_COMPUTE` - the post-process
chain reads it from compute - so `Draw()` brackets itself with a barrier to
`SHADER_RESOURCE` and back; a graphics-stage read of it is a different state on DX12.
`settings.depthOcclusion` off restores the old draw-over-everything behaviour, and with no
depth texture passed the shader never touches `t0` at all.

**A mirror has to WIN the tie against its own mesh, and the test is OWNERSHIP, not
distance.** `st::Mirror` is a bare plane; what makes it visible is a mesh in the same
place, on the same entity. Comparing their two distances compares ray-vs-triangle
against ray-vs-plane, which disagree by however far the mesh surface sits from its own
origin — a hair for a flat plane, centimetres for a mirror in a frame — and the sign of
that difference decides whether the beam bounces or stops dead. So `Trace()` asks
instead: is the geometry hit on this element's OWN `followEntity`? If so, and the ray
also crosses the aperture, the element wins outright at any distance. `opticBias`
(2 cm) is only the fallback for an element whose glass belongs to a different entity.
The aperture still bounds it — a beam landing on the mesh OUTSIDE the aperture
correctly stops there, and `OpticSurface`'s diagnostics (`hitsThisFrame`,
`lastMissMargin`) say which of the two happened.

**A mirror with THICKNESS reflects inside itself unless two things are handled.** The
symptom is unmistakable and misleading: the beam hits, a spot appears on the glass, no
beam comes off it, and flattening the mesh to zero scale on the normal axis makes it
work. Both causes are the same mistake in different places - the reflective surface is
the mesh's FACE, not its middle:

1. `fitToMesh` puts the plane on the front face (`centre + normal * halfThickness`),
   not the mesh centre. At the centre the outgoing beam is born inside the solid.
2. The leg leaving an element sets `query.ignoreEntity` to that element's own
   `followEntity`, so it cannot stop on the glass it just bounced off. Only the leg
   immediately after the bounce is exempt, so the beam can still stop on that mesh
   later in its path.

Point the normal axis OUT of the reflective face. A double-sided mirror with real
thickness is ambiguous by nature; (2) is what keeps the back side usable.

**Optical elements are never latched off by a bad frame.** `OpticSurface::resolved` is
separate from `enabled` for that reason: a frame in which `followEntity` has no
transform (mid-load, mid-reload) skips the element for that frame only. Folding it into
`enabled` made a mirror that had one such frame dead forever, and indistinguishable in
the inspector from one that simply refuses to work.

**A dichroic mirror is why the tracer is a stack, not a loop.** `Mirror::dichroic`
reflects `tint * reflectance` and passes `transmitTint * transmittance` on as a SECOND
beam, so one ray in becomes two out - which a linear walk cannot express.
`OpticsSystem::Trace` keeps a `pending` stack of branches and runs them depth first,
deliberately: the reflected chain finishes before the transmitted half starts, so
`terminals[0]` is the primary beam and `Path(id)->hit` still hands gameplay the beam
the player is aiming. `BeamTraceDesc::maxSplits` caps the branching and `MAX_LEGS`
(64) is the backstop, because splits compound - two dichroics facing each other are an
exponential, not a loop. A 50/50 beam splitter is the same feature with both tints
white and both coefficients at 0.5.

**Array projection multiplies the trace, not the draw.** `LaserArray` turns one laser
into a Grid / Ring / Fan / Cross / Spiral of rays, each a full trace through the same
optics, so a pattern pointed at a mirror reflects as a pattern. `spreadAngle` and
`offset` are different instruments and both exist on purpose: angle is a dot projector
(the pattern grows with distance), offset is a lenslet array (it does not). Trails are
per RAY, so `trailMax` is per ray too and a 5x5 grid asks for 25x the dot budget. The
GPU side is affordable only because both shader loops reject on a squared distance
before reaching any `atan` - without that early-out 128 segments is two transcendentals
per segment per pixel.

**A projector reflects by standing a VIRTUAL projector behind the glass.** A projector
is a cone, not a ray, so it cannot use the beam tracer. `Projector::opticBounces`
instead uploads one extra `StProjector` per element: for a mirror, the apex and both
orientation vectors reflected through the plane (the handedness flips, which is correct
- a projected image in a mirror IS mirror-writing); for a lens, the apex moved to where
the thin lens images it, which zooms and shifts the picture the way a real zoom lens
does. Each virtual projector is clipped to its element's aperture in the shader
(`projector_aperture`), without which the cone would light the whole room from inside a
wall. It is ONE level deep by design - a virtual projector does not itself reflect - so
two facing mirrors give two images, not an infinite corridor. Every element the image
reaches costs another `ST_PROJECTOR_MAX` slot and another shadow map, which is why the
default is 0.

**Optics update BEFORE anything that reflects through them.** `st::App::Update` runs
`optics_` then `projectors_` then `lasers_`. Reversing it lags every reflection by a
frame, which reads as the reflection sliding around on its own.

**Every lens type is one ray transfer with a different deviation term.** `Lens::Type`
is Spherical / Cylindrical / Toric / Aspheric / Axicon / Prism / Window, and all of
them are `slope' = slope - f(u, v)` in the aperture's own axes. That is the whole model,
so a Fresnel lens is Spherical (only its appearance differs) and plano-convex vs
biconvex is just a different focal length. What it structurally cannot do: chromatic
dispersion (a leg carries one colour), thick-lens and TIR (elements are infinitely
thin), and anything that turns one beam into many — lenslet arrays, gratings, a beam
splitter's transmitted half. The tracer carries a single ray per leg by construction.

**Forward-axis numbering is append-only.** `0 +Z, 1 -Z, 2 -Y, 3 +Y, 4 +X, 5 -X`, in
`st::LocalAxes` (`Framework/scene/Ray.h`). X was appended rather than inserted where it
would read better because scene metadata already stores these as plain integers, so
renumbering 0-3 would silently re-aim every projector, laser and mirror already placed.
`Projector::Forward` is defined against the same table rather than carrying a second
copy of it.

**The asset package is three formats, and the split between them is the whole point.**
`.strd` is the INDEX, `.stafp<N>` are the PAYLOAD parts, `.stsd` is one MAP. They are
separate files because they answer different questions at different times.

- `.strd` is deliberately **not** NBT. NBT is a linear self-describing tree: finding
  one asset means walking every tag before it and allocating a node for each, which is
  fine for a 40-key options file and wrong for a 100,000-entry index consulted during a
  load screen. The index is a flat, fixed-stride, memory-mapped table behind a
  power-of-two open-addressed bucket array, so a lookup is `id & (bucketCount-1)`, a
  linear probe, and one 80-byte struct read - no parse, no allocation. The mapped bytes
  ARE the data structure.
- `.stsd` keeps NBT for the metadata a person or a tool reads (name, source, the asset
  ID list) and keeps the megabytes OUT of it: the entity payload is an out-of-band,
  page-aligned blob, so opening a map to read its name does not parse a tag tree the
  size of the map.
- Asset IDs are XXH64 of the lower-cased path; the name heap stores the ORIGINAL case.
  Both are needed: the engine asks materials for the exact string they saved (case
  sensitive on Linux), while a lookup must not care whether someone typed
  `Textures/Wall.DDS`. See `StHash.h` - `NormalizePath` vs `CanonicalPath`.

**Converting a `.wiscene` is a header patch, not a re-encode, and that is why it is
reversible.** From archive version 90 on, `wi::scene::Scene::Serialize` writes
`jump_before`/`jump_after` right after its `reserved` word: absolute offsets bracketing
the embedded resource block. So the split copies bytes `[0, jumpBefore)` verbatim,
appends a `uint64_t 0` resource count, and rewrites `jumpAfter` - the entity bytes are
never interpreted, so nothing can be lost in a component format this build does not
understand. Merging reverses it. Both of Milistry's maps round-trip **byte-identically**
(`asset_pack_test` asserts this, and `stpack unpack --rebuild-scenes` proves it on the
real 37/39 MB files). Transcribing ~40 versioned component managers into NBT instead
would have to be re-done on every engine merge and would make the conversion lossy;
moving bytes is exact by construction.

**Codec choice is per asset and is driven by the EXTENSION, not just the type.** A
`.dds`/`.png`/`.ogg` is already entropy-coded: zstd buys 1-3%, costs a decompress on
every read, and - the real cost - gives up the zero-copy mapped read that mip and audio
streaming depend on. So those stay `Codec::None`. Raw-sample files that land on the same
*types* (`.bmp`, `.tga`, `.hdr`, `.wav`) do compress, which is why
`DefaultCodecFor(path, type, size)` exists alongside the type-only overload. Everything
large and compressible gets `ZstdChunked`: fixed 256 KB frames plus an offset table, so
a ranged read decodes only the frames it overlaps and stays seekable. Whole-frame
`Zstd` is the one codec that is NOT streamable, and the writer clears
`AssetFlag_Streamable` when it picks it.

**A pack is mounted by installing ONE override, not by teaching the engine about it.**
`wi::helper::SetAssetSourceOverride` already sits under every `FileRead`/`FileExists`
the engine performs, so `st::AssetSystem::Install()` redirects the resource manager,
streaming, video and scripts at once, and any path the packs do not hold falls through
to the real filesystem - which is what keeps shader caches, save data and loose
development assets working beside a packed game. Mount before `wi::initializer`;
`st::Run` does it from `AppConfig::assetPacks`. Later mounts shadow earlier ones, which
is how a patch pack works.

**`FileExists` was hooked and `FileTimestamp` was not, and that combination was fatal.**
`FileTimestamp` asked the (overridden) `FileExists`, was told yes for a packaged path,
then called `std::filesystem::last_write_time` on a path with no file behind it. With
`/EHsc-` the resulting `filesystem_error` is a `__fastfail`, so the process died with
`STATUS_STACK_BUFFER_OVERRUN` and nothing in the log. `AssetSourceOverride` now carries
an optional `file_stat` that answers both `FileSize` and `FileTimestamp`, and the
filesystem fallback uses the `error_code` overload. `AssetSystem` reports timestamp **0**
on purpose: a mounted package is immutable, and `wi::resourcemanager` reloads whenever
the timestamp it sees is newer than the one it cached, so a value that never rises is
exactly "this can never go stale".

**Every part is mapped at `Open()`, not on first touch.** The read path is `const` and
lock-free because `wi::helper`'s override is called from every loading job at once, and
a lazy map needs a lock there. Mapping is address space, not memory: a 40 GB package
costs 40 GB of a 128 TB address space and no RAM, with pages arriving from the page
cache on touch.

**The packer is a build step, so it links no engine.** `tools/stpack` builds from
`Framework/io/asset/*` + `Framework/io/Nbt.cpp` + its own copy of zstd
(`tools/stpack_zstd.cpp`, the same `extern "C" { #include zstd.c }` trick as
`Engine/Utility/utility_common.cpp`). It is a normal target rather than a configure-time
bootstrap like the `.stpd` reader, because it is needed while a game BUILDS, not while
one configures - and it is defined outside `SIMTARY_BUILD_TESTS` because a game adds
this workspace `EXCLUDE_FROM_ALL` and pulls the tool in through its pack target's
`add_dependencies`.

**Packing is incremental; asset copying is not.** `<APP>_Pack` is a real
`add_custom_command` with `DEPENDS` on a `CONFIGURE_DEPENDS` glob of the content tree,
unlike `<APP>_Assets`, which is deliberately always out of date. It has to be:
re-running zstd over 76 MB of maps on every build, changed or not, is tens of seconds
each time. `<APP>_Repack` forces it; `<APP>_AssetsResync` drops the stamp so the next
ordinary build regenerates the package it just wiped.

**Packing and copying loose are ALTERNATIVES, not companions.** With `PACK_ASSETS` the
content directory is NOT copied to `<exe>/assets` at all — the package is the shipped
form. `stpack` takes every regular file under it (the only thing it skips is `*.wiscene`,
which it has already converted to `.stsd`), so a loose copy beside the package is a
second copy of every asset. And it is a copy nothing reads: `AssetSystem::Install()` puts
the packs in front of the filesystem, so a path the package holds always resolves out of
the package.

For Milistry that was 56 MB of dead weight beside a 90 MB package — `<exe>/assets` went
from 156.6 MB to 100.9 MB — most of it one 53 MB `.wav`. It is also a staleness hazard in
the one case the loose copy IS read: SDL loads the splash straight off disk before the
override exists (`Framework/stRun.cpp`), so an out-of-date loose `splash.bmp` wins over
the packed one. Removing it is safe because that code already falls back to reading the
splash through `AssetSystem`.

This replaced an older rule that copied the whole tree and then deleted only the
`.wiscene` and `.staod` files back out. That left every other packed asset duplicated,
and it re-copied them on EVERY build — `copy_if_different` sees the destination missing,
because the previous build had just deleted it. `PACK_ONLY` now describes what
`PACK_ASSETS` already does and is kept only as an accepted spelling.

**The loading window carries two lines because two different things report.**
`SubWinStatus` has a `status` (the PHASE - "Loading materials", "Processing assets",
pushed from the main thread by `st::App::SetLoadingStatus`) and a `detail` (WHAT is in
flight - the asset, its size, and n-of-total, pushed from the LOADING WORKER THREADS by
`st::AssetSystem`'s progress callback). One field cannot serve both: the fast producer
fires several times per frame and would overwrite the slow one, so the phase would never
be readable. Setting the status clears the detail, because a phase change retires
whatever belonged to the previous phase. The detail line is elided in the MIDDLE
(`SS_PATHELLIPSIS`, hand-rolled on X11) - it is a path, and dropping the tail leaves
every texture in a folder looking identical.

**Per-asset progress exists because there is a dead spot the engine cannot report.**
`Scene::Serialize` reports per component manager, and then waits for every texture the
scene referenced to finish loading. On a real map that wait IS the load, and nothing
reports during it - the bar sits still. Every one of those loads is a read through
`st::AssetSystem`, so that is the one place that knows which asset is in flight;
`SetLoadProgressCallback` fires there. It runs on job-system workers while the main
thread is blocked inside `Scene::Load()`, which rules out `loading_`, the `EventBus`
(main-thread only by contract), ImGui and the scene - `SubWinStatus` is the only sink
built for it, and while a scene loads it is the only thing that can paint anyway. The
callback deliberately does NOT move the progress bar: the engine owns it for the whole
load, and a second writer would make it jump backwards. `assetsExpected` comes from the
`.stsd`'s own reference list minus whatever is missing, which is what makes the count a
real "13 of 14" rather than a running total with no denominator.

**The Resource Explorer edits a WORKING SET, not the package.** A mounted package is
memory-mapped and, on Windows, locked — it cannot be written in place. "Edit" therefore
builds one row per asset that remembers only WHERE its bytes are (still in the source
package, a file on disk, or a buffer the window built), and copies nothing until Save.
Opening a 40 GB package to rename one texture costs a few hundred KB of rows, and every
untouched row is copied straight from the old package into the new one.

Save writes the complete new package into a staging folder FIRST — reading the untouched
rows out of the still-mounted original, which is why the unmount cannot happen any
earlier — and only then unmounts, deletes the old files, swaps the staged ones in and
remounts. A crash or a full disk at any point leaves the original intact. Old parts are
deleted by PATTERN rather than by count: the new package may have fewer parts than the
old one, and an orphaned `.stafpN` beside a new index is a mismatched UUID that makes the
reader refuse the whole set.

**The Resource Explorer is Editor-mode only, and that is a safety property.** Everything
it can do writes files. A plain DevUI session and a shipped build both ignore dropped
files entirely (`st::App::HandleDroppedFile` returns unless `IsEditorMode()`), so
dragging something onto a running game can never quietly start rewriting content. The
panel is a docked editor window rather than a floating DevUI one for the same reason
Hierarchy/Properties have `##editor` copies: docking one must not disturb the other.

**Everything the editor creates lands in front of the free camera.** `EditorUI::SpawnPoint()`
is six metres down the free camera's forward axis, and it is the one placement rule:
`Scene > Create` uses it, `Scene > Import model...` uses it, and so does a model dropped on
the viewport. Placement always goes through `PlaceEntityAt` (`devui/imcomponents.h`), which
writes the 64-bit ABSOLUTE position as well as `translation_local` — an object placed only in
local space comes back at the world origin after a save/load or an undo.

**Four interchange formats import; `.wiscene` is not one of them.** `wi::scene::LoadModel`
reads a `wi::Archive` and nothing else, which is the right SHIPPING format and the wrong
authoring one — nothing exports it. `Framework/io/model` is the other half: `.gltf`, `.glb`,
`.fbx` and `.obj` convert into components on load. `EditorUI::ImportModelAtSpawn` dispatches
on extension across all three loaders (`.stsd` → `AssetSystem`, `.wiscene` → `LoadModel`,
everything else → `st::model::Import`), and the editor's dialog filter and drop test are
generated from `st::model::SupportedExtensions()`, so adding a backend is one edit.

Handedness is converted in one place per backend and never twice: ufbx does it during load
(`target_axes` + `MODIFY_GEOMETRY`, so no node is left with a negative scale), and the glTF
importer mirrors Z per-vertex and reverses winding. `tests/model_import_test` covers the
third-party half — API shapes, parse options, accessor layout, external-buffer callbacks —
with no engine and no graphics device, which is the half that breaks on a dependency bump.

**Import options are an ImGui panel, not a file-dialog extension.** A native dialog can
only carry custom controls through a platform hook (a Win32 `OFNHookProc`), which would make
the options Windows-only — so the OS dialog picks the FILE and `EditorUI::DrawImportOptions`
asks everything else, as a modal, before anything is built. The dialog's filter LABEL is
built by `ImportFilterDescription()` from `st::model::SupportedExtensions()`, because the
Win32 helper shows `FileDialogParams::description` verbatim and never derives one from the
extension list: a bare "Model or scene" would tell the user nothing about what opens.

**"Import into a Hierarchy group" is not cosmetic.** A load creates the model's nodes AND its
mesh, material and animation-data entities — and those carry no `TransformComponent`, so they
get no parent, and the Hierarchy renders anything parentless as a top-level row. Importing one
character therefore adds hundreds of loose rows next to the model. With the option on, every
entity the import created is attached to the one root, which also makes the import a single
thing to select, move, hide or delete. `Component_Attach` only rebases a transform when BOTH
sides have one, so attaching a material is the pure metadata edit it looks like, and
`RunHierarchyUpdateSystem` skips a child with neither transform nor layer.

**Two things are called "import" and they are not the same.** The Resource Explorer imports
a file into the asset PACKAGE (`AssetExplorer::QueueImport`). `EditorUI::ImportModelAtSpawn`
merges a `.stsd` / `.wiscene` into the LIVE SCENE. `HandleDroppedFile` routes between them by
extension: a model goes into the world, everything else goes into the package. The package
side loses nothing by that — it has its own `Add files...` / `Add folder...` buttons.
An import is one undo step even though a model creates hundreds of entities: the set is found
by diffing the scene's entity list around the load and pushed through
`EditorHistory::RecordCreatedMany`. A step that owned only the root would delete it and orphan
every mesh and material behind it.

**Drag and drop, in one table.** Two payload types, several destinations:

| Payload | Dragged from | Dropped on | Result |
|---|---|---|---|
| `SIMTARY_ENTITY_PAYLOAD` | a Hierarchy row | another Hierarchy row | re-parent (`Component_Attach`, which rebases the transform so nothing jumps) |
| `SIMTARY_ENTITY_PAYLOAD` | a Hierarchy row | the strip under the tree | un-parent (`Component_Detach`) |
| `SIMTARY_ENTITY_PAYLOAD` | a Hierarchy row | an `EntityField` in Properties | bind a native component's entity reference (GUID, survives reload) |
| `SIMTARY_ASSET_PAYLOAD` | a Resource Explorer row | an asset field in Properties | set that texture / sound / script / sky map to the asset's logical path |
| `SIMTARY_ASSET_PAYLOAD` | a Resource Explorer row | the editor viewport | import the model at the spawn point (declined for non-models) |

A re-parent target is only opened when the drop would be LEGAL — never onto itself or one of
its own descendants — so an illegal row simply does not highlight, which reads as "not here"
without a separate rejection cue. `AssetPayload` carries the logical path and type, not just
the id, so a drop site needs to know nothing about the explorer's working set: a mounted
package resolves its own logical paths (`"textures/wall.dds"`) through the asset-source
override exactly as the engine stored them.

**A new editor panel does not appear in an existing layout.** `BuildDefaultLayout` runs
once and is then restored from `imgui.ini`, so a window added later comes up floating for
anyone with a saved layout. `Simtary > Editor > Reset Editor Layout` is the fix, and is
why that menu item exists.

**The editor saves `.stsd` by default; `.wiscene` is an explicit export.** `Scene > Save`
and `Save As` write the native descriptor, and a bare filename gets `.stsd`;
`Scene > Export .wiscene...` is the one that writes a self-contained `wi::Archive`, for
the standalone Wicked editor. `EditorUI::SaveScene` dispatches on the extension, so
either is reachable by typing one.

**The native save turns resource embedding ON for one serialize, and not for the bytes.**
`wi::resourcemanager`'s mode is `NO_EMBEDDING` throughout this workspace (nothing calls
`SetMode`), so a scene serializes with its resource block empty — which is already the
shape a `.stsd` blob wants. But the resource block is also the ONLY place the engine
records WHICH files a scene uses, and the descriptor needs that list: without it nothing
can report "13 of 14 assets" during a load, and `MissingAssetsFor()` cannot warn before
the world comes up untextured. So `SaveSceneDescriptor` flips to `EMBED_FILE_DATA`
around the one `Scene::Serialize` call, restores the previous mode immediately, and lets
`SplitWiscene` lift the block straight back out — the same code path the build-time
packer uses, and the one the tests prove byte-exact.

**A native save writes a sidecar package only for what is not already mounted.** Every
resource the mounted packages already hold is a reference and costs nothing; whatever is
left is content that session introduced, which lives nowhere the next run would look, so
it goes into `<name>.strd` beside the map and is mounted immediately. Skipping that step
gives a map that saves "successfully" and reloads grey.

**A native save ALSO writes the map's raw resources into the project's
`assets/resources/`, and the two halves answer different questions.** The sidecar package
above is for NOW: it makes the map reload in this session. The loose export is for the
next BUILD - `stpack pack --resource-dir` merges that folder into the game package, so a
model imported in the editor becomes shipped content instead of living in a stray `.strd`
beside the map. Without it the sidecar is the only copy, nothing mounts it on the next
launch, and the map that saved fine comes up untextured tomorrow.

It is the same write the build-time mirror does, and deliberately the same code:
`st::asset::ExportSceneResources` (`Framework/io/asset/SceneDescriptor.h`) is what both
`stpack --mirror-resources` and the editor call, because two copies of "write these
resources out under the names the map uses" would drift. What the editor adds is the case
the packer cannot see - resources that never came from a `.wiscene` on disk, because an
in-editor import brought them in from a model file.

Three rules the shared function carries, all of them load-bearing:

- **A file already in the folder is never overwritten.** It may be a deliberate override
  (`--on-conflict`), and rewriting tens of megabytes to reproduce bytes already on disk is
  time spent for nothing. A repeat export therefore costs one stat() per resource.
- **A name that escapes the folder is refused.** Resource names come out of a scene file
  and this writes into the developer's own source tree, so an absolute path, a drive
  letter or a `..` segment is counted and dropped rather than followed.
- **A resource with no bytes is not written.** That is what an embedded resource whose
  source file has since moved looks like (`Serialize_WRITE` re-reads from
  `container_filename` and gets nothing), and a 0-byte file here would be worse than
  none: the packer would merge it in as the texture, and every later export would skip
  it as already present.

Where it writes: `AppConfig::editorResourceDir` if the project set one, otherwise the
`assets/resources` the build baked in as `ST_PROJECT_RESOURCE_DIR`. It has to be baked -
the running game sits in the build output and the folder the packer merges from is beside
the project's sources, so the editor cannot find it by looking around itself. `Scene >
Export resources on save` turns it off; the setting is saved with the other editor
preferences. A failure to write does NOT fail the save - the `.stsd` and the sidecar are
what the map needs to exist, and losing a save over the next build's folder is the worse
trade.

**A zero `packUuid` in a `.stsd` is not a mismatch.** It means the map was never bound to
one package build, which is exactly what an editor save is when every resource it
references was already mounted. `stpack scene` only warns about a UUID that is set AND
different.

**`simtary_add_app(MODULE)` ships the game as a DLL beside the engine executable, and
the rules that make it safe are not optional.** `<NAME>.exe` holds engine + framework +
a loader `main()`; `application.dll` holds the project's `src/`. One source tree serves
both — `ST_APP_ENTRY` (`Framework/stModule.h`) expands to a `main()` normally and to the
module's exported descriptor with `MODULE`. What the split buys is relink time: editing
a scene relinks ~800 KB instead of 12 MB.

`MODULE_NAME` is free-form — `application` (the default, so the host needs to know
nothing about the project), the project's own name, or anything else; `--module=<name>`
overrides it at runtime with no rebuild. Naming it after the project is handled rather
than merely allowed: the module's import library, its `.exp` and its PDB would otherwise
collide with the host's, so the first two go to `<build>/module/` (nothing links them —
the host uses `LoadLibrary`/`GetProcAddress`) and the PDB gains a `_module` suffix.
dbghelp resolves a PDB by the name recorded in the image, so the suffix costs nothing.
Renaming leaves the previously-named DLL sitting in the output directory — delete it, or
a stale `--module=` still finds it.

It is a **matched-version** boundary, not a stable ABI. Host and module share C++ types
and must come from one build; `ST_ENGINE_BUILD_ID` encodes the version, the compiler and
every layout-affecting option (`SIMTARY_LARGE_WORLD` alone changes
`sizeof(TransformComponent)` by 12 bytes) and the loader refuses a mismatched pair rather
than reading a scene at the wrong stride.

The module links **no** static library the host already has. SDL2 and OpenAL are shared
and safe to link twice; ImGui, the engine and every other archive stay host-side, or the
module gets a second ImGui context and a second engine.

Exports are DERIVED, not annotated. `Simtary.lib` publishes ~64,700 symbols and a PE
export table holds 65,535, so `WINDOWS_EXPORT_ALL_SYMBOLS` cannot work here.
`cmake/simtary_module_def.ps1` intersects the module's undefined externals with what the
host defines and writes a `.def` — about 100 symbols for the template. Its "defined" side
must walk the link graph **transitively**: `Utility`, `Jolt` and `LUA` are separate
archives that `Simtary.lib` does not absorb, and missing them means the module fails to
link against symbols the host demonstrably has.

**Five header-static bugs had to be fixed before any of this could run, and they are all
the same bug.** A `static` inside an `inline` function or an inline variable is one
instance per BINARY, not per process. A DLL therefore gets its OWN copy of state the
engine assumes is global, and nothing warns you:

- `wi::ecs::CreateEntity()` — the id counter. Host and module each had one starting at 1,
  so both issued the SAME entity ids. Two entities sharing an id means attaching one to a
  root can attach it to itself, and `Scene::Entity_IsDescendant` walks parents with **no
  cycle guard**, so it spun forever. Now declared in `wiECS.h`, defined in `wiECS.cpp`.
- `wi::scene::GetScene()` / `GetCamera()` — the module got its own world. It would load a
  map into a scene the executable never renders. Now defined in `wiScene.cpp`.
- `wi::graphics::GetDevice()` — the module's copy stayed `nullptr` forever, because it is
  the executable's `Application::SetWindow` that assigns it. Now defined in
  `wiGraphicsDevice.cpp`.
- `wi::allocator::shared_allocators` was an inline variable — two 256-entry tables, two id
  counters both starting at 0, so a `wi::shared_ptr` released on the other side was freed
  through a block allocator built for a different type. Now a non-inline
  `shared_allocator_table()` in `Engine/wiAllocator.cpp`.
- `ska::flat_hash_map`'s `empty_default_table()` sentinel is a function-local static in a
  class template, and `deallocate_data` guarded with `begin != empty_default_table()`. A
  map default-constructed in the module and grown by host code failed that guard and
  called `free()` on a static array. It now recognises the empty state STRUCTURALLY
  (`max_lookups < min_lookups`, which `compute_max_lookups` can never return for a real
  allocation).

The symptoms were `0xC0000374` heap corruption at an unrelated `free`, and an infinite
spin inside a hash lookup — in both cases pages of stack with nothing pointing at the
cause. **The rule: state the engine treats as process-global must be declared in a header
and defined in exactly one `.cpp`.** Two known survivors are per-module by design and
sound: `make_shared_single`'s heap allocator and the `shared_block_allocator<T>` template
variable each register their own id in the now-shared table, so an object is always freed
through the allocator that made it. `wiPlatform.h`'s `SetWindowFullScreen` keeps a
per-binary `WINDOWPLACEMENT`, which only matters if both halves toggle fullscreen.

`Scene::Entity_IsDescendant` still has no cycle guard. Nothing produces a cycle now that
ids are unique, but a corrupt scene file would hang the app with no diagnostic.

**One CRT for the whole workspace, and `project()` must name every language.** `LUA` and
`SimtaryModelIO` are C targets; while `project()` declared `LANGUAGES CXX` only, CMake
never initialised the C runtime-library abstraction and MSBuild gave those two its own
default — the STATIC CRT — while every C++ target used the dynamic one. Two CRTs in one
link is two heaps. The visible symptom was only `LNK4098`; the invisible one was fatal at
a module boundary. Fixed by `LANGUAGES C CXX` plus an explicit
`CMAKE_MSVC_RUNTIME_LIBRARY`.

**Shader cache.** `Simtary/shaders/` holds `hlsl6/` and `spirv/` and is staged into
every game's output before the incremental `offlineshadercompiler` pre-pass, so first
launch is never cold. Windows warms BOTH, since either backend can be the one that
launches - not twice the work in practice, because the shipped cache covers both and the
pre-pass only recompiles what an edit invalidated. After a shader-heavy change, publish
the result back with the `simtary_shadercache_update` target, which now publishes every
backend it warmed.

**Exceptions and RTTI are off everywhere** (`/EHsc-`, `/GR-`; `-fno-exceptions`,
`-fno-rtti`). Do not introduce `throw`, `dynamic_cast` or `typeid` in engine or app
code. Native components use address-based `GetNativeTypeID<T>()` for type identity
for exactly this reason.

**One physical gamepad must not take two player slots.** On Windows this workspace
defines `SDL2=1` *and* `PLATFORM_WINDOWS_DESKTOP`, so `wi::input` compiles the XInput,
RawInput and SDL backends all at once and an Xbox pad is enumerated twice - once by
XInput, once by SDL - with the two copies deadzoned and signed independently.
`wiInput.cpp` now checks SDL's player index (which IS the XInput user index on Windows,
via `sdlinput::GetControllerXInputUserIndex`) against the already-registered XInput
slots and skips the duplicate. RawInput is a third enumeration in principle, but
`rawinput::ParseMessage()` is called from nowhere in this workspace - SDL2 owns the
message loop and there is no WndProc - so that path is inert and non-XInput pads
(DualShock/DualSense) reach the engine through SDL.

Deadzoning is ONE shared rule (`wi::input::ApplyStickDeadzone` /
`ApplyTriggerDeadzone`, tunable through `GetAnalogSettings()`), radial and rescaled.
It used to be three different per-axis constants - 0.24 in XInput, 0.26 in RawInput,
0.20 in SDL - none of which rescaled, so an axis snapped from 0 straight to the
deadzone value and flickered whenever the stick rested near that line. `ControllerState`
carries the pre-deadzone values alongside the processed ones purely so the DevUI scope
(`Simtary > Gamepad Analog`) can draw both on one axis; nothing in the game reads them.

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

**Dependencies are declared, not hand-rolled.** `ST_REQUIRE_COMPONENTS(TransformComponent,
Health)` in a component body is Unity's `[RequireComponent]`: the manager creates whatever
is missing during the same reconcile that built the instance, *before* `Awake()`, so
`Start()` can assume `GetComponent<T>()` returns non-null. Engine and native types mix
freely in the list, and requirements are transitive up to `REQUIRE_MAX_ROUNDS` (8) deep —
`CreateMissingInstances` and `ResolveRequirements` alternate until nothing new is attached,
which is why a required NATIVE component is live the same frame rather than one later.
A cycle settles instead of spinning because `requiredResolved` is set before the
requirements are applied, not after. Override `DescribeRequired()` by hand when a
registration NAME is needed rather than a type (`RequiredComponent::Native("stAudioEmitter")`
— a type registered under several names, as the audio components are, cannot be resolved by
type alone). Nothing is ever auto-REMOVED: detaching the requirer leaves what it pulled in,
because the dependency may be holding state.

`Require<T>()` is the imperative half, for a conditional dependency. Main thread only. An
engine component comes back usable immediately; a native one returns the existing instance
or, if there is none, attaches it and returns **nullptr** — attaching is only a metadata
write, so the instance appears on the next reconcile.

**An inspector edit only persists if it goes back through the metadata.** The scene saves
`MetadataComponent`, not the live instance, so a `DrawDebug()` widget writing straight into
a member changes the frame and nothing else — the value is gone on the next load. `Bind()`
now RECORDS every field it binds, and `SaveBoundParams()` writes them all back to their
`NCA_` keys; a hand-drawn `DrawDebug()` calls it once its widgets report a change
(`if (dirty) { Apply(); SaveBoundParams(); }`, which is what every component in
`Framework/` does). Parameters exposed through `DescribeParams()` instead need none of
this: the shared inspector writes each edit back through `Set*()` itself.

**The Add Component list is grouped and categorized, and both are part of the
registration.** A registration carries a `group` (section title), a `tag` (a badge in
front of it) and a `category` (a sub-tree inside the group), so the picker is sections
rather than one flat scroll of everything the executable linked in.
`ST_REGISTER_NATIVE_COMPONENT(_AS)` files a component under **Project** with no
category; `ST_REGISTER_FRAMEWORK_COMPONENT(_AS)(TYPE, NAME, CATEGORY)` — and the explicit
`RegisterNativeComponent(..., "Framework", "ST", "Audio")` the audio components use —
puts it under **[ST] Framework** in that category;
`ST_REGISTER_NATIVE_COMPONENT_IN(TYPE, NAME, GROUP, TAG, CATEGORY)` invents any other
section a game wants. The framework's categories are **Audio** (emitter, collector,
speaker, microphone, geometry/wall/room), **Optical** (laser, mirror, lens, projector)
and **Scene** (ray, day/night).

Only Project is open by default; categories inside an opened group are open but
collapsible; typing in the search box forces open exactly the sections and categories
that have a hit and hides the rest. Engine components (the 38 `Scene` managers) sit last
and closed. An empty category means "directly under the group header", which is what a
project that never names one gets, so the plain list still looks plain.
`GetRegisteredNativeComponentGroups()` is what the editor reads — each group carries both
the flat `components` list and the `categories` split of the same names;
`GetRegisteredNativeComponentNames()` still returns everything as one list.

**Steam Audio hears nothing until something registers geometry, and `stAudioGeometry` is
that something.** `Spatializer::AddStaticMesh` existed from the start and nothing called
it, so occlusion, transmission, reflections and pathing were all inert no matter what an
emitter asked for. `Engine/stAudioGeometry.{h,cpp}` is one component under three names —
`stAudioRoom` (a hollow box of up to six slabs around an interior), `stAudioWall` (one
slab) and `stAudioGeometry` (a solid Box, or Mesh: the entity's own rendered triangles) —
with Steam Audio's material table as presets plus Custom. The name only picks the default
shape; the dropdown can change it afterwards.

Two things about it are load-bearing. Every shape is a CLOSED solid with real
`thickness`, because transmission is computed through the material — a zero-thickness
wall leaks everything — and because a closed slab presents a real surface to a listener
on either side whichever way the tracer orients a normal. And geometry is registered in
ORIGIN-RELATIVE space like every other audio coordinate, so a large-world rebase
invalidates a mesh that has not moved a millimetre: the component re-registers on an
origin change even when `staticGeometry` is on. Otherwise static geometry is registered
once at `Start` and never re-read; a door sets `staticGeometry` off and pays a rebuild
(and a scene re-commit) at `rebuildRateHz`.

**The graphics backend is chosen at STARTUP and both are in the Windows build.**
`st::ResolveGraphicsAPI` (`Framework/render/GraphicsAPI.h`) picks it from, highest
first: the command line (`vulkan` / `dx12`), the player's saved choice (`options.stad`,
`display` -> `graphicsAPI`, written by the Display panel), `AppConfig::graphicsAPI`, and
finally the platform default - DirectX 12 on Windows, Vulkan elsewhere. A backend whose
runtime is not installed falls back to the other rather than failing to start, which is
also why the command line outranks the saved choice: a machine that saved a backend it
cannot run is still startable.

Three things about it are load-bearing:

- **The device is created by the FRAMEWORK, before `Application::SetWindow`.**
  `st::App::CreateGraphicsDevice()` is called from `st::Run`; SetWindow only builds a
  device when none exists, which is the seam the engine documents for exactly this. Doing
  it there is also what lets the shader path be pointed at `shaders/hlsl6/` or
  `shaders/spirv/` to match, since SetWindow appends nothing when it did not create the
  device.
- **`wi::arguments::Parse` must run before that.** It used to be called AFTER SetWindow,
  so `vulkan`, `dx12`, `debugdevice` and the GPU preference flags were all read from an
  empty argument set and did nothing at all; the device was always the platform default.
- **`SDL_WINDOW_VULKAN` is only asked for on a Vulkan run.** SDL loads the Vulkan loader
  for that flag and fails window creation when there is none, so passing it
  unconditionally made a DirectX 12 build refuse to start on a machine with no Vulkan
  driver.

Switching is a RESTART, not a live toggle: a device owns every texture, buffer, pipeline
and shader the engine has. `st::RequestRestart()` (`stRun.h`) shuts the game down
normally - so `App::Exit` writes `options.stad` first - and starts the same command line
again; the Display panel's "Restart Now" is its one caller.

Other useful args: `debugdevice`, `gpuvalidation`, `gpu_verbose`, `igpu`, `nvidiagpu`,
`amdgpu`, `intelgpu`.

## Git

`Engine/` is a separate repository with its own remote — commits there go upstream to
Simtary4, so keep workspace-specific changes in `Framework/` unless the change is
genuinely an engine-core fix. `libs/ImGui`, `libs/ImGuizmo`, `libs/libzmq` and
`libs/SDL2` are submodules of THIS repository.
