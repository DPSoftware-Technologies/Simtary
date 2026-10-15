# VR (OpenXR) - design

Status: **steps 1-3 done** (2026-10-15), seen in the headset by the user on SteamVR/OpenXR
2.17.10 (headset driver: iVRy), 1176x1320 per eye, R8G8B8A8_UNORM_SRGB swapchains. Each eye is
its own `RenderPath3D` + camera; while a session runs (outside editor mode) the game path is
neither updated nor rendered and the window mirrors the left eye. Recenter: automatic on the
first tracked frame, then F9 / bridge `xr.recenter`. Dev: `xr.capture {path}` writes both eye
images as sent.

Things that were wrong once and must stay right:
- `PreUpdate()` on an eye path BEFORE its camera is rewritten - it saves "previous camera" for
  TAA and the volumetric-cloud reprojection; saving the game camera there smeared the clouds
  into streaks and a blocky band above the horizon.
- SYNCHRONIZED is `shouldRender = false`: SteamVR keeps the app there (its own menu shown)
  until the headset is actually active, which on iVRy took ~2 minutes once.

Run it: `Milistry.exe --xr` (dev build: bridge `xr.status`). `--xr` picks DirectX 12 even when
the saved backend is Vulkan; an explicit `vulkan` argument still wins and VR stays off.

## Why OpenXR

OpenXR (Khronos) is the one API every current runtime speaks: SteamVR, Meta (Link on PC,
native on Quest), Pico, HTC, Varjo. OpenVR is SteamVR-only and legacy - Valve points new
work at OpenXR too. Quest standalone is Android and OpenXR-only, so the same code path
later serves the Android port (`PORTING-ANDROID.md`). It has DX12 and Vulkan bindings.

## v1 scope: HMD only

In:
- head-tracked stereo view, seated, from the vehicle's internal camera
  (`stChaseCamera` internal view is the anchor; the cab is a fixed frame, the comfortable case)
- recenter (key / gamepad button), desktop window mirrors the left eye
- Steam Audio listener follows the head
- DX12 only (the Windows default)

Out (later):
- controllers / hands - waits for the character system; input stays keyboard + gamepad
- chase view in VR, Vulkan, Quest standalone, single-pass multiview, foveated rendering,
  depth layer submission

## Pieces

**Library.** Khronos' prebuilt loader zip (`openxr_loader_windows-<ver>.zip`, Apache-2.0,
`SIMTARY_OPENXR_VERSION` = 1.1.63) fetched by `CMakeLists.txt` like Steam Audio, imported as
`SimtaryOpenXR` (defines `SIMTARY_HAS_OPENXR`), `openxr_loader.dll` staged next to the exe.
`SIMTARY_ENABLE_OPENXR` is ON on Windows x64: it costs one 2.6 MB DLL and is inert unless a run
passes `--xr`. The runtime is the user's (SteamVR / Meta Link) - nothing else ships.

**`Framework/xr/XRSystem` (`st::xr`).** Owns instance, system, session, reference space
(`LOCAL`, seated) and one colour swapchain per eye. Per frame:
`xrWaitFrame` -> `xrBeginFrame` -> `xrLocateViews` (two poses + two FOVs) -> render ->
`xrEndFrame` with a projection layer. Handles session state (idle / ready / focused /
stopping / loss pending) so the game keeps running when the headset is off.

**DX12 hookup** (`XR_KHR_D3D12_enable`). The session is created with the engine's
`ID3D12Device` and direct queue. Swapchain images come back as `ID3D12Resource`s that must
be wrapped as `wi::graphics::Texture` - engine work in `wiGraphicsDevice_DX12`
(wrap an external resource; check whether the fork already has a helper).

**Stereo.** v1: two `RenderPath3D`s (left / right), each with its own camera, each rendered
fully, final composite copied into that eye's swapchain image. Twice the CPU culling and
render cost, but each eye gets its own TAA / FSR history for free and no engine render
change is needed. Both paths use `setSceneUpdateExternal(true)` like the game path.

**Camera.** Eye world pose = internal-view anchor * recentred HMD eye pose. Projection from
`XrFovf`, which is ASYMMETRIC (four angles) - Wicked's `CameraComponent` builds a symmetric
FOV, so it needs a custom-projection path (to check / add, marked SIMTARY).
Reverse-Z stays internal: v1 submits colour only.

**Pacing.** `xrWaitFrame` sets the frame rate (90 Hz typical); desktop vsync off while a
session runs.

**Comfort.** In VR: no camera shake (explosions), no motion blur, no DOF, no lens flare, no
chromatic aberration, no `stChaseCamera` look / auto-recenter (the head IS the look).

## Steps

1. ~~Loader, CMake option, `XRSystem` instance + session on the DX12 device, log runtime
   name - no rendering~~ DONE. Engine: `GraphicsDevice_DX12::GetD3D12Device()` (SIMTARY).
2. ~~Swapchains~~ DONE: wrapped with `GraphicsDevice_DX12::WrapNativeTexture` (SIMTARY); the
   eye output is re-drawn into a UNORM target and copied into the sRGB image (bytes unchanged)
3. ~~Two render paths, asymmetric projection, HMD pose -> camera; desktop mirror~~ DONE
   (anchor = game camera position + HEADING only; v-sync off during a session)
4. Anchor to the vehicle internal view, recenter, listener on the head
5. Comfort switches, performance pass (resolution scale per eye, settings preset)

## Known after step 1

- The empty frame loop ran at **30 fps** on iVRy. Likely cause (not yet confirmed): `xrWaitFrame` (in `st::App::Update`) and the
  desktop v-sync both block, so the frame lands every other HMD interval. Step 3 turns desktop
  v-sync off while a session runs and moves wait/begin to the top of the frame, end after the
  eye renders.

## Risks

- external-resource wrap and resource states on the swapchain images (DX12 interop)
- asymmetric projection through Wicked's camera and everything that reads it (culling,
  shadows cascades, TAA jitter)
- 2x render cost at 90 Hz on the current settings
