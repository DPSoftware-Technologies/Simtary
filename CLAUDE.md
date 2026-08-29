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
├── tests/ tools/   nbt_test, asset_pack_test, model_import_test, stpack,
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
| `SceneManager.*` | Named scene registry with deferred transitions + load/unload callbacks. |
| `ImguiHelper.cpp` | The hand-rolled ImGui backend and the per-frame UI ordering. |
| `devui.cpp`, `devui/` | **DevUI** — developer tooling, not game UI: menu bar, backlog, graphics settings, hierarchy/properties, scene manager, About. Gated by `AppConfig::devUI`. |
| `devui/imassets.*` | **Resource Explorer** — browse a mounted package (which part, what offset, which codec, what hash), and edit one: add / remove / rename / recompress assets and write it back. Lives ONLY in Editor mode, docked along the bottom. Reached through `App::Resources()`. |
| `devui/imcomponentinspectors.*` | **The Properties field editors** — one function per `wi::scene::Scene` component manager, every option each component has. All 38 managers are covered, so the inspector's "no inline editor" list is a missing-table report, not a normal state. `imcomponents.*` stays the ADD/REMOVE catalogue; this is the EDIT surface. Name and Transform are drawn by `imhierarchy.cpp` instead, because they carry panel behaviour (shared row, euler cache, drift check) rather than a field list. |
| `stLoading.cpp` | Default `RenderLoadingScreen` — the pipeline warm-up overlay plus whatever `LoadingState` carries. |
| `io/` | `Nbt` (the `.stad`/`.stcd` format), `NbtStore`, `SettingsManager`, `SaveGame`, `PlayerPrefs`, `UserData` (LocalLow path resolver). |
| `io/model/` | **Model import**: `.gltf`/`.glb` through tinygltf v3, `.fbx` and `.obj`/`.mtl` through ufbx. `st::model::Import()` builds nodes, meshes, PBR materials, skins and animations straight into a `wi::scene::Scene`. Both loaders are C — this workspace is `_HAS_EXCEPTIONS=0`, and a throwing C++ parser would need its own exception settings behind a C boundary. Every byte reaches them through `wi::helper::FileRead`, so a model inside a mounted asset package imports like one on disk. |
| `io/asset/` | **The asset package**: `.strd` index + `.stafp<N>` payload parts + `.stsd` maps. `AssetFormat.h` is the on-disk layout, `AssetPack` reads it, `AssetPackWriter` builds it, `SceneDescriptor` splits and rebuilds `.wiscene`, `StHash.h` is XXH64. `AssetSystem` is the only engine-aware file: it mounts packages and installs `wi::helper::SetAssetSourceOverride` so every engine read resolves through them. Reach it anywhere via `st::AssetSystem::Get()`, or `App::Assets()`. |
| `input/InputSystem.*` | Centralized action/axis keymap, refreshed once per frame. |
| `crash/CrashHandler.*` | sentry-native + Crashpad, offline only; launches `SimtaryCrashReporter`. |
| `render/LensFlare.*` | Procedural screen-space flare (`assets/shaders/StLensFlare*`). |
| `render/Projector.*` | `st::Projector` + `st::ProjectorSystem` — SQUARE (or rect/ellipse/rounded) image projection with projector optics: throw ratio, aspect, lens shift, keystone, barrel/pincushion, edge softness, vignette, plus a rectangular volumetric beam. `opticBounces` reflects the image off `st::Mirror` and images it through `st::Lens` by uploading a virtual projector clipped to that element's aperture. Runs as a `RenderPath3D` custom post process (`assets/shaders/StProjectorCS.hlsl`), plus one depth-only pass per shadow-casting projector (`RenderShadows()`, driven from `st::App::Render()` BEFORE the render path so the command list is recorded ahead of the pass that samples it). Reach it anywhere via `st::ProjectorSystem::Get()`. |
| `render/ProjectorComponent.cpp` | The `"Projector"` NATIVE COMPONENT — attach `st::Projector` to a spot light from the editor (`NCI_0 = "Projector"`, optics as `NCA_0_*` args). Follows its own entity, takes the image off it (video / camera render / material base colour, pinned with `NCA_0_imageSource`), and zeroes that light by default since the light IS the circle. |
| `render/Laser.*` | `st::Laser` + `st::LaserSystem` — traced laser beams. Millimetre-thin core plus a wide halo, both integrated ANALYTICALLY per pixel (`assets/shaders/StLaserCS.hlsl`) rather than ray marched. Walks the beam through the optics below and stops it on the first surface `st::Raycast` reports, so one laser can be several straight legs. The impact spot leaves a fading persistence trail, which is what makes a moving beam draw a line instead of blinking a dot. `LaserArray` turns one laser into a grid/ring/fan/cross/spiral of rays (off by default — each ray is a full trace). `st::LaserSystem::Get().Path(id)->hit` is what the beam is on. |
| `render/LaserComponent.cpp` | The `"sticLaser"` NATIVE COMPONENT. Follows its own entity; picks up a sibling `"sticRay"` (mode/range/axis) every frame instead of casting a second time. |
| `render/Optics.*` | `st::Mirror` + `st::Lens` + `st::OpticsSystem` — flat apertures a beam reflects off (`d' = d - 2(d·n)n`) or bends through. `Lens::Type` covers Spherical / Cylindrical / Toric / Aspheric / Axicon / Prism / Window, all one paraxial ray transfer with a different deviation term. `Mirror::dichroic` splits the beam in two (reflect one band, transmit the rest), which is why `Trace()` walks a stack of branches rather than a single chain. `Trace()` is the sequential walk that turns one ray into a list of legs; it lives on the CPU because leg N+1 depends on where leg N landed, which a per-pixel pass cannot discover. Draws nothing, and carries its own “is a beam reaching me” diagnostics because a silent element is otherwise undebuggable. |
| `render/OpticsComponents.cpp` | The `"sticMirror"` / `"sticLens"` native components. |
| `scene/Ray.*` | `st::Raycast` / `st::RayHit` / `st::RayQuery` — one raycast over either backend: `Mesh` (`Scene::Intersects`, hits anything drawn, no body needed), `Physics` (`wi::physics::Intersects`, Jolt bodies only), `Both` (nearer wins) or `None`. Also `st::LocalAxes` — the ONE forward-axis table, shared by the projector, laser, ray and optics. |
| `scene/RayComponent.*` | The `"sticRay"` native component: a raycast bolted to an entity, re-cast every frame. The shared seam for a laser sight, a rangefinder, an interaction prompt and an “am I aiming at it” HUD. |
| `render/Framebuffer.*` | `st::gfx::Framebuffer` — an off-screen surface you draw into and hand to a material, a light mask or a projector. CPU mode wraps libgfx (`GFXcanvas`) and owns the staging texture, row pitch and flip; GPU mode is a render target you draw into with `wi::image`/`wi::font` between `Begin()`/`End()`. |
| `display/DisplaySettings.*` | Player-facing video options: window mode, monitor, resolution, refresh rate, v-sync, frame cap, render scale. NOT DevUI — `st::App::Display().GUI(app)` drops into a game's own options menu, and DevUI renders the same panel in its Display tab. Sole owner of v-sync and the frame cap; `GraphicsSettings` deliberately no longer carries them. |
| `audio/faust/` | `FaustManager` (OpenAL DSP host) + `FaustProcessor<T>`. Starts with no processors — games register their own AOT instruments. |
| `SubWinStatus.*` | The native (Win32/X11) loading window, on its own thread, because a blocking `Scene::Load()` means no ImGui frame can be drawn. Two text lines - a phase and a dimmed, middle-elided detail line - plus a progress bar. Every setter is thread-safe; the detail line is written from loading workers. |
| `anim/`, `eventBus.*`, `ZmqHandler.*` | Animation descriptors (`.staod`, NBT — read through `wi::helper::FileRead`, so they resolve out of a mounted asset package), main-thread event bus, ZMQ bridge, native (Win32/X11) loading window. |

