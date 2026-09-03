#include "imaudio.h"

#include "stAudioEngine.h"
#include "stAudioSpatial.h"
#include "imgui.h"

#include <string>
#include <vector>
#include <cstdio>

namespace st::devui {

namespace {

// A meter that reads like a meter: green up to -6 dBFS, amber to -1, red above.
// Clipping is the one thing a mixer must never make you squint for.
void LevelBar (float level, const char* overlay = nullptr) {
    const ImVec4 colour =
        level > 0.89f ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f) :
        level > 0.50f ? ImVec4(0.90f, 0.70f, 0.25f, 1.0f) :
                        ImVec4(0.35f, 0.75f, 0.45f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
    ImGui::ProgressBar(level, ImVec2(-FLT_MIN, 0), overlay);
    ImGui::PopStyleColor();
}

void Fader (const char* label, float value, void (*set)(float)) {
    float v = value;
    if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
        set(v);
}

const char* SubmixName (audio::Submix s) {
    switch (s) {
    case audio::Submix::SoundEffect: return "SoundEffect";
    case audio::Submix::Music:       return "Music";
    case audio::Submix::Voice:       return "Voice";
    case audio::Submix::UI:          return "UI";
    case audio::Submix::Ambient:     return "Ambient";
    default:                         return "?";
    }
}

void LabelledValue (const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

} // namespace

void AudioMixerGUI () {
    audio::AudioEngine& engine = audio::AudioEngine::Get();

    if (!engine.IsInitialized()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Audio engine is not running.");
        ImGui::TextWrapped("No OpenAL output device was opened at start-up, so the game is "
                           "silent. Check the backlog for the stAudioEngine line.");
        return;
    }

    const audio::AudioEngine::Stats stats = engine.GetStats();
    audio::Spatializer& spatializer = audio::Spatializer::Get();

    // ── transport / master ────────────────────────────────────────────────────
    bool paused = engine.IsPaused();
    if (ImGui::Checkbox("pause all", &paused))
        engine.SetPaused(paused);
    ImGui::SameLine();
    if (ImGui::Button("stop all"))
        audio::StopAll();
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", engine.IsSpatialAvailable() ? "Steam Audio" : "fallback panner");

    LevelBar(stats.peakOutput, "output peak");

    if (ImGui::CollapsingHeader("Mix", ImGuiTreeNodeFlags_DefaultOpen)) {
        Fader("master", engine.GetMasterVolume(),
              [](float v) { audio::AudioEngine::Get().SetMasterVolume(v); });
        ImGui::Separator();
        // One fader per submix. The lambdas are capture-less so they stay plain
        // function pointers; the submix is baked into each rather than captured.
        Fader("SoundEffect", engine.GetSubmixVolume(audio::Submix::SoundEffect),
              [](float v) { audio::AudioEngine::Get().SetSubmixVolume(audio::Submix::SoundEffect, v); });
        Fader("Music", engine.GetSubmixVolume(audio::Submix::Music),
              [](float v) { audio::AudioEngine::Get().SetSubmixVolume(audio::Submix::Music, v); });
        Fader("Voice", engine.GetSubmixVolume(audio::Submix::Voice),
              [](float v) { audio::AudioEngine::Get().SetSubmixVolume(audio::Submix::Voice, v); });
        Fader("UI", engine.GetSubmixVolume(audio::Submix::UI),
              [](float v) { audio::AudioEngine::Get().SetSubmixVolume(audio::Submix::UI, v); });
        Fader("Ambient", engine.GetSubmixVolume(audio::Submix::Ambient),
              [](float v) { audio::AudioEngine::Get().SetSubmixVolume(audio::Submix::Ambient, v); });
    }

    // ── OpenAL ────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("OpenAL - device and 2D", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("openal", 2, ImGuiTableFlags_SizingStretchProp)) {
            LabelledValue("device", "%s", engine.GetDeviceName());
            LabelledValue("mix rate", "%d Hz", engine.GetSampleRate());
            LabelledValue("block", "%d frames (%.1f ms)", engine.GetFrameSize(),
                          1000.0f * (float)engine.GetFrameSize() / (float)engine.GetSampleRate());
            LabelledValue("sample format", "%s", engine.GetConfig().floatOutput ? "float32 (if supported)" : "int16");
            LabelledValue("2D voices in use", "%d / %d", stats.voices2D, engine.GetConfig().maxVoices2D);
            LabelledValue("blocks rendered", "%llu", (unsigned long long)stats.blocksRendered);
            ImGui::EndTable();
        }

