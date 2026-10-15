#pragma once
// XRSystem - the OpenXR (VR) session, on the engine's own graphics device. Design and the
// step plan: Simtary/VR.md.
//
// Opt-in per run: --xr on the command line (which also selects DirectX 12). Without it - or
// in a build without the loader (SIMTARY_HAS_OPENXR, CMake option SIMTARY_ENABLE_OPENXR) -
// every call is a no-op and the runtime is never touched.
//
// Session: instance (XR_KHR_D3D12_enable) -> system (a head-mounted display; polled every
// 2 s while none is connected) -> D3D12 session on the engine's device + graphics queue ->
// LOCAL (seated) reference space -> one sRGB colour swapchain per eye -> session state
// machine (begin on READY, end on STOPPING, rebuild after a loss).
//
// One frame, spread over the engine's frame because each half needs a different moment:
//   Update()      st::App::Update      events; xrWaitFrame (paces the main loop to the
//                                      headset) + xrBeginFrame + xrLocateViews
//   RenderEyes()  st::App::Render      each eye: its own RenderPath3D + camera (the game
//                                      camera's position and heading + the head pose, the
//                                      headset's asymmetric FOV), then the result copied
//                                      into that eye's acquired swapchain image
//   EndFrame()    st::Run, after the   release the images + xrEndFrame with the projection
//                 frame was SUBMITTED  layer - the runtime waits on the queue, so the copy
//                                      must already be on it
// While a session runs the desktop v-sync is turned off by st::App (two blocking waits per
// frame halve the rate).
//
// Main thread only. Initialize() once the graphics device exists, Shutdown() before the
// device is destroyed. Dev build: bridge command `xr.status`.

#include <cstdint>
#include <functional>
#include <string>

namespace wi { class RenderPath3D; }
namespace wi::graphics { struct Texture; }

namespace st::xr {

struct XRStatus {
    bool compiled = false;        // built against the OpenXR loader
    bool requested = false;       // --xr was given
    bool instance = false;        // talking to a runtime
    bool system = false;          // a headset is connected
    bool session = false;         // session created on the engine's device
    bool running = false;         // between xrBeginSession and xrEndSession
    bool adapterMatches = true;   // the runtime wants the GPU the engine is running on
    std::string runtime;          // runtime name + version, e.g. "SteamVR/OpenXR 2.11.2"
    std::string systemName;       // the headset, as the runtime names it
    std::string state;            // session state: IDLE, READY, SYNCHRONIZED, VISIBLE, FOCUSED, ...
    std::string error;            // last failure; empty when all is well
    std::string swapchainFormat;  // what the eyes are submitted as
    uint32_t eyeWidth = 0, eyeHeight = 0;         // per-eye image size in use (recommended)
    uint32_t maxEyeWidth = 0, maxEyeHeight = 0;   // the most the runtime accepts
    uint64_t framesSubmitted = 0; // xrEndFrame calls
    uint64_t framesWithImage = 0; // ... of which carried the eyes (a projection layer)
};

class XRSystem {
public:
    static XRSystem& Get ();

    // appName is what the runtime shows (SteamVR's dashboard, the Link app list).
    void Initialize (const std::string& appName);
    void Update ();
    // configure: called on each eye's render path every frame before it renders, so the
    // eyes get the game's graphics settings (st::App mirrors its own path into it).
    void RenderEyes (float dt, const std::function<void(wi::RenderPath3D&)>& configure);
    void EndFrame ();
    void Shutdown ();

    // Make where the head is NOW the seat: its position and heading become the anchor's.
    // Done automatically on the first tracked frame of a session; F9 / bridge xr.recenter.
    void Recenter ();

    // The desktop window stopped rendering its own view (st::App, while a session runs):
    // the LEFT eye then does the scene's per-frame GPU work (particle simulation, probe and
    // decal refresh) that the game path otherwise does, and the right eye only draws.
    void SetEyesOwnScene (bool value) { eyesOwnScene_ = value; }
    // What the desktop shows instead: the left eye's output, or nullptr before one exists.
    const wi::graphics::Texture* MirrorTexture () const;

    // Dev: write both eye images (as sent to the headset) to <prefix>_L.png / _R.png after
    // the next submitted frame. Bridge: xr.capture {path}.
    void CaptureEyes (const std::string& prefix) { capturePrefix_ = prefix; }

    const XRStatus& Status () const { return status_; }
    bool IsRunning () const { return status_.running; }

private:
    struct Impl;                  // OpenXR handles; the headers stay inside XRSystem.cpp
    Impl* impl_ = nullptr;
    XRStatus status_;
    std::string capturePrefix_;
    bool eyesOwnScene_ = false;
};

} // namespace st::xr
