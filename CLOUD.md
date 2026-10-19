# Blackhawk Streaming (BHS) - design

Status: design only (2026-10-18). Nothing built. Written from the planning conversation; the
open questions at the end need answers before code starts.

**BHS = Blackhawk Streaming**: server-side rendering streamed to thin clients, in the same space as
Unreal's Pixel Streaming and Unity's Render Streaming, but lightweight, with an **open, versioned
control protocol** so any cloud provider can build its own service on it. It is the first user of the
"Unified Engine" idea (`Milistry/docs/FEATURES.md`, side ideas) and was planned as a standalone
framework before it became part of the Simtary platform diagram.

## Goals

- Many sessions per node: **target 3 on an RTX 5050 laptop, 12 on an RTX 5090** (a plan, not a
  measurement - see Capacity).
- Lightweight: a session is a headless engine process (~385 MB today) plus render targets, not a
  multi-GB editor-class process.
- **Open standard control layer**; media rides on WebRTC (already IETF / W3C).
- Standalone: BHS never includes Simtary headers. Any engine, or a tiny test program, can host it.
- Browser as the first client (no install). Native clients via Moonlight/Sunshine stay a separate,
  optional route (Sunshine is GPL-3.0, so it runs as its own process and is never linked in).

## Non-goals (for now)

- A new video / transport protocol. A custom loss-recovery stack is a lot of work and browsers could
  not use it.
- VNC / RDP / SPICE (desktop protocols; poor fit for 60 fps 3D; SPICE needs a VM).
- Hosting many simulations inside ONE process (the engine keeps process-global state).
- Running without a GPU in production (software encode exists for tests only).

## Architecture

```
 browser / thin client
        |  WebRTC (video, Opus audio, data channel for input)
        v
 gateway ----------------------------- auth, signalling, TURN config, placement
        |  node API (REST/JSON)
        v
 node agent  (one per machine) -------- capacity, create / destroy sessions, metrics
        |  spawns
        v
 session = engine process, offscreen render -> hardware encode -> BHS transport
```

Two session shapes, both needed:
- **different simulations:** one engine process per session (isolated; a crash loses one session)
- **one simulation, several viewers** (instructor + trainees, spectators): ONE process, several
  cameras from the same world (the multi-camera render already exists from the VR work); each viewer
  gets its own encoder

## The standalone boundary

BHS is a library with a **C interface** (no exceptions or runtime types cross it) plus separate
programs. The host engine supplies:
- video: each frame as a GPU texture handle (D3D12 / Vulkan / Metal), size, timestamp
- audio: PCM blocks
- an input-event callback (keys, mouse, gamepad), fed into the host's own input system

BHS provides sessions, hardware encode, WebRTC, signalling, the node agent and capacity reporting.
Simtary's side is one small adapter (`Framework/stream`). Proof of "standalone": a tiny program that
streams a test pattern or a spinning cube with no engine at all.

Layout while young: `bhs/` inside the Simtary repository with its own build target and no Simtary
includes; extraction to its own repository later is a copy.

## The open protocol (what providers customise)

Specified in a versioned document (Apache-2.0 or MIT) with a reference implementation and a
conformance test suite:

1. **Signalling** - JSON over WebSocket: offer / answer / ICE candidates, session attach, errors.
2. **Data channel** - binary or JSON frames for input, quality requests, clipboard, and app messages.
3. **Node API** - REST/JSON: create / destroy session, list sessions, capacity, health, metrics.
4. **Provider hooks** - authentication, usage metering (for billing), placement policy, TURN server
   settings, web-client branding.

Reference: Epic's published Pixel Streaming infrastructure (signalling server, matchmaker, SFU, browser
player library) - to be studied. Open question whether BHS should speak the same signalling protocol so
Epic's browser player can be reused; needs a check of its licence and current protocol version first.

## Process modes

| mode | GPU | window | used for |
|---|---|---|---|
| normal | real | yes | a player on their own PC |
| headless (`--server`, built) | none (`GraphicsDevice_Null`) | no | dedicated multiplayer server |
| **offscreen** (new, BHS) | real | no | a streaming session |

Offscreen renders into a texture for the encoder instead of presenting. DX12 needs no window; the Vulkan
device in this engine may want a hidden one (check when building). There is no sound card on a node, so
the audio mix is captured inside the engine (the mix thread already exists) rather than played.

## Accounts, licences, metering (BHS does not own accounts)

BHS asks the provider; it never stores credentials or money.

1. **Identity** - the provider's login (OAuth2 / OpenID Connect) issues a signed token (JWT); the gateway
   only verifies the signature. BHS sees an opaque user id, nothing personal.