        // Underruns are the number that matters: anything but zero is audible as a
        // click or a stutter, and it means the mixer missed its deadline.
        if (stats.underruns > 0)
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f),
                               "underruns: %llu  (the mixer missed a block deadline)",
                               (unsigned long long)stats.underruns);
        else
            ImGui::TextDisabled("underruns: 0");

        // Mix load is the fraction of one block period spent mixing. Past ~0.8 the
        // next underrun is a matter of time.
        LevelBar(stats.mixLoad > 1.0f ? 1.0f : stats.mixLoad, "mix load");
    }

    // ── Steam Audio ───────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Steam Audio - 3D", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!spatializer.IsSteamAudioAvailable()) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "Steam Audio is NOT active.");
            ImGui::TextWrapped("3D falls back to distance attenuation plus stereo panning: no HRTF, "
                               "no occlusion, no reflections, no pathing. Either the build was "
                               "configured with SIMTARY_ENABLE_STEAMAUDIO=OFF, or phonon.dll is "
                               "missing next to the executable.");
        } else {
            const audio::SimulationSettings& sim = spatializer.GetSimulationSettings();
            if (ImGui::BeginTable("steamaudio", 2, ImGuiTableFlags_SizingStretchProp)) {
                LabelledValue("ray tracer", "%s",
                              sim.backend == audio::SceneBackend::Embree     ? "Embree" :
                              sim.backend == audio::SceneBackend::RadeonRays ? "Radeon Rays" : "built-in");
                LabelledValue("rays / bounces", "%d / %d", sim.maxRays, sim.maxBounces);
                LabelledValue("IR duration", "%.2f s at ambisonic order %d", sim.maxDuration, sim.maxOrder);
                LabelledValue("source budget", "%d", sim.maxSources);
                LabelledValue("sim threads", "%d", sim.threads);
                LabelledValue("sim rate", "%.1f Hz", sim.updateRateHz);
                LabelledValue("reflections", "%s", sim.reflections ? "enabled" : "off");
                LabelledValue("pathing", "%s", sim.pathing ? "enabled" : "off");
                ImGui::EndTable();
            }
            // Occlusion and reflections are inert until scene geometry is registered,
            // and nothing does that automatically yet. Saying so here saves the hour
            // otherwise spent wondering why a wall does not block anything.
            ImGui::TextDisabled("Geometry: register meshes with Spatializer::AddStaticMesh() for "
                                "occlusion, reflections and pathing to do anything.");
        }
    }

    // ── collectors ────────────────────────────────────────────────────────────
    std::vector<audio::CollectorRef> collectors;
    engine.GetCollectors(collectors);

    if (ImGui::CollapsingHeader("Collectors (microphones)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%d collector%s, %d rendering",
                    (int)collectors.size(), collectors.size() == 1 ? "" : "s", stats.activeCollectors);
        if (ImGui::BeginTable("collectors", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("priority");
            ImGui::TableSetupColumn("routed");
            ImGui::TableSetupColumn("volume");
            ImGui::TableSetupColumn("level");
            ImGui::TableHeadersRow();

            for (audio::CollectorRef& c : collectors) {
                ImGui::TableNextRow();
                ImGui::PushID(c.get());

                ImGui::TableNextColumn();
                if (c->IsPrimary())
                    // The primary is the one the player actually hears; everything else
                    // is rendering into a buffer for someone else's benefit.
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s  (primary)", c->GetName().c_str());
                else if (!c->IsEnabled())
                    ImGui::TextDisabled("%s  (disabled)", c->GetName().c_str());
                else
                    ImGui::TextUnformatted(c->GetName().c_str());

                ImGui::TableNextColumn(); ImGui::Text("%d", c->GetPriority());

                ImGui::TableNextColumn();
                bool routed = c->GetRouteToOutput();
                if (ImGui::Checkbox("##routed", &routed)) c->SetRouteToOutput(routed);

                ImGui::TableNextColumn();
                float volume = c->GetVolume();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##vol", &volume, 0.0f, 1.0f, "%.2f")) c->SetVolume(volume);

                ImGui::TableNextColumn();
                LevelBar(c->Output().GetRMS());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ── emitters ──────────────────────────────────────────────────────────────
    std::vector<audio::EmitterRef> emitters;
    engine.GetEmitters(emitters);

    if (ImGui::CollapsingHeader("Emitters (speakers)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%d emitter%s, %d producing signal, %d through Steam Audio",
                    (int)emitters.size(), emitters.size() == 1 ? "" : "s",
                    stats.activeEmitters, stats.spatialEmitters);

        if (ImGui::BeginTable("emitters", 7,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 220))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("submix");
            ImGui::TableSetupColumn("play");
            ImGui::TableSetupColumn("volume");
            ImGui::TableSetupColumn("distance");
            ImGui::TableSetupColumn("occlusion");
            ImGui::TableSetupColumn("level");
            ImGui::TableHeadersRow();

            for (audio::EmitterRef& e : emitters) {
                ImGui::TableNextRow();
                ImGui::PushID(e.get());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e->GetName().c_str());
                if (ImGui::IsItemHovered()) {
                    if (audio::AudioClip clip = e->GetClip())
                        ImGui::SetTooltip("%s\n%.2f s, %d ch, %d Hz\n%s",
                                          clip->name.c_str(), clip->GetLengthSeconds(),
                                          clip->channels, clip->sampleRate,
                                          e->IsSpatial() ? "3D (Steam Audio)" : "2D (OpenAL)");
                    else
                        ImGui::SetTooltip("no clip - fed through Input()\n%s",
                                          e->IsSpatial() ? "3D (Steam Audio)" : "2D (OpenAL)");
                }

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s%s", SubmixName(e->GetSubmix()), e->IsSpatial() ? " 3D" : " 2D");

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(e->IsPlaying() ? "||" : ">")) {
                    if (e->IsPlaying()) e->Pause(); else e->Play();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("[]")) e->Stop();

                ImGui::TableNextColumn();
                float volume = e->GetVolume();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##vol", &volume, 0.0f, 1.0f, "%.2f")) e->SetVolume(volume);

                // The simulator's verdict. When an emitter is playing but silent, the
                // answer is almost always in these two columns rather than in the fader.
                const audio::SpatialResult result = e->GetSpatialResult();
                ImGui::TableNextColumn();
                if (e->IsSpatial()) {
                    if (!result.audible)
                        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f), "%.1f m (culled)", result.distance);
                    else
                        ImGui::Text("%.1f m  x%.2f", result.distance, result.distanceAttenuation);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableNextColumn();
                if (e->IsSpatial())
                    ImGui::Text("%.0f%%", result.occlusion * 100.0f);
                else
                    ImGui::TextDisabled("-");

                ImGui::TableNextColumn();
                LevelBar(e->Output().GetRMS());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

void AudioMixerWindow (bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Audio Mixer", p_open))
        AudioMixerGUI();
    ImGui::End();
}

} // namespace st::devui
