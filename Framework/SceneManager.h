#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include "stScene.h"

class SceneManager {
public:
    // Where a registered scene came from. Shown in the Scene Manager window, and what
    // DiscoverScenes() consults before it adds anything.
    enum class Origin {
        Cpp,      // registered from st::App::RegisterScenes - a C++ class
        Folder,   // found in the scene folder by DiscoverScenes()
        Runtime,  // created by NewScene() this session, not saved anywhere yet
    };

    void Register(const std::string& name, std::unique_ptr<Scene> scene);

    // Register a map file as a scene of its own (an st::FileScene). Returns false when
    // the name is already taken - the existing scene wins, which is what makes a C++
    // scene override a same-named file in the scene folder.
    bool RegisterFileScene(const std::string& name, const std::string& path,
                           const std::string& searchFolder = "assets/scenes",
                           Origin origin = Origin::Folder);

    // Scan `folder` for .stsd / .wiscene maps and register one scene per map, named after
    // the file. Additive and safe to re-run: nothing already registered is touched.
    //
    // A file is skipped when
    //   - a scene of that name exists already (case-insensitive), so C++ wins the name, or
    //   - a registered scene declares it loads that file (Scene::SceneFiles()), so a map a
    //     C++ scene already owns is not offered twice.
    //
    // Returns how many scenes were added. The packed .stsd is preferred over the .wiscene
    // it was converted from when both are present.
    int DiscoverScenes(const std::string& folder = "assets/scenes");

    // Create an empty scene (an st::RuntimeScene) and queue it for loading. `baseName` is
    // made unique if needed ("Untitled", "Untitled 2", ...). Returns the name it got.
    std::string NewScene(const std::string& baseName = "Untitled",
                         bool withDefaultLighting = true);

    // Forget a scene. Refuses to drop the active one - unload or switch away first.
    bool Unregister(const std::string& name);

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

    // Registered scene names, sorted - for UI selectors.
    std::vector<std::string> Names() const;

    bool   Has      (const std::string& name) const { return scenes_.count(name) != 0; }
    Origin OriginOf (const std::string& name) const;
    Scene* Get      (const std::string& name) const;

private:
    // Case-insensitive lookup, so "cinema.wiscene" in the scene folder does not shadow a
    // C++ scene called "Cinema" - and does not get registered next to it either.
    const std::string* FindNameNoCase(const std::string& name) const;

    std::unordered_map<std::string, std::unique_ptr<Scene>> scenes_;
    std::unordered_map<std::string, Origin>                 origins_;
    Scene*      current_     = nullptr;
    std::string currentName_;
    std::string pendingLoad_;
    std::string loadingName_;
    Callback    onLoaded_;
    Callback    onUnloaded_;
};
