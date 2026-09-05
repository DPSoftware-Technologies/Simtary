#include "display/DisplaySettings.h"

#include "Simtary.h"
#include "imgui.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>

namespace st {

std::string DisplayMode::Label() const {
    char buf[64];
    if (refreshRate > 0)
        snprintf(buf, sizeof(buf), "%d x %d @ %d Hz", width, height, refreshRate);
    else
        snprintf(buf, sizeof(buf), "%d x %d", width, height);
    return buf;
}

// enumeration
void DisplaySettings::Refresh() {
    displayNames_.clear();

    const int count = SDL_GetNumVideoDisplays();
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(i);
        char buf[128];
        snprintf(buf, sizeof(buf), "%d: %s", i + 1, name ? name : "Display");
        displayNames_.push_back(buf);
    }
    if (displayNames_.empty())
        displayNames_.push_back("1: Display");

    if (displayIndex >= (int)displayNames_.size())
        displayIndex = 0;

    modesForDisplay_ = -1;   // force a re-enumerate on next use
    EnumerateModes();
}

void DisplaySettings::EnumerateModes() {
    if (modesForDisplay_ == displayIndex)
        return;

    modes_.clear();

    const int count = SDL_GetNumDisplayModes(displayIndex);
    for (int i = 0; i < count; ++i) {
        SDL_DisplayMode m{};
        if (SDL_GetDisplayMode(displayIndex, i, &m) != 0)
            continue;
        // SDL lists one entry per pixel format; we only care about geometry + rate,
        // so collapse the duplicates.
        DisplayMode dm{ m.w, m.h, m.refresh_rate };
        if (std::find(modes_.begin(), modes_.end(), dm) == modes_.end())
            modes_.push_back(dm);
    }

    // Largest first, which is the order players expect in a resolution dropdown.
    std::sort(modes_.begin(), modes_.end(), [](const DisplayMode& a, const DisplayMode& b) {
        if (a.width  != b.width)  return a.width  > b.width;
        if (a.height != b.height) return a.height > b.height;
        return a.refreshRate > b.refreshRate;
    });

    modesForDisplay_ = displayIndex;
}

// live state
void DisplaySettings::ReadFromWindow(wi::Application& app) {
    SDL_Window* win = app.window;
    if (win == nullptr)
        return;

    displayIndex = SDL_GetWindowDisplayIndex(win);
    if (displayIndex < 0) displayIndex = 0;

    // SDL_WINDOW_FULLSCREEN_DESKTOP is FULLSCREEN plus one extra bit, so test the
    // desktop flag in full FIRST - a plain `& SDL_WINDOW_FULLSCREEN` matches both.
    const Uint32 flags = SDL_GetWindowFlags(win);
    if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
        windowMode = WindowMode::Borderless;
    else if (flags & SDL_WINDOW_FULLSCREEN)
        windowMode = WindowMode::Fullscreen;
    else
        windowMode = WindowMode::Windowed;

    SDL_DisplayMode m{};
    if (SDL_GetWindowDisplayMode(win, &m) == 0)
        resolution.refreshRate = m.refresh_rate;

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    resolution.width  = w;
    resolution.height = h;

    uint32_t rw = 0, rh = 0;
    app.GetRenderResolution(rw, rh);
    renderScale = (w > 0 && rw > 0) ? (float)rw / (float)w : 1.0f;

    targetFrameRate = app.getTargetFrameRate();

    appliedMode_       = windowMode;
    appliedDisplay_    = displayIndex;
    appliedResolution_ = resolution;
    appliedVsync_      = vsync;
    appliedLock_       = framerateLock;
    appliedTargetFps_  = targetFrameRate;
    appliedScale_      = renderScale;
}

void DisplaySettings::Init(wi::Application& app) {
    if (initialized_)
        return;
    Refresh();
    ReadFromWindow(app);
    initialized_ = true;
}

