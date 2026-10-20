# Simtary editions and renderer architecture

Status: **design only (2026-10-14).** Nothing below is implemented except the Detroit renderer
(the current engine) and the Gumin safeguards already made for Mali-G68 (PORTING-ANDROID.md).

**Phase 1 done (2026-10-20).** `Framework/render/api/Renderer.h` is the render interface
(Renderer + Draw2D), `Framework/render/detroit/` implements it on Wicked, and
`cmake/SimtaryRenderBoundary.cmake` fails the build when Framework code outside its
Detroit-only list touches Wicked's renderer. See "Phase 1 as built" below.

**Phase 3 spike running (2026-10-20): Hurricanes on Filament draws vehicletest.** `libs/filament`
(submodule, tag v1.77.3), `tools/build_filament.ps1` -> `deps/filament-install`, CMake
`SIMTARY_RENDERERS="Detroit;Hurricanes"` (Milistry preset `win_x86-64-hurricanes-dev`), run with
`--renderer=hurricanes`. See "Phase 3 spike as built" below.

**DRAFT (2026-10-20): Hurricanes on Google Filament.** The Petrol renderer is proposed to be an
adapter over Filament instead of a hand-written GL renderer - see "Hurricanes renderer
(Petrol)". Not decided; it waits on the Phase 3 spike.

## Editions

From the project sheet. An edition is a BUILD CONFIGURATION of one engine, not a fork.

| Architecture | Standard | Enterprise | Character | Processor | Target |
|---|---|---|---|---|---|
| JP8 | - | GEPA, Banker | like Caterpillar / Guratra, more restricted, highest performance | enterprise x86-64 (+ ARM64 for Banker) | not common processors |
| Diesel | **Detroit** (in dev) | **Caterpillar** (next) | high precision, realistic, advanced compute | x86-64; Caterpillar multicore x86-64 | normal PC; HEDT / research / enterprise |
| Diesel | Gumin | Guratra | Detroit with more safeguards and optimisation | x86-64, ARM64 | flagship phones, Apple Silicon, Snapdragon X Elite; HEDT |
| Petrol | **Hurricanes** | - | low precision / graphics, basic compute | x86-64, ARM64 | most mobiles, low-end PC |
| Electric | Nikola | - | - | RV64, ARM32/64 | - |

What separates editions is mostly the **renderer** and the **precision / quality defaults**.
Everything else is shared.

## Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Game (Milistry, template, ...)                                         │
├──────────────────────────────────────────────────────────────────────┤
│ Framework  st::App, scenes, input, terrain, vehicle, audio, Taipan,    │  shared by
│            assets, devui, dev bridge                                    │  every edition
├──────────────────────────────────────────────────────────────────────┤
│ Core       wi::scene (ECS), Jolt physics, jobsystem, resource manager, │
│            audio engine, math, archive                                  │
├───────────────────────────────┬──────────────────────────────────────┤
│ Render interface (new, thin)  st::render::Renderer                     │
├───────────────────────────────┼──────────────────────────────────────┤
│ Detroit renderer              │ Hurricanes renderer (new, DRAFT)      │
│  = current wi::renderer +     │  = FilamentRenderer adapter over      │
│    RenderPath3D               │    Google Filament: GL ES 3.0+ /      │
│  Vulkan 1.3 / DX12, bindless  │    Vulkan / Metal / WebGL2,           │
│  Gumin = + mobile safeguards  │    materials compiled by matc         │
└───────────────────────────────┴──────────────────────────────────────┘
```

Rules that keep the split clean:

1. **Core and Framework never include a renderer header.** Anything that draws today from
   Framework (projector, laser, lens flare, ImGui backend, terrain chunk rendering, the editor
   viewports, the game UI - `ui/UISystem` draws through `wi::image` / `wi::font` and
   `ui/UIRml` through `wi::graphics`) goes through the render interface, or is marked
   Detroit-only. With Filament this rule is strict, not stylistic: a Petrol build has no
   `wi::graphics::GraphicsDevice` at all, because Filament owns the GPU context.
2. **Scene data is renderer-neutral.** `MeshComponent`, `MaterialComponent`, lights, weather stay
   the single description; each renderer reads what it supports and ignores the rest (a
   Hurricanes build simply has no SSR, voxel GI, path tracing, volumetric clouds).
3. **GPU resources belong to the renderer.** Components keep CPU data plus an opaque handle
   the active renderer fills in. Today `MeshComponent` owns `wi::graphics::GPUBuffer`s directly
   - that is the main refactor (see Phase 1).

## The render interface

Deliberately small: what the GAME needs, not what a GPU API offers.

```cpp
namespace st::render {

struct Caps {
    bool bindless = false;
    bool compute = false;
    bool shadows = true;
    int  maxTextureSize = 4096;
    const char* name = "";
};

struct FrameDesc {
    const wi::scene::Scene*           scene = nullptr;
    const wi::scene::CameraComponent* camera = nullptr;
    float renderScale = 1.0f;     // internal resolution vs window
    int   qualityTier = 0;        // 0 low .. 3 ultra, renderer maps it to its own options
};

class Renderer {
public:
    virtual ~Renderer () = default;
    virtual bool Initialize (void* nativeWindow) = 0;
    virtual void Shutdown () = 0;
    virtual void Resize (int width, int height) = 0;
    virtual Caps GetCaps () const = 0;

