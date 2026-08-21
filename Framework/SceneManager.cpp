#include "SceneManager.h"
#include <algorithm>

void SceneManager::Register(const std::string& name, std::unique_ptr<Scene> scene) {
    scenes_[name] = std::move(scene);
}

std::vector<std::string> SceneManager::Names() const {
    std::vector<std::string> names;
    names.reserve(scenes_.size());
    for (const auto& kv : scenes_) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

void SceneManager::Load(const std::string& name) {
    pendingLoad_ = name;
}

void SceneManager::Reload() {
    // Queue a reload of the active scene. Update() unloads then loads the same name,
    // so entities are torn down and rebuilt. Guard: nothing to reload if idle.
    if (!currentName_.empty())
        pendingLoad_ = currentName_;
}

void SceneManager::SetCallbacks(Callback onLoaded, Callback onUnloaded) {
    onLoaded_   = std::move(onLoaded);
    onUnloaded_ = std::move(onUnloaded);
}

void SceneManager::Update(float dt) {
    if (!pendingLoad_.empty()) {
        // Published for the whole transition so the loading screen can name the scene
        // it is waiting on; Load() below may block for a long time.
        loadingName_ = pendingLoad_;

        if (current_) {
            const std::string previous = currentName_;
            current_->Unload();
            if (onUnloaded_) onUnloaded_(previous);
        }

        auto it = scenes_.find(pendingLoad_);
        if (it != scenes_.end()) {
            current_     = it->second.get();
            currentName_ = pendingLoad_;
            current_->Load();
            if (onLoaded_) onLoaded_(currentName_);
        }
        pendingLoad_.clear();
        loadingName_.clear();
    }

    if (current_)
        current_->Update(dt);
}

void SceneManager::OnGUI() {
    if (current_)
        current_->OnGUI();
}

void SceneManager::OnDevGUI() {
    if (current_)
        current_->OnDevGUI();
}
