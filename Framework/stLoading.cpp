// Default loading screen.
//
// Two things share it, because the player sees both as "the game is not ready yet":
//
//   1. The first-launch pipeline warm-up. The engine compiles object shaders and
//      builds PSOs on background job-system threads; ImGui's own shaders were loaded
//      synchronously in ImguiInit(), so this overlay renders fine while the rest is
//      still compiling.
//   2. Whatever LoadingState carries — a scene name and a progress line pushed by
//      Scene::ReportProgress().
//
// Note what this CANNOT do: Scene::Load() blocks the main thread, so no ImGui frame
// is drawn while a scene transition runs. That case is covered by the native
// SubWinStatus window, which lives on its own thread (see st::App::Update). This
// overlay covers the non-blocking cases and the compile warm-up.
//
// A project replaces the whole thing by overriding st::App::RenderLoadingScreen.

#include "stApp.h"

namespace {
// wi::renderer::IsPipelineCreationActive() sums four job-system contexts, and one of
// them (object_pso_job_ctx) is Priority::Low. A low-priority job can stay queued
// behind other work indefinitely, so "N jobs remaining" is NOT a reliable
// "startup is still going" signal — waiting for it to reach zero can pin this overlay
// on screen forever. The warm-up notice therefore gets a hard deadline: past this many
// frames it is dismissed whatever the counter says. The engine keeps compiling in the
// background either way; only the notice goes away.
constexpr int kMaxWarmupFrames = 600;   // ~10 s at 60 fps
}

void st::App::RenderLoadingScreen(const st::LoadingState& state) {
    if (!loadingDone_) {
        loadingFrames_++;

        // wi::renderer compiles object shaders + builds PSOs on background job-system
        // threads; this returns the number of jobs still in flight.
        const int active = wi::renderer::IsPipelineCreationActive();

        if (active > 0) { loadingSawWork_ = true; loadingIdleFrames_ = 0; }
        else            { loadingIdleFrames_++; }

        // Settled: the queue has been idle for a short debounce. If no job ever
        // started (shaders precompiled/embedded), give it ~30 frames then drop it.
        const bool settled = loadingIdleFrames_ > 10
                          && (loadingSawWork_ || ImGui::GetFrameCount() > 30);

        if (settled || loadingFrames_ > kMaxWarmupFrames)
            loadingDone_ = true;
    }

    // Nothing to say: no warm-up notice and no scene progress. Draw nothing at all —
    // an empty ImGui window still paints a box.
    const bool showProgress = state.active
        && (!state.scene.empty() || !state.status.empty() || state.percent >= 0);
    if (loadingDone_ && !showProgress)
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(20, vp->Size.y - 50));

    ImGui::Begin("##loading_overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);

    if (!loadingDone_) {
        const int active = wi::renderer::IsPipelineCreationActive();
        if (active > 0)
            ImGui::Text("Creating Pipeline... (%d job%s remaining)", active, active == 1 ? "" : "s");
        else
            ImGui::TextUnformatted("Finishing up...");
    }

    if (showProgress) {
        if (!state.scene.empty())
            ImGui::Text("Loading %s", state.scene.c_str());
        if (!state.status.empty())
            ImGui::TextUnformatted(state.status.c_str());
        if (state.percent >= 0)
            ImGui::ProgressBar(state.percent / 100.0f, ImVec2(260.0f, 0.0f));
    }

    ImGui::End();
}