    // scene resources: called when components appear / change / disappear
    virtual void SyncScene (wi::scene::Scene& scene) = 0;

    virtual void RenderFrame (const FrameDesc& frame) = 0;   // 3D
    virtual void RenderUI (ImDrawData* ui) = 0;              // ImGui on top
    virtual void Present () = 0;

    // pick / screenshot / debug, used by editor + dev bridge
    virtual bool ReadBackColor (std::vector<uint8_t>& rgba, int& w, int& h) = 0;
};

std::unique_ptr<Renderer> CreateRenderer (Edition edition);

} // namespace st::render
```

`DetroitRenderer` wraps what exists (`wi::renderer` + `RenderPath3D` + the Vulkan/DX12 device).
`HurricanesRenderer` is new.

DRAFT addition - 2D drawing for the game UI. `UISystem` and RmlUi need a little more than
`RenderUI(ImDrawData*)`: textured quads with 9-slice / fill / rounding, SDF text, scissor, and
RmlUi's compiled geometry with a transform. Proposed as a second small interface each renderer
implements, so UI code stops calling `wi::image` / `wi::font` directly:

```cpp
class UIRenderer {
public:
    virtual ~UIRenderer () = default;
    virtual TextureHandle LoadTexture (const char* path) = 0;
    virtual TextureHandle CreateTexture (const uint8_t* rgba, int w, int h) = 0;
    virtual void DrawImage (const ImageParams& p) = 0;            // tint, 9-slice, fill, radius, gradient
    virtual void DrawText (const TextParams& p) = 0;              // font, size, wrap, align, shadow
    virtual GeometryHandle CompileGeometry (const UIVertex* v, int nv, const int* idx, int ni) = 0;
    virtual void DrawGeometry (GeometryHandle g, TextureHandle t, const float transform[16]) = 0;
    virtual void SetScissor (int x, int y, int w, int h, bool enabled) = 0;
};
```

## Hurricanes renderer (Petrol)

Target: Mali-G52/G72 class, Adreno 5xx/6xx, Intel/AMD integrated GPUs. 30 fps at 720p internal.

### DRAFT: build it on Google Filament

Proposal (2026-10-20): Hurricanes is `FilamentRenderer`, an adapter that mirrors `wi::scene` into
[Filament](https://github.com/google/filament) (Google's mobile-first PBR renderer, Apache 2.0),
instead of a renderer written here. The table below is what that gives; the original
hand-written plan is kept after it as the fallback.

| Area | Hurricanes on Filament |
|---|---|
| API | Filament backends: OpenGL ES 3.0+ (Android - reaches further down than the 3.2 planned), desktop GL, Vulkan, Metal (iOS / macOS), WebGL2 via WASM (browser - fits BHS / cloud) |
| Lighting | Filament's clustered forward: sun + point / spot lights, physically based units |
| Shadows | cascaded shadow maps for the sun (PCF / VSM), spot shadows |
| Materials | Filament `lit` model = metallic-roughness, same as Detroit, so a map looks alike in both editions. A handful of `.mat` files (opaque, masked, transparent, terrain splat, unlit) compiled by `matc` to every backend at BUILD time |
| Sky | Filament skybox + IBL from a prefiltered cubemap (`cmgen`); no atmosphere raymarch |
| Terrain | same `TerrainData`; chunks submitted as Filament renderables with a splat material; no virtual texture |
| Post | Filament's: tonemap, bloom, FXAA / TAA, dynamic resolution, fog - each switched by quality tier |
| Textures | KTX2: ASTC (Android), BC (PC), basis/uastc as one universal option |
| Precision | float world positions (`SIMTARY_LARGE_WORLD=OFF`), floating origin for big maps |
| Shaders | none hand-written for 3D; only the material sources |
| Not supported | voxel GI, SSR as Detroit has it, volumetric clouds, path tracing, GPU particle emitters, ocean, hair |

What the adapter does (`SyncScene` in the render interface is exactly this):

| `wi::scene` | Filament |
|---|---|
| entity with Transform + Object + Mesh | `utils::Entity` + `TransformManager` instance + `RenderableManager` (one primitive per mesh subset) |
| `MeshComponent` vertex / index data | `VertexBuffer` / `IndexBuffer`, built once, rebuilt on change (skinned: bone matrices per frame) |
| `MaterialComponent` | `MaterialInstance` of the matching `.mat`, parameters copied (base colour, roughness, metalness, emissive, normal map, alpha mode) |
| `LightComponent` | `LightManager` (directional / point / spot) |
| `CameraComponent` | `Camera` + `View` |
| `WeatherComponent` (sky, fog, sun) | `Skybox`, `IndirectLight`, `View` fog options |
| mesh LODs (`subsets_per_lod`) | adapter picks the level per frame (Filament has no LOD of its own) |

What it costs, honestly:

- **A second scene in memory.** Filament keeps its own entities; the adapter keeps a map
  `wi::ecs::Entity -> utils::Entity` and must track create / change / remove. Dirty tracking on
  `wi::scene` is the work; a full re-walk each frame is too slow for Scene1-sized maps on phones.
- **Everything Framework draws itself must be ported** - game UI (`UIRenderer` above, plus an
  RmlUi render interface on it), ImGui (Filament ships `filagui`), lens flare, projector, laser
  (or marked Detroit-only), particles (Petrol gets simple CPU billboards, not the GPU emitters).
- **No `wi::graphics` in a Petrol build.** Streaming (`stream/StreamHost`), screenshots and the
  bridge's readback go through `Renderer::ReadBackColor` / a Filament `readPixels` path instead
  of the shared D3D12 texture ring. BHS on Filament needs its own encoder hand-off.
- **Build.** Filament is a large CMake project with its own tools (`matc`, `cmgen`, `mipgen`)
  needed on the host at build time; prebuilt releases exist for Android / desktop. Its libraries
  add a few MB. Its exception / RTTI settings must be checked against this workspace's
  `/EHsc- /GR-` contract before its headers are included from engine or app code - if they do
  not fit, it sits behind a wrapper the way JSBSim does.
- **Two renderers to keep looking alike.** Same PBR model makes this mostly lighting and
  exposure calibration (Filament uses physical light units; Detroit's intensities need a
  conversion table).

Why it is still worth it: the original plan below is 3-4 weeks for a first image and then
the long tail - shadow artefacts, driver bugs, ASTC paths, iOS - that Filament has already
paid for on exactly these GPUs. The engine-specific work (scene sync, materials, UI) is
needed either way.

### Fallback: the original hand-written plan

| Area | Hurricanes |
|---|---|
| API | OpenGL ES 3.2 (Android), desktop GL 4.3 core (low-end PC), same code path |
| Lighting | forward, 1 directional (sun) + up to 8 point/spot per draw, picked on CPU |
| Shadows | 1-2 cascade shadow map for the sun, 1024-2048 |
| Materials | base colour, normal, roughness/metal (packed), emissive; alpha test / blend |
| Sky | analytic gradient + sun disk (no atmosphere LUT raymarch) |
| Terrain | same `TerrainData`; renderer-side mesh LOD + 4-layer splat texture, no virtual texture |
| Post | tonemap + optional FXAA, bloom only on the high tier |
| Textures | ASTC (Android), BC on PC GL, RGBA8 fallback |
| Precision | float world positions (`SIMTARY_LARGE_WORLD=OFF`), floating origin for big maps |
| Shaders | ~20 small GLSL ES 3.20 shaders, hand-written; no permutation explosion |
| Not supported | bindless, compute-heavy effects, GI, SSR, volumetric clouds, path tracing, mesh shaders |

Why GL ES rather than Vulkan 1.1 here: Mali/Adreno GL drivers are mature, run on effectively
every Android device (including ones without Vulkan), and avoid the driver-compiler bugs met on
Mali Vulkan. A Vulkan 1.1 non-bindless backend for Hurricanes stays possible later behind the
same interface. (Filament keeps this choice open at runtime: GL ES and Vulkan are both its
backends.)

Shaders: hand-written GLSL rather than cross-compiling Detroit's HLSL. The Detroit shaders are
built around bindless descriptor indexing; translating them gives GL shaders with the same
problem. A small separate set is cheaper to write than to fight.

## Gumin (Detroit + safeguards)

Already started on the tablet - everything in PORTING-ANDROID.md "Step 3": `spirv_mobile`
shader set, descriptor-set and limit headroom, no Int64 / clip distance, BC CPU decode, Mali
compiler workarounds, pipeline cache. Remaining: ASTC content, render scale + memory budget,
GUI blur fault, a mobile quality preset. Gumin needs a Vulkan 1.3 bindless GPU (Mali-G68+,
Adreno 6xx+ recent drivers, Xclipse, Apple via MoltenVK later).

Xclipse (Exynos 1480 / 2x00) is AMD RDNA-based hardware with a Samsung driver: Int64, clip
distance, 1024-thread workgroups, BC and many descriptor sets are expected, so it may run the
desktop `spirv` set rather than `spirv_mobile` - to be confirmed from its Vulkan Caps Viewer
report before relying on it. Testing it needs the game data inside the APK (AAssetManager),
since that phone stays without Developer options.

## Build configuration

One CMake option picks the edition; it sets the renderer and defaults, nothing else.

```
-DSIMTARY_EDITION=DETROIT | GUMIN | HURRICANES | CATERPILLAR
```

| Edition | Renderer | Shader set | LARGE_WORLD | Default quality |
|---|---|---|---|---|
| DETROIT | Detroit | hlsl6 + spirv | ON | high |
| GUMIN | Detroit | spirv_mobile | OFF on mobile | medium |
| HURRICANES | Hurricanes (DRAFT: Filament) | Filament materials (`matc` -> GLES / Vulkan / Metal); was glsl_es | OFF | low |
| CATERPILLAR | Detroit (+ double precision paths, multicore sim) | hlsl6 + spirv | ON | ultra |

Presets: `win_x86-64` (Detroit), `android_arm64` (Gumin), `android_arm64-petrol` and
`win_x86-64-petrol` (Hurricanes). A runtime fallback is allowed on Android: start Gumin, and if
the device lacks bindless / Vulkan 1.3, start Hurricanes instead - one APK for both.

## Phases

| # | Work | Size |
|---|---|---|
| 1 | Render interface + DetroitRenderer as a wrapper; move Framework drawing (ImGui backend, lens flare, projector, laser, editor viewports, **game UI via `UIRenderer`**) behind it. No behaviour change. | 1-2 weeks |
| 2 | GPU resources out of components (mesh / material / texture handles owned by the renderer). Biggest refactor; Detroit keeps working throughout. | 2-3 weeks |
| 3 | DRAFT (Filament): **spike first, 1 week** - Filament on desktop GL in a test app beside Simtary, `SyncScene` for meshes / materials / sun / camera, vehicletest drawn; check build flags, binary size, look vs Detroit, and frame time on the Mali tablet. Go / no-go. Then the full `FilamentRenderer` (dirty tracking, skinning, terrain, LODs, sky / IBL, UI + RmlUi on `UIRenderer`, ImGui via filagui). Was: HurricanesRenderer on GL 4.3 by hand, 3-4 weeks. | 1 + ~3 weeks |
| 4 | Same renderer on Android (GL ES, Vulkan where it is better) on the A51 (Mali-G72) and the tablet; KTX2 / ASTC; touch controls. iOS / WebGL become possible here at little extra renderer cost. | 2 weeks |
| 5 | Edition presets, runtime fallback, per-edition quality tiers. | 1 week |

Phase 1 alone is worth doing even before Hurricanes: it is what makes the Mali work, the
editor, and the dev bridge stop reaching into `wi::renderer` from everywhere.

### Phase 1 as built (2026-10-20)

The interface came out smaller in one place and larger in another than the sketch above:

- **Frame lifecycle stays with `wi::Application`.** `st::App` is still a `wi::Application`; its
  `Render()` / `Compose(cmd)` overrides forward to the Detroit renderer's host half
  (`DetroitRenderer::Initialize / BeginFrame / RenderXREyes / Compose`). This matches the plan
  for Petrol: there, `wi::Application` keeps running the loop on `GraphicsDevice_Null` and the
  Filament renderer presents - so no portable `RenderFrame/Present` was needed yet.
- **Draw2D was added** (the `UIRenderer` drafted above): game UI and RmlUi draw only through
  it. `TextureRef` / `GeometryRef` are `shared_ptr` handles the backend subclasses.
- **Services the framework and games actually used** went into `Renderer`: `GameImage`,
  `GameResolution`, `SaveImage`, `ToResource` (a renderer texture as a `wi::Resource`, which is
  how scene data holds textures), `ImGuiTexture`, `PickRay`, thread-safe debug shapes,
  `DebugOverlays`, pipeline/VRAM/adapter status, `Update`, `Load/SaveSettings`.
- **Detroit-only, by list** (the boundary script says why for each): the backend; the
  `wi::Application` host (`stApp.*`, `stRun.cpp`); lens flare, projector, laser, framebuffer,
  GraphicsAPI; XR; streaming; the editor's viewports, template preview, terrain panel and the
  graphics options panel. Everything else in Framework is on the interface - UI, RmlUi, DevUI
  (asset thumbnails, About icon, VRAM readout), loading screen, display options, scripts' debug
  lines, ray component, editor bridge screenshots - and so are Milistry's two users (mouse
  raycast, explosion puff texture).
- **Not done in Phase 1:** `stApp.h` still hands out Detroit types (`GamePath()`, `Graphics()`,
  `Projectors()`, `Lasers()`, `OnRenderPathSetup(wi::RenderPath3D&)`, `OnPreCompose/OnCompose(
  CommandList)`) - code using those is Detroit code, whatever folder it is in. Device creation
  is still `stRun`/`GraphicsAPI`. Phase 3 moves these behind the edition switch.

## Phase 3 spike as built (2026-10-20)

- **Switching**: a build carries the renderers `SIMTARY_RENDERERS` lists; `--renderer=` picks one
  at startup (`Framework/render/api/Backend.h`), restart to change. Detroit-only builds carry no
  Filament (a stub `HurricanesRenderer` that is never Ready). The About box shows the RUNNING
  edition (`Caps::codename / architecture / api / basedOn`).
- **A Hurricanes run**: `stRun` gives Wicked `GraphicsDevice_Null` (no SDL_WINDOW_VULKAN), the
  Detroit host still initialises on it (so Detroit-only accessors have objects), and
  `HurricanesRenderer` takes the window's HWND, becomes `st::render::Get()`, and in
  `App::Render` mirrors `wi::scene` into Filament and presents. Scene update, physics,
  terrain generation, scripts, input all run unchanged on the null device.
- **Mirror**: Wicked is left-handed; positions/normals and world matrices are mirrored in Z
  (`S*M*S`), which also turns clockwise front faces into Filament's counter-clockwise ones.
  Meshes (LOD 0 subsets), materials (`StLit.mat`: base colour x vertex colour x base colour map,
  roughness, metalness, emissive), the directional light as the sun (wicked intensity x 8000 lux
  against a sunny-16 exposure), the game camera, a flat sky and a constant SH ambient.
- **Terrain**: chunks are ordinary meshes; their layer blend (`ChunkData::blendmap_layers`,
  CPU) becomes vertex colour (each layer's base colour x its texture's average, stacked like the
  engine's shader). Chunk materials get no emissive - Wicked gives them strength 100 against a
  black virtual-texture emissive map Filament does not have.
- **Build facts**: Filament's headers need C++20 - only `render/hurricanes/*.cpp` compile as
  C++20. Filament's `zstd` / `meshoptimizer` are not linked (the engine has both). Built from
  PowerShell: Filament refuses MSYS2 (Git Bash sets `MSYSTEM`). Exe +3.3 MB (17.2 -> 20.5 MB).
- **2D** (`render/hurricanes/HurricanesUI.*`, the Overlay): Draw2D, RmlUi and ImGui all go into
  ONE triangle batch per frame, drawn as one renderable (a primitive per texture/scissor change,
  ordered by blendOrder) in an orthographic, no-post-processing view over the 3D with
  `materials/StUi.mat`. Rounded corners are a CPU fan, gradients vertex colour, text stb_truetype
  atlases (Latin-1) with Liberation Sans as font 0. ImGui's font atlas is re-made on Filament;
  ImGui images the overlay did not make (the editor's Wicked viewports) are skipped. Verified:
  DevUI, the vehicle HUD, the test UI (panel, buttons, toggle, slider, text) and the RmlUi page
  look like Detroit's; the About box says Hurricanes / Petrol.
- **UV origin**: Filament samples v bottom-up; image rows are uploaded top first, so both
  materials flip v (3D textures were upside down until then).
- **Not yet**: debug shapes, skinning, point/spot lights, weather/sky, DDS/KTX textures (stb
  only: PNG/JPG/TGA/BMP), particles, live terrain edits (chunk meshes cached), transparency, UI
  world canvases (no render targets), text shadow softness/bolden, `FromResource` (a Wicked
  resource has no pixels on the null device unless it kept its file - the About icon and asset
  thumbnails are blank), frame-time numbers (the fps readout is Wicked's loop, not Filament's
  presents), and the Mali tablet.

## Open questions

- Should Hurricanes run the same physics / vehicle model at full fidelity, or a cheaper
  vehicle model on low-end CPUs (the A51 benchmark decides)?
- Is Steam Audio part of Petrol (prebuilt lib is 4 KB-page aligned - fails on 16 KB devices),
  or does Petrol use OpenAL panning only?
- One APK with runtime fallback, or separate store listings per edition?
- Caterpillar "high precision": double-precision simulation only, or rendering too?
- Filament (DRAFT): vendored as a submodule built from source, or prebuilt release libraries
  per platform (faster builds, but `matc` / `cmgen` must match the library version)?
- Filament: does it build and link under `/EHsc- /GR-`, or does it need a wrapper boundary?
- Filament: which Filament version to pin, and who bumps it (same policy as RmlUi / Taipan)?
- ~~Does the editor ever run on Hurricanes?~~ **Decided 2026-10-20: no. Hurricanes is play-only**;
  authoring and development are Detroit's. A Hurricanes run refuses editor mode (F2, `--edit`,
  the bridge) with a log line; DevUI windows (backlog, graphics, scene manager, ...) still work.
  The editor's viewports, template preview and UI Editor stay Detroit render paths.
- Streaming from a Petrol server (BHS): needed, or is cloud always Detroit?