bool DisplaySettings::Dirty() const {
    return windowMode      != appliedMode_
        || displayIndex    != appliedDisplay_
        || resolution      != appliedResolution_
        || vsync           != appliedVsync_
        || framerateLock   != appliedLock_
        || targetFrameRate != appliedTargetFps_
        || renderScale     != appliedScale_;
}

// -- standby --------------------------------------------------------------------
// The engine has its own inactive path (wiApplication.cpp: `if (!is_window_active
// && !alwaysactive)` sleeps and skips the whole frame), but nothing in this fork ever
// assigns is_window_active, so it never triggers -- and a hard stop would leave the
// window unrepainted anyway. Capping the frame rate keeps the window alive while
// still handing the GPU back.
void DisplaySettings::UpdateStandby(wi::Application& app, float dt) {
    SDL_Window* win = app.window;
    if (win == nullptr)
        return;

    idleTimer_ += dt;

    const bool focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;

    // Unfocused wins over idle: alt-tabbed is the stronger signal, and a player who
    // alt-tabs is idle by definition.
    int target = 0;   // 0 = not in standby
    if (standbyOnUnfocus && !focused)
        target = unfocusedFps;
    else if (standbyOnIdle && idleTimer_ >= idleSeconds)
        target = idleFps;

    if (target > 0) {
        if (!standbyActive_ || target != standbyTarget_) {
            standbyActive_ = true;
            standbyTarget_ = target;
            app.setFrameRateLock(true);
            app.setTargetFrameRate((float)target);
        }
        return;
    }

    if (standbyActive_) {
        standbyActive_ = false;
        standbyTarget_ = 0;
        // Restore what was last APPLIED, not the pending edits: the player may be
        // mid-edit in the options panel, and coming back from standby must not
        // silently commit that.
        app.setFrameRateLock(appliedLock_);
        app.setTargetFrameRate(appliedTargetFps_);
    }
}

// apply
void DisplaySettings::Apply(wi::Application& app) {
    SDL_Window* win = app.window;
    if (win == nullptr)
        return;

    // Drop out of fullscreen first: SDL will not resize or move a fullscreen window,
    // so every path below starts from a plain window and re-enters if needed.
    SDL_SetWindowFullscreen(win, 0);

    const int centered = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);

    switch (windowMode) {
    case WindowMode::Windowed:
        SDL_SetWindowSize(win, resolution.width, resolution.height);
        SDL_SetWindowPosition(win, centered, centered);
        SDL_SetWindowBordered(win, SDL_TRUE);
        break;

    case WindowMode::Borderless:
        // Move to the target display first - FULLSCREEN_DESKTOP takes over whichever
        // display the window is currently on.
        SDL_SetWindowPosition(win, centered, centered);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        break;

    case WindowMode::Fullscreen: {
        SDL_SetWindowPosition(win, centered, centered);
        // Ask for the closest supported mode rather than trusting the combo blindly:
        // the mode list can go stale if a display was unplugged since Refresh().
        SDL_DisplayMode want{};
        want.w = resolution.width;
        want.h = resolution.height;
        want.refresh_rate = resolution.refreshRate;
        SDL_DisplayMode got{};
        if (SDL_GetClosestDisplayMode(displayIndex, &want, &got) != nullptr)
            SDL_SetWindowDisplayMode(win, &got);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
        break;
    }
    }

    // Render resolution: 1.0 means "follow the window", anything else pins a fixed
    // render target the engine upscales from.
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (renderScale >= 0.999f && renderScale <= 1.001f) {
        app.SetRenderResolution(0, 0);
    } else {
        app.SetRenderResolution(
            (uint32_t)std::max(1, (int)(w * renderScale)),
            (uint32_t)std::max(1, (int)(h * renderScale)));
    }

    wi::eventhandler::SetVSync(vsync);
    app.setFrameRateLock(framerateLock);
    app.setTargetFrameRate(targetFrameRate);
    // Drop any standby cap; UpdateStandby() re-evaluates on the next frame and will
    // re-apply it if the window is still unfocused or idle.
    standbyActive_ = false;
    standbyTarget_ = 0;

    // Rebuild the swapchain against the new window size. st::Run does the same on a
    // resize event; doing it here means the change lands on this frame instead of
    // waiting for SDL to deliver one.
    app.SetWindow(win);

    appliedMode_       = windowMode;
    appliedDisplay_    = displayIndex;
    appliedResolution_ = resolution;
    appliedVsync_      = vsync;
    appliedLock_       = framerateLock;
    appliedTargetFps_  = targetFrameRate;
    appliedScale_      = renderScale;
}

