#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include "stScene.h"

class SceneManager {
public:
    void Register(const std::string& name, std::unique_ptr<Scene> scene);

    // Deferred: actual transition happens at the top of the next Update()
    void Load(const std::string& name);

    // Reload the current scene from scratch: defers a full Unload() + Load() of the
    // active scene at the next Update(). Re-creates the scene's entities; the scene
    // C++ object is reused (its EventBus subscription captures `this`), so this is a
    // clean entity-level reset, not an object reconstruction. No-op if none active.
    void Reload();

    void Update(float dt);
    void OnGUI();
    // The active scene's developer UI. st::App only calls this while DevUI is visible.
    void OnDevGUI();

    // Fired around a deferred transition, with the scene name. Reload() fires both.
    // st::App wires these to its OnSceneUnloaded / OnSceneLoaded hooks.
    using Callback = std::function<void(const std::string&)>;
    void SetCallbacks(Callback onLoaded, Callback onUnloaded);

    // Set while a transition is in flight, so the loading screen knows what is
    // being loaded. Empty when idle.
    const std::string& LoadingName() const { return loadingName_; }

    // A transition is queued but has not run yet. Scene::Load() blocks the main
    // thread, so st::App uses this to raise the native loading window BEFORE
    // calling Update().
    bool HasPendingLoad() const { return !pendingLoad_.empty(); }
    const std::string& PendingName() const { return pendingLoad_; }

    const std::string& CurrentName() const { return currentName_; }

    // Registered scene names, sorted — for UI selectors.
    std::vector<std::string> Names() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Scene>> scenes_;
    Scene*      current_     = nullptr;
    std::string currentName_;
    std::string pendingLoad_;
    std::string loadingName_;
    Callback    onLoaded_;
    Callback    onUnloaded_;
};
