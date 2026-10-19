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
    // Asking an editing scene to play is asking for Play mode. Pause in Edit mode only
    //	records the state, so the next Play starts paused, as Unity's pause button does.
    if (editing_ && state != PlayState::Paused) pending_ = Transition::EnterPlay;
}

void PlayControl::TogglePause () {
    SetState(state_ == PlayState::Paused ? lastPlay_ : PlayState::Paused);
}

void PlayControl::Stop () {
    if (editing_) return;
    pending_ = Transition::EnterEdit;
}

PlayControl::Transition PlayControl::ConsumeTransition () {
    const Transition t = pending_;
    pending_ = Transition::None;
    if (t == Transition::EnterPlay || t == Transition::Restart) {
        editing_     = false;
        sceneTime_   = 0.0;
        sceneFrames_ = 0;
    } else if (t == Transition::EnterEdit) {
        editing_   = true;
        stepsLeft_ = 0;
        if (state_ == PlayState::Paused) state_ = lastPlay_; // a Stop ends the pause too
    }
    return t;
}

void PlayControl::Reset () {
    stepsLeft_ = 0;
    if (editing_) {
        // Nothing to rewind in Edit mode: Reset means "play from the start".
        SetState(PlayState::Playing);
        return;
    }
    SetState(PlayState::Playing);
    pending_ = Transition::Restart;
    if (resetHook_) resetHook_();
}

void PlayControl::Step (int frames) {
    if (frames <= 0) return;
    // From Edit mode a step enters Play mode paused and takes the frame, like Unity's.
    if (editing_) pending_ = Transition::EnterPlay;
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

    switch (editing_ ? PlayState::Paused : state_) {
        case PlayState::Playing:
            delta   = realDelta * timeScale;
            physics = true;
            break;
        case PlayState::PlayingNoPhysics:
            delta   = realDelta * timeScale;
            physics = false;
            break;
        case PlayState::Paused:
            if (stepsLeft_ > 0 && !editing_) {
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

    // Mode first: in Edit mode none of the transport buttons is "pressed".
    if (editing_) ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "EDIT");
    else          ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f), "PLAY");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(editing_ ? "Edit mode: the scene is drawn but nothing runs.\nPlay (F5) snapshots it and starts it."
                                   : "Play mode: changes made now are thrown away by Stop (F5).");
    ImGui::SameLine();

    const bool playing = !editing_ && state_ == PlayState::Playing;
    if (playing) ImGui::PushStyleColor(ImGuiCol_Button, active);
    if (button("Play")) Play();
    if (playing) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Realtime, physics on");

    ImGui::SameLine();
    ImGui::BeginDisabled(editing_);
    if (button("Stop")) Stop();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Back to Edit mode. Everything that changed while playing is put back.");

    ImGui::SameLine();
    const bool noPhysics = !editing_ && state_ == PlayState::PlayingNoPhysics;
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
    ImGui::BeginDisabled(!paused && !editing_);
    if (button("Step")) Step();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One frame of %.1f ms, physics included.", stepDelta * 1000.0f);

    ImGui::SameLine();
    if (button("Restart")) Reset();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Play from the start: the Play-mode snapshot is put back and play\n"
                          "begins again. A game launched playing has none - its scene reloads.");

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