2. **Entitlements** - hook `authorize(user, app, params)` -> allow / deny + limits (session length,
   resolution, frame rate, which scenarios).
3. **Licence** - in streaming the app binary never reaches the user, so "may this account start a
   session" IS the licence check. (Installed offline copies, if ever: a signed licence file, Ed25519,
   checked offline; never fully unbreakable.)
4. **Metering** - events `session_started` / `session_ended` / usage (GPU-seconds, bytes) go to the
   provider via webhook or event stream; the provider bills.

Spec hooks: `authenticate`, `authorize`, `on_session_event`. The reference implementation uses a local
token file for development; real providers plug in their own. In `st::net`, the Hello message grows an
account token so the server can ban and identify per account instead of per name.

Personal data: keep as little as possible (the provider's login keeps it; BHS keeps an opaque id).
Laws such as Thailand's PDPA and the GDPR, and any age rules, apply once real accounts exist - check
with someone qualified before launch.

## Session start: cold vs warm

- **Cold start** = process + engine init + GPU device + shader / pipeline warm-up + scene load + terrain
  generation. Probably seconds to tens of seconds on a real GPU (not measured yet; the headless server's
  engine init is ~64 ms, the scene a few seconds).
- **Warm pool**: sessions started in advance with the scene loaded, no player in the world and no encoder
  running. A connect attaches an encoder and spawns the player (the same order as Hello -> Welcome -> spawn
  in `st::net`, so the world-first model already exists). Idle warm sessions still hold RAM / VRAM and count
  against the node's capacity.
- Plan: (1) stream a loading screen (with progress) the moment someone connects to a cold session;
  (2) keep a small warm pool per popular scenario, sized from node load; (3) a world with several viewers
  attaches instantly; (4) per-user state (appearance, loadout, licence limits) is applied at attach, never at
  load, so any warm session can serve anyone.
- Offscreen mode needs its own loading screen drawn inside the render (the native loading window cannot
  exist without a desktop) so it can be streamed.
- Measure the real cold-start time of the vehicle scene on this PC once offscreen mode exists; that number
  sets the warm-pool size.

## Media path

- Offscreen render into a GPU texture; no window, no display.
- Encode straight from that texture (no CPU copy). Windows first step: **Media Foundation hardware
  H.264 MFT** (works on NVIDIA / AMD / Intel, no vendor SDK). Then NVENC (lowest latency, NVIDIA SDK
  licence terms apply), VAAPI on Linux AMD / Intel, Vulkan Video later. AV1 where hardware has it.
  Avoid software x264 (GPL, patents).
- Audio: capture the mix, Opus.
- Transport: WebRTC; **libdatachannel** (MPL-2.0, small) is the first candidate. Google libwebrtc is
  too large to start with.

## Capacity (measured, not configured)

The node agent benchmarks on start-up: render time, encode time and VRAM per session, and reports
how many sessions fit at each quality level (resolution x frame rate). The targets above become a
result to check.

Known limits to plan around:
- GeForce drivers cap concurrent hardware-encode sessions (the number has grown with driver versions;
  check the current one). A 12-session target on an RTX 5090 may hit that cap before the GPU runs out.
  Datacenter cards have no such cap.
- GPU render time is shared across sessions: quality settings must scale per session. VRAM is shared too
  (8 GB on the 5050).
- Software encode: tests / a VPS without a GPU only.

## Milestones

0. ~~Sunshine / Moonlight baseline~~ - dropped (2026-10-19). BHS has its own native protocol;
   `bhs/PROTOCOL.md` is the draft spec. Latency is measured on BHS itself from milestone 2.
1. **DONE 2026-10-19** - standalone test program `bhs/tools/encode_test` (`bhs_encode_test`, Windows,
   Media Foundation): GPU-drawn test pattern -> D3D11 texture -> hardware H.264 -> MP4, no CPU copy.
   Build: `cmake -S Simtary/bhs -B build/bhs -G "Visual Studio 18 2026" -A x64`. Measured on this PC
   (RTX 5050 laptop, driver 616.92, 1080p, 8 Mbps): 1 session 322 fps (5.4x realtime); 3 sessions 527 fps
   total; 6 sessions 558 fps; 12 sessions 598 fps (0.8x realtime each = 48 fps). No concurrent-session
   cap was hit up to 12. Encode alone therefore fits ~9 sessions at 1080p60 or 12 at 1080p30 / 720p60 on
   this GPU; the engine's own render time comes on top and is NOT measured yet. Not verified which of the
   three listed encoders (NVIDIA / AMD x2) did the work - confirm before quoting the numbers.
2. **DONE 2026-10-19** - `bhs/tools/stream_test` (`bhs_stream_test`) + `bhs/web/index.html`: GPU test
   pattern -> BGRA to NV12 on the GPU (D3D11 video processor) -> NVIDIA H.264 encoder MFT (picked by the
   GPU's LUID, async, no B-frames, CBR) -> libdatachannel RTP -> browser (Chrome / Edge 155 verified).
   Signalling is `PROTOCOL.md` section 1 (hello / welcome / offer / answer / candidate / error, a second
   viewer gets `capacity_full`); data channels `input` (unordered, no retransmit) and `control` (stats
   1 Hz). Mouse and keys come back and move a crosshair and a box in the picture. Measured: 1280x720 @ 60
   fps, ~6 Mbps, convert + encode 0.7-1.0 ms per frame, 60 fps delivered. Latency and a second GPU
   vendor are NOT measured. Lessons: (a) Mbed TLS cannot be used - the browser's DTLS handshake fails
   with "message could not be parsed" (suspected fragmented ClientHello; not proven); the build uses
   OpenSSL 3 from msys2 via `bhs/tools/setup_openssl_msys.ps1`. (b) libdatachannel's WebSocketServer hands
   over the only reference to a client socket: keep it or it closes right after the handshake. (c) The
   hardware encoder MFT is async and needs `MFTEnum2` + `MFT_ENUM_ADAPTER_LUID` to pick the right GPU.
3. **DONE 2026-10-19** - Simtary adapter. `bhs.dll` (C interface `bhs/include/bhs.h`, built and installed
   once by `bhs/tools/build_bhs.ps1` into `deps/bhs-install`, imported by CMake as `SimtaryBHS`) and
   `Framework/stream/StreamHost`: `Milistry.exe --scene=vehicletest --stream[=port]` renders in a HIDDEN
   window of exactly the stream size (DirectX 12), copies each finished backbuffer into a ring of three
   shared textures (engine hook `Application::onFrameComposed`), hands the oldest to BHS (two frames old,
   so the GPU is surely done; no fence yet), and feeds the viewer's keys / mouse / wheel back through
   `wi::input::InjectButton` and ImGui's input events. Verified in Chrome: the vehicle scene with its
   HUD at 720p, 58 fps delivered (engine 60), W drives the vehicle, right-drag orbits the chase camera.
   Lessons: (a) D3D11 can only open a D3D12 shared texture that has RENDER_TARGET in its bind flags
   (`bhs/tools/share_test` prints the matrix); (b) the input gate treated the hidden window as
   unfocused and read nothing, and the display standby cap held an unfocused window to 30 fps - both now
   count a connected stream viewer as focus. The same standby cap is a likely cause of the VR 30 fps
   (not re-tested). Not done: gamepad / touch input, fence-based sync (the two-frame delay is added
   latency), Vulkan, Linux, resize, per-viewer cameras, loading screen while the scene loads.
4. Node agent running several sessions; measure real capacity on this machine.
5. Gateway: signalling, auth, placement, TURN config.
6. Multi-view: several cameras of one world.
7. Publish the protocol spec + conformance tests.

## No GPU, naming, voice (decided 2026-10-19)

- **No GPU = no streaming.** The node agent checks for a usable GPU and a hardware encoder at start-up;
  without them it reports capacity 0 and answers `create_session` with `no_gpu`. No software-render
  fallback. Such a machine can still run a dedicated server.
- **Names:** *dedicated server* (headless, no GPU), *integrated server* (world + local player),
  *streaming session* (BHS, real GPU, offscreen). *World host* is the code they share.
- **Packages:** the dedicated server build is small (`SIMTARY_NO_SHADERS`, Debian 11 chroot). The BHS node
  needs the full build with shaders, so a newer build chroot (libdxcompiler needs a newer libstdc++).
- **Voice:** the protocol reserves a send+receive audio line and a host `on_audio_in` callback; enabled per
  account through `limits.voice`; not recorded by default. Built after the first browser session works.

## Open questions

- What are GEPA, Caterpillar and Gurutra in the platform diagram, and what do the JLTV4 and StageSim
  boxes contain?
- Do the three "Core" names (HMMWV4, JLTV4, Blackhawk4) change now that BHS is the framework name?
- First deployment: local test on this PC, or a real cloud server (a GPU-less VPS cannot hardware-encode)?
- ~~Signalling: Epic-compatible, or BHS's own?~~ Decided: BHS's own, see `bhs/PROTOCOL.md`.
