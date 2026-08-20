#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
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

    const std::string& CurrentName() const { return currentName_; }

    // Registered scene names, sorted — for UI selectors.
    std::vector<std::string> Names() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Scene>> scenes_;
    Scene*      current_     = nullptr;
    std::string currentName_;
    std::string pendingLoad_;
};
