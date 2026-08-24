#pragma once
// Window / video settings — the options a PLAYER expects: window mode, monitor,
// resolution, refresh rate, v-sync, frame cap and render scale.
//
// Deliberately NOT part of DevUI. Video options belong in a game's own settings menu,
// so this is a framework module with a Begin/End-free GUI() that drops into any
// window:
//
//     ImGui::Begin("Options");
//     Display().GUI(*this);          // from inside an st::App
//     ImGui::End();
//
// The framework's DevUI Graphics Settings window also renders it, in its "Display"
// tab, so the same knobs are reachable while developing.
//
// Edits are staged: changing a combo does nothing until Apply() is called (games want
// an explicit "Apply" + revert-on-timeout flow). st::App persists the applied state to
// options.stad under the "display" compound and re-applies it on the next launch.
//
// Window mode maps onto SDL like this:
//   Windowed    plain resizable window at `resolution`
//   Borderless  SDL_WINDOW_FULLSCREEN_DESKTOP — native desktop resolution, alt-tabs
//               instantly. Use renderScale/SetRenderResolution to render smaller.
//   Fullscreen  SDL_WINDOW_FULLSCREEN with an exclusive display mode (the only mode
//               where `resolution` and `refreshRate` change the actual signal)

#include "io/Nbt.h"

#include <string>
#include <vector>

namespace wi { class Application; }

namespace st {

enum class WindowMode {
    Windowed   = 0,
    Borderless = 1,   // borderless fullscreen at the desktop resolution
    Fullscreen = 2,   // exclusive fullscreen, changes the display mode
};

struct DisplayMode {
    int width       = 0;
    int height      = 0;
    int refreshRate = 0;   // Hz, 0 = unspecified / desktop default

    bool operator==(const DisplayMode& o) const {
        return width == o.width && height == o.height && refreshRate == o.refreshRate;
    }
    bool operator!=(const DisplayMode& o) const { return !(*this == o); }

    // "1920 x 1080 @ 144 Hz"
    std::string Label() const;
};

class DisplaySettings {
public:
    // Enumerate monitors + modes and seed the pending state from the live window.
    // Call once, after the window exists.
    void Init(wi::Application& app);
    // Re-enumerate monitors and modes (a display was plugged in or unplugged).
    void Refresh();

    // ── pending state, edited by GUI() ─────────────────────────────────────────
    WindowMode  windowMode   = WindowMode::Windowed;
    int         displayIndex = 0;
    DisplayMode resolution;                 // window size, or the exclusive mode
    bool        vsync        = true;
    bool        framerateLock   = false;
    float       targetFrameRate = 60.0f;
    // Render at a fraction of the output resolution and let the engine upscale.
    // 1.0 = native. Drives wi::Application::SetRenderResolution.
    float       renderScale  = 1.0f;

    // ── standby frame rate ─────────────────────────────────────────────────────
    // Drop the frame cap when nobody is watching: the window lost focus, or the
    // player has not touched anything for a while. Saves power and GPU on a machine
    // that is alt-tabbed or left sitting on a menu.
    //
    // Unlike the rest of this class these apply LIVE — UpdateStandby() reads them
    // every frame, so no Apply is needed and they are not part of Dirty().
    bool  standbyOnUnfocus = true;    // window does not have input focus
    int   unfocusedFps     = 30;
    bool  standbyOnIdle    = false;   // no input for idleSeconds
    float idleSeconds      = 60.0f;
    int   idleFps          = 30;

    // Per-frame standby check. st::App::Update calls this; it caps the frame rate
    // while unfocused or idle and restores the player's own cap when they come back.
    void UpdateStandby(wi::Application& app, float dt);
    // Real user input arrived — resets the idle timer. st::Run calls this from the
    // SDL event loop.
    void NotifyActivity() { idleTimer_ = 0.0f; }
    // Whether a standby cap is in force right now (for HUD / debug readouts).
    bool StandbyActive() const { return standbyActive_; }

    // Push the pending state to the window + engine, then rebuild the swapchain.
    void Apply(wi::Application& app);
    // Throw away pending edits and re-read what is actually live.
    void ReadFromWindow(wi::Application& app);
    // True while the pending state differs from what was last applied.
    bool Dirty() const;

    // ── persistence (options.stad, "display" compound) ─────────────────────────
    void SaveTo(nbt::Tag& out) const;
    void LoadFrom(const nbt::Tag& in);
    // LoadFrom + Apply, for startup.
    void LoadAndApply(const nbt::Tag& in, wi::Application& app);

    // ImGui widgets for the whole panel, with no Begin/End of its own. Draws the
    // Apply/Revert buttons too, since nothing takes effect without them.
    void GUI(wi::Application& app);

    // Monitor names, indexed by displayIndex.
    const std::vector<std::string>& Displays() const { return displayNames_; }
    // Modes available on displayIndex, deduplicated and sorted.
    const std::vector<DisplayMode>& Modes() const { return modes_; }

private:
    void EnumerateModes();

    std::vector<std::string> displayNames_;
    std::vector<DisplayMode> modes_;
    int  modesForDisplay_ = -1;   // which display modes_ was built for

    // Last applied values, for Dirty() and Revert.
    WindowMode  appliedMode_       = WindowMode::Windowed;
    int         appliedDisplay_    = 0;
    DisplayMode appliedResolution_;
    bool        appliedVsync_      = true;
    bool        appliedLock_       = false;
    float       appliedTargetFps_  = 60.0f;
    float       appliedScale_      = 1.0f;
    bool        initialized_       = false;

    // Standby runtime state.
    float idleTimer_     = 0.0f;
    bool  standbyActive_ = false;
    int   standbyTarget_ = 0;
};

} // namespace st