// persistence
void DisplaySettings::SaveTo(nbt::Tag& out) const {
    out.putInt  ("windowMode",      (int)appliedMode_);
    out.putInt  ("display",         appliedDisplay_);
    out.putInt  ("width",           appliedResolution_.width);
    out.putInt  ("height",          appliedResolution_.height);
    out.putInt  ("refreshRate",     appliedResolution_.refreshRate);
    out.putBool ("vsync",           appliedVsync_);
    out.putBool ("framerateLock",   appliedLock_);
    out.putFloat("targetFrameRate", appliedTargetFps_);
    out.putFloat("renderScale",     appliedScale_);
    // Standby applies live, so persist the live values rather than an applied_ copy.
    out.putBool ("standbyOnUnfocus", standbyOnUnfocus);
    out.putInt  ("unfocusedFps",     unfocusedFps);
    out.putBool ("standbyOnIdle",    standbyOnIdle);
    out.putFloat("idleSeconds",      idleSeconds);
    out.putInt  ("idleFps",          idleFps);
}

void DisplaySettings::LoadFrom(const nbt::Tag& in) {
    windowMode             = (WindowMode)in.getInt("windowMode", (int)windowMode);
    displayIndex           = in.getInt  ("display",         displayIndex);
    resolution.width       = in.getInt  ("width",           resolution.width);
    resolution.height      = in.getInt  ("height",          resolution.height);
    resolution.refreshRate = in.getInt  ("refreshRate",     resolution.refreshRate);
    vsync                  = in.getBool ("vsync",           vsync);
    framerateLock          = in.getBool ("framerateLock",   framerateLock);
    targetFrameRate        = in.getFloat("targetFrameRate", targetFrameRate);
    renderScale            = in.getFloat("renderScale",     renderScale);
    standbyOnUnfocus       = in.getBool ("standbyOnUnfocus", standbyOnUnfocus);
    unfocusedFps           = in.getInt  ("unfocusedFps",     unfocusedFps);
    standbyOnIdle          = in.getBool ("standbyOnIdle",    standbyOnIdle);
    idleSeconds            = in.getFloat("idleSeconds",      idleSeconds);
    idleFps                = in.getInt  ("idleFps",          idleFps);

    // A saved display that is no longer plugged in would leave the window on a
    // monitor that does not exist.
    if (displayIndex < 0 || displayIndex >= (int)displayNames_.size())
        displayIndex = 0;
    if (resolution.width < 320 || resolution.height < 240) {
        resolution.width  = 1280;
        resolution.height = 720;
    }
}

void DisplaySettings::LoadAndApply(const nbt::Tag& in, wi::Application& app) {
    Init(app);
    LoadFrom(in);
    Apply(app);
}

