#include "SceneManager.h"
#include "scene/FileScene.h"
#include "scene/RuntimeScene.h"
#include "scene/SceneFile.h"
#include "io/asset/AssetSystem.h"
#include "io/asset/AssetPack.h"
#include "wiHelper.h"
#include "wiBacklog.h"

#include <algorithm>
#include <map>
#include <unordered_set>

namespace {

std::string Lower (const std::string& s) { return wi::helper::toLower(s); }

// "assets/scenes" and "assets/scenes/" are the same folder, and so is "assets\scenes".
std::string NormalizeFolder (const std::string& folder) {
    std::string out = folder;
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

} // namespace

void SceneManager::Register(const std::string& name, std::unique_ptr<Scene> scene) {
    origins_[name] = Origin::Cpp;
    scenes_[name]  = std::move(scene);
}

const std::string* SceneManager::FindNameNoCase(const std::string& name) const {
    const std::string wanted = Lower(name);
    for (const auto& kv : scenes_) {
        if (Lower(kv.first) == wanted)
            return &kv.first;
    }
    return nullptr;
}

SceneManager::Origin SceneManager::OriginOf(const std::string& name) const {
    auto it = origins_.find(name);
    return it == origins_.end() ? Origin::Cpp : it->second;
}

Scene* SceneManager::Get(const std::string& name) const {
    auto it = scenes_.find(name);
    return it == scenes_.end() ? nullptr : it->second.get();
}

std::vector<std::string> SceneManager::Names() const {
    std::vector<std::string> names;
    names.reserve(scenes_.size());
    for (const auto& kv : scenes_) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

bool SceneManager::RegisterFileScene(const std::string& name, const std::string& path,
                                     const std::string& searchFolder, Origin origin) {
    // Whoever got the name first keeps it. For DiscoverScenes() that is always the C++
    // registration, because RegisterScenes() runs before the scan.
    if (FindNameNoCase(name) != nullptr)
        return false;

    origins_[name] = origin;
    scenes_[name]  = std::make_unique<st::FileScene>(path, searchFolder);
    return true;
}

int SceneManager::DiscoverScenes(const std::string& folder) {
    const std::string dir = NormalizeFolder(folder);

    // Every map file a registered scene says it loads. Compared by STEM so that
    // "assets/scenes/s1map.wiscene", "s1map.stsd" and "s1map" are all the same claim -
    // which of the two forms exists depends on whether the build has packed.
    std::unordered_set<std::string> claimedStems;
    for (const auto& kv : scenes_) {
        if (kv.second == nullptr) continue;
        for (const std::string& file : kv.second->SceneFiles())
            claimedStems.insert(Lower(st::SceneNameFromFile(file)));
    }

    // stem -> the file to open for it. The .stsd wins when both forms are present: it is
    // what ships, and it pulls its textures out of the content pack.
    std::map<std::string, std::string> candidates;
    auto consider = [&](const std::string& path) {
        const std::string extension = Lower(wi::helper::GetExtensionFromFileName(path));
        if (!st::IsSceneFileExtension(extension))
            return;
        const std::string stem = st::SceneNameFromFile(path);
        if (stem.empty())
            return;
        auto it = candidates.find(Lower(stem));
        if (it == candidates.end()) {
            candidates.emplace(Lower(stem), path);
        } else if (extension == "stsd") {
            it->second = path;
        }
    };

    // Loose files first. This is where an unpacked development build keeps its .wiscene
    // sources, and where the packer writes its converted .stsd.
    if (wi::helper::DirectoryExists(dir)) {
        // The trailing slash matters: the helper concatenates directory + filename raw.
        wi::helper::GetFileNamesInDirectory(dir + "/", [&](std::string path) { consider(path); });
    }

    // Then whatever the mounted packages hold under the same folder, so a shipped build
    // with no loose files still finds its maps.
    const std::string prefix = Lower(dir) + "/";
    st::AssetSystem& assets = st::AssetSystem::Get();
    for (uint32_t m = 0; m < assets.MountCount(); ++m) {
        const st::asset::AssetPack* pack = assets.PackAt(m);
        if (pack == nullptr) continue;
        const std::string mountPoint = assets.MountPointAt(m);
        for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
            const st::asset::StrdAsset* entry = pack->AssetAt(i);
            if (entry == nullptr) continue;
            std::string logical = mountPoint + pack->NameString(*entry);
            std::replace(logical.begin(), logical.end(), '\\', '/');
            const std::string lowered = Lower(logical);
            if (lowered.size() <= prefix.size()) continue;
            if (lowered.compare(0, prefix.size(), prefix) != 0) continue;
            // Only the folder itself, not a sub-folder inside it.
            if (lowered.find('/', prefix.size()) != std::string::npos) continue;
            consider(logical);
        }
    }

    int added = 0;
    for (const auto& kv : candidates) {
        const std::string& path = kv.second;
        const std::string  stem = st::SceneNameFromFile(path);

        // A map a C++ scene already loads is that scene's, not a scene of its own.
        if (claimedStems.count(Lower(stem)) != 0)
            continue;

        // A name a C++ scene already holds stays that scene's - the C++ version overrides
        // the file, which is the whole point of the override rule.
        if (FindNameNoCase(stem) != nullptr)
            continue;

        if (RegisterFileScene(stem, path, dir, Origin::Folder)) {
            ++added;
            wi::backlog::post("SceneManager: discovered scene '" + stem + "' (" + path + ")");
        }
    }
    return added;
}

std::string SceneManager::NewScene(const std::string& baseName, bool withDefaultLighting) {
    const std::string base = baseName.empty() ? std::string("Untitled") : baseName;

    std::string name = base;
    for (int suffix = 2; FindNameNoCase(name) != nullptr; ++suffix)
        name = base + " " + std::to_string(suffix);

    origins_[name] = Origin::Runtime;
    scenes_[name]  = std::make_unique<st::RuntimeScene>(withDefaultLighting);
    Load(name);
    return name;
}

bool SceneManager::Unregister(const std::string& name) {
    // Dropping the active scene would leave current_ dangling with entities still in the
    // world; switching away first is the caller's job.
    if (name == currentName_ || name == pendingLoad_)
        return false;

    auto it = scenes_.find(name);
    if (it == scenes_.end())
        return false;

    scenes_.erase(it);
    origins_.erase(name);
    return true;
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
