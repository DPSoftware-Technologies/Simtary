#include "scene/PlayControl.h"

#include "imgui.h"
#include "wiPhysics.h"

namespace st {

const char* ToString (PlayState state) {
    switch (state) {
        case PlayState::Playing:          return "Playing";
        case PlayState::PlayingNoPhysics: return "Playing (no physics)";
        case PlayState::Paused:           return "Paused";
    }
    return "?";
}

PlayControl& PlayControl::Get () {
    static PlayControl instance;
    return instance;
}

void PlayControl::SetState (PlayState state) {
    if (state != PlayState::Paused)
        lastPlay_ = state;
    state_     = state;
    stepsLeft_ = 0;   // a state change ends a step that has not been taken yet
}

void PlayControl::TogglePause () {
    SetState(state_ == PlayState::Paused ? lastPlay_ : PlayState::Paused);
}

void PlayControl::Reset () {
    sceneTime_   = 0.0;
    sceneFrames_ = 0;
    stepsLeft_   = 0;
    SetState(PlayState::Playing);
    if (resetHook_) resetHook_();
}

void PlayControl::Step (int frames) {
    if (frames <= 0) return;
    // A step is only meaningful from a stop. Asking for one while the scene is running
    // means "pause here, then take a frame", which is what the button does when it is
    // pressed without pausing first.
    if (state_ != PlayState::Paused) SetState(PlayState::Paused);
    stepsLeft_ += frames;
}

float PlayControl::Apply (float realDelta) {
    realDelta_ = realDelta;

    float delta = 0.0f;
    bool  physics = false;

    switch (state_) {
        case PlayState::Playing:
            delta   = realDelta * timeScale;
            physics = true;
            break;
        case PlayState::PlayingNoPhysics:
            delta   = realDelta * timeScale;
            physics = false;
            break;
        case PlayState::Paused:
            if (stepsLeft_ > 0) {
                // A stepped frame is a whole frame: fixed length, physics on, so what it
                // shows is what the game would have done. timeScale stays out of it.
                --stepsLeft_;
                delta   = stepDelta;
                physics = lastPlay_ == PlayState::Playing;
            }
            break;
    }

    // Written only on a change: this is a global engine switch, and a game that turned
    // physics off for its own reasons should keep that until the transport moves.
    if (!physicsWritten_ || physics != physicsWanted_) {
        wi::physics::SetSimulationEnabled(physics);
        physicsWanted_  = physics;
        physicsWritten_ = true;
    }

    sceneDelta_ = delta;
    if (delta > 0.0f) {
        sceneTime_ += double(delta);
        ++sceneFrames_;
    }
    return delta;
}

void PlayControl::GUI (bool compact) {
    // The pressed-in look is the state readout: with three modes and a toggle button,
    // "which one am I in" has to be visible without reading a label.
    const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);

    // A menu-bar row is one line of text tall. A normal Button carries frame padding on
    // top of that and stretches the whole bar, so the compact form is not a style choice
    // - it is what lets the transport sit BESIDE the gizmo buttons instead of above them.
    auto button = [compact](const char* label) {
        return compact ? ImGui::SmallButton(label) : ImGui::Button(label);
    };

    const bool playing = state_ == PlayState::Playing;
    if (playing) ImGui::PushStyleColor(ImGuiCol_Button, active);
    if (button("Play")) Play();
    if (playing) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Realtime, physics on");

    ImGui::SameLine();
    const bool noPhysics = state_ == PlayState::PlayingNoPhysics;
    if (noPhysics) ImGui::PushStyleColor(ImGuiCol_Button, active);
    if (button(compact ? "No physics" : "Play (no physics)")) PlayWithoutPhysics();
    if (noPhysics) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Animation and logic run; nothing is simulated.\n"
                          "Rigid bodies, ragdolls, cloth and vehicles hold still.");

    ImGui::SameLine();
    const bool paused = state_ == PlayState::Paused;
    if (paused) ImGui::PushStyleColor(ImGuiCol_Button, active);
    if (button("Pause")) Pause();
    if (paused) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The scene sees dt = 0. The camera still moves.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (button("Step")) Step();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One frame of %.1f ms, physics included.", stepDelta * 1000.0f);

    ImGui::SameLine();
    if (button("Reset")) Reset();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reload the scene and play from the start.\n"
                          "A full Unload() + Load(), not a rewind.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(compact ? 80.0f : 110.0f);
    ImGui::SliderFloat("##timescale", &timeScale, 0.05f, 4.0f, "x%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time scale. Applies while playing; a step ignores it.");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) timeScale = 1.0f;

    ImGui::SameLine();
    const int minutes = int(sceneTime_ / 60.0);
    if (compact) {
        // Which button is pressed in already says what the mode is, so the compact
        // readout spends its width on the clock rather than repeating it.
        ImGui::TextDisabled("%02d:%05.2f", minutes, sceneTime_ - minutes * 60.0);
    } else {
        ImGui::TextDisabled("%s  |  %02d:%05.2f  |  %llu frames",
                            ToString(state_), minutes, sceneTime_ - minutes * 60.0,
                            (unsigned long long)sceneFrames_);
    }
}

} // namespace st
