# Simtary editions and renderer architecture

Status: **design only (2026-10-14).** Nothing below is implemented except the Detroit renderer
(the current engine) and the Gumin safeguards already made for Mali-G68 (PORTING-ANDROID.md).

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
│ Detroit renderer              │ Hurricanes renderer (new)             │
│  = current wi::renderer +     │  forward, no bindless,                │
│    RenderPath3D               │  OpenGL ES 3.2 / desktop GL 4.3       │
│  Vulkan 1.3 / DX12, bindless  │  small hand-written shader set        │
│  Gumin = + mobile safeguards  │                                       │
└───────────────────────────────┴──────────────────────────────────────┘
```

Rules that keep the split clean:

1. **Core and Framework never include a renderer header.** Anything that draws today from
   Framework (projector, laser, lens flare, ImGui backend, terrain chunk rendering, the editor
   viewports) goes through the render interface, or is marked Detroit-only.
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

## Hurricanes renderer (Petrol)

Target: Mali-G52/G72 class, Adreno 5xx/6xx, Intel/AMD integrated GPUs. 30 fps at 720p internal.

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
same interface.

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
| HURRICANES | Hurricanes | glsl_es | OFF | low |
| CATERPILLAR | Detroit (+ double precision paths, multicore sim) | hlsl6 + spirv | ON | ultra |

Presets: `win_x86-64` (Detroit), `android_arm64` (Gumin), `android_arm64-petrol` and
`win_x86-64-petrol` (Hurricanes). A runtime fallback is allowed on Android: start Gumin, and if
the device lacks bindless / Vulkan 1.3, start Hurricanes instead - one APK for both.

## Phases

| # | Work | Size |
|---|---|---|
| 1 | Render interface + DetroitRenderer as a wrapper; move Framework drawing (ImGui backend, lens flare, projector, laser, editor viewports) behind it. No behaviour change. | 1-2 weeks |
| 2 | GPU resources out of components (mesh / material / texture handles owned by the renderer). Biggest refactor; Detroit keeps working throughout. | 2-3 weeks |
| 3 | HurricanesRenderer on desktop GL 4.3 first (fast to debug), Scene1 + vehicletest drawn. | 3-4 weeks |
| 4 | Same renderer on GL ES 3.2 on the A51 (Mali-G72) and the tablet; ASTC; touch controls. | 2 weeks |
| 5 | Edition presets, runtime fallback, per-edition quality tiers. | 1 week |

Phase 1 alone is worth doing even before Hurricanes: it is what makes the Mali work, the
editor, and the dev bridge stop reaching into `wi::renderer` from everywhere.

## Open questions

- Should Hurricanes run the same physics / vehicle model at full fidelity, or a cheaper
  vehicle model on low-end CPUs (the A51 benchmark decides)?
- Is Steam Audio part of Petrol (prebuilt lib is 4 KB-page aligned - fails on 16 KB devices),
  or does Petrol use OpenAL panning only?
- One APK with runtime fallback, or separate store listings per edition?
- Caterpillar "high precision": double-precision simulation only, or rendering too?
