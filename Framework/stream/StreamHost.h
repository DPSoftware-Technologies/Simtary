#pragma once
// StreamHost - the engine side of Blackhawk Streaming (Simtary/CLOUD.md, Simtary/bhs/PROTOCOL.md).
//
//   Milistry.exe --scene=vehicletest --stream[=port]
//       [--stream-size=1280x720] [--stream-fps=60] [--stream-kbps=6000]
//       [--stream-bind=127.0.0.1] [--stream-window]
//
// The process renders as usual (DirectX 12, a window that is hidden unless --stream-window), and
// every finished frame is copied into a shared GPU texture and handed to bhs.dll, which encodes it
// (hardware H.264) and sends it to the browser that opened bhs/web/index.html. Keys, mouse and
// wheel come back and are fed into wi::input / ImGui as if they came from a local device.
//
// While no viewer is connected nothing is copied or encoded. Latency note: the frame handed over is
// two frames old (a ring of three shared textures), so the GPU is certainly done with it.
//
// Built only with the prebuilt bhs install (SIMTARY_HAS_BHS, tools: bhs/tools/build_bhs.ps1);
// otherwise --stream reports that it is missing. Windows / DirectX 12 for now.

#include <cstdint>
#include <string>

namespace wi { class Application; }

namespace st::stream {

struct Options {
    bool enabled = false;
    uint16_t port = 9000;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 60;
    uint32_t kbps = 6000;
    std::string bind = "127.0.0.1";
    bool showWindow = false;
};

class StreamHost {
public:
    static StreamHost& Get ();

    // Reads the --stream* arguments. Call once, before the window is created (it decides the
    // window's size and whether it is shown).
    void ParseCommandLine ();
    bool Requested () const { return options_.enabled; }
    const Options& GetOptions () const { return options_; }

    // After the graphics device exists. Starts bhs and hooks the frame.
    void Initialize (wi::Application& app);
    // Main thread, once a frame: applies the viewer's queued input.
    void Update ();
    void Shutdown ();

    bool IsRunning () const { return running_; }
    bool HasViewer () const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    Options options_;
    bool running_ = false;
};

} // namespace st::stream