// GUI
void DisplaySettings::GUI(wi::Application& app) {
    EnumerateModes();

    ImGui::PushID("st_display");

    // Window mode
    const char* modeNames[] = { "Windowed", "Borderless Fullscreen", "Fullscreen" };
    int modeIdx = (int)windowMode;
    if (ImGui::Combo("Window Mode", &modeIdx, modeNames, IM_ARRAYSIZE(modeNames)))
        windowMode = (WindowMode)modeIdx;

    // Monitor
    if (displayNames_.size() > 1) {
        if (ImGui::BeginCombo("Monitor", displayNames_[displayIndex].c_str())) {
            for (int i = 0; i < (int)displayNames_.size(); ++i) {
                if (ImGui::Selectable(displayNames_[i].c_str(), i == displayIndex)) {
                    displayIndex = i;
                    EnumerateModes();
                }
            }
            ImGui::EndCombo();
        }
    }

    // Resolution
    // Borderless always takes the desktop resolution, so the dropdown would be a lie.
    ImGui::BeginDisabled(windowMode == WindowMode::Borderless);
    if (ImGui::BeginCombo("Resolution", resolution.Label().c_str())) {
        // Only exclusive fullscreen drives the refresh rate, so windowed mode shows
        // one entry per distinct size instead of one per size-and-rate pair.
        const bool exclusive = (windowMode == WindowMode::Fullscreen);
        int lastW = -1, lastH = -1;
        for (const DisplayMode& m : modes_) {
            if (!exclusive && m.width == lastW && m.height == lastH)
                continue;   // modes_ is sorted, so duplicates of a size are adjacent
            lastW = m.width;
            lastH = m.height;

            DisplayMode entry = m;
            if (!exclusive) entry.refreshRate = 0;

            const bool selected = entry.width == resolution.width
                               && entry.height == resolution.height
                               && (!exclusive || entry.refreshRate == resolution.refreshRate);
            if (ImGui::Selectable(entry.Label().c_str(), selected))
                resolution = entry;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (windowMode == WindowMode::Borderless)
        ImGui::TextDisabled("Borderless uses the desktop resolution. Use Render Scale below.");

    ImGui::Spacing();
    ImGui::SeparatorText("Frame Rate");

    ImGui::Checkbox("V-Sync", &vsync);
    ImGui::Checkbox("Limit Frame Rate", &framerateLock);
    ImGui::BeginDisabled(!framerateLock);
    ImGui::SliderFloat("Target FPS", &targetFrameRate, 30.0f, 360.0f, "%.0f");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText("Render Scale");

    ImGui::SliderFloat("Scale", &renderScale, 0.25f, 1.0f, "%.2f");
    {
        int w = 0, h = 0;
        if (app.window) SDL_GetWindowSize(app.window, &w, &h);
        ImGui::TextDisabled("Rendering at %d x %d, presented at %d x %d",
            (int)(w * renderScale), (int)(h * renderScale), w, h);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Standby");
    ImGui::TextDisabled("Applies immediately - Apply/Revert below do not affect these.");

    ImGui::Checkbox("Limit FPS when unfocused", &standbyOnUnfocus);
    ImGui::BeginDisabled(!standbyOnUnfocus);
    ImGui::SliderInt("Unfocused FPS", &unfocusedFps, 5, 120);
    ImGui::EndDisabled();

    ImGui::Checkbox("Limit FPS when idle", &standbyOnIdle);
    ImGui::BeginDisabled(!standbyOnIdle);
    ImGui::SliderFloat("Idle after", &idleSeconds, 5.0f, 600.0f, "%.0f s");
    ImGui::SliderInt("Idle FPS", &idleFps, 5, 120);
    ImGui::EndDisabled();

    if (standbyActive_)
        ImGui::Text("Standby active - capped at %d FPS", standbyTarget_);
    else if (standbyOnIdle)
        ImGui::TextDisabled("Idle for %.0f s", idleTimer_);

    ImGui::Spacing();
    ImGui::Separator();

    // Nothing above takes effect until Apply - a player changing resolution should
    // never have the window jump around while they scroll the dropdown.
    ImGui::BeginDisabled(!Dirty());
    if (ImGui::Button("Apply"))
        Apply(app);
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        ReadFromWindow(app);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Detect Displays"))
        Refresh();

    if (Dirty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(unapplied changes)");
    }

    ImGui::PopID();
}

} // namespace st
