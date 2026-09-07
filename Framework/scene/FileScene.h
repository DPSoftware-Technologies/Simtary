#pragma once
// FileScene - a scene that is nothing but a map file.
//
// This is what SceneManager::DiscoverScenes() registers for every .stsd/.wiscene it finds
// in the scene folder, so dropping a map into assets/scenes/ makes it selectable in the
// Scene Manager with no C++ at all. It is also usable directly when a game scene needs no
// behaviour of its own:
//
//     scenes.Register("Cinema", std::make_unique<st::FileScene>("assets/scenes/cinema"));
//
// The file is loaded FLAT (st::LoadSceneFileFlat), so its own top-level nodes are the
// hierarchy's top-level rows rather than children of one wrapper entity.

#include "stScene.h"
#include "scene/SceneFile.h"

namespace st {

class FileScene : public ::Scene {
public:
    // `pathOrStem` is resolved at Load() time, not here: whether the packed .stsd or the
    // source .wiscene is the one that exists can change between builds.
    explicit FileScene (std::string pathOrStem, std::string searchFolder = "assets/scenes");

    void Load   () override;
    void Update (float dt) override;
    void Unload () override;

    std::vector<std::string> SceneFiles () const override { return { requested_ }; }

    // The file this scene was asked for, before resolution.
    const std::string& RequestedPath () const { return requested_; }
    // The file the last Load() actually opened. Empty until then.
    const std::string& LoadedPath () const { return loaded_.path; }

private:
    std::string     requested_;
    std::string     folder_;
    LoadedSceneFile loaded_;
};

} // namespace st
