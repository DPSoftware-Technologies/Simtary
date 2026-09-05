#pragma once
// Scene - one screen/level/state of a game.
//
// A project subclasses this, builds its entities in Load(), and registers the scene
// by name from st::App::RegisterScenes(). SceneManager owns the instances and
// switches between them; transitions are deferred to the next frame, so calling
// SceneManager::Load() from inside a UI callback is safe.
//
// The scene OBJECT outlives Unload() - it is reused if the scene is loaded again
// so reset any cached entity handles in Unload().

#include <string>

class Scene {
public:
    virtual ~Scene() = default;

    // Build the scene's entities. Runs on the main thread.
    virtual void Load() {}
    // Per frame, after input has been refreshed. A scene is responsible for its own
    // wi::scene::Scene::Update(dt) - the render path's own scene update is disabled
    // so the scene is never stepped twice in a frame.
    virtual void Update(float dt) {}
    // The scene's game-facing ImGui, drawn every frame.
    virtual void OnGUI() {}
    // The scene's developer ImGui - tuning sliders, debug readouts. Only called
    // while DevUI is visible, so it costs nothing in a shipped build.
    virtual void OnDevGUI() {}
    // Destroy what Load() created and reset cached handles.
    virtual void Unload() {}

protected:
    // Report load progress from inside Load(). Drives the native startup window and
    // the in-game loading screen (st::App::RenderLoadingScreen).
    //   percent: 0..100, or -1 for indeterminate
    void ReportProgress(int percent, const std::string& status);
};