### Project hook surface (`st::App`)

Content: `RegisterScenes`, `OnInitialize`, `OnExit`.
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

- `simtary_add_app(NAME ... ORGANIZATION ... ICON ... [SOURCE_DIR] [ASSETS_DIR] [CONTENT_SUBDIR] [EXTRA_SOURCES|INCLUDES|LIBS] [NO_SHADER_WARM] [NO_CRASH_REPORTER] [PACK_ASSETS] [PACK_ONLY] [PACK_NAME] [PACK_PART_SIZE] [PACK_LEVEL])`
- `simtary_pack_assets(TARGET ... CONTENT_DIR ... [NAME] [PART_SIZE] [LEVEL] [PACK_SUBDIR] [SCENE_SUBDIR])`
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
can only ever load a `.cso` that the build already produced. `StProjectorCS.hlsl` and
`StLaserCS.hlsl` are the shaders in this category.

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

**A packed build must not also ship the `.wiscene`.** `copy_directory` has no filter, so
`<APP>_Assets` copies the maps along with everything else; with `PACK_ASSETS` on, the
copy step then deletes exactly those files from the output. Leaving them costs ~37 MB
each of duplicated content AND leaves a second copy that can go stale and still be
found. The removal lives on the copy target, not the pack target, because the pack
target is incremental and skips itself when up to date - by which time the copy has
already put the `.wiscene` back.

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

**A zero `packUuid` in a `.stsd` is not a mismatch.** It means the map was never bound to
one package build, which is exactly what an editor save is when every resource it
references was already mounted. `stpack scene` only warns about a UUID that is set AND
different.

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
