#pragma once
// RuntimeScene - an empty scene created at runtime, from the Scene Manager's "New scene".
//
// It has no file behind it: Load() asks st::DayNight for a sun and a sky, so an otherwise
// empty world is lit and has a running day/night cycle, and everything after that is
// whatever the editor creates. Unload() removes every entity that appeared while it was
// active, which is what makes switching away from an unsaved new scene leave the world
// clean.
//
// Save it from Scene > Save As; once it is a .stsd in the scene folder the next run picks
// it up as a FileScene through SceneManager::DiscoverScenes().

#include "stScene.h"
#include "wiScene.h"

#include <unordered_set>
#include <vector>

namespace st {

class RuntimeScene : public ::Scene {
public:
    // `withDefaultLighting` off gives a genuinely empty world - no sun, no weather, and
    // nothing for the daylight system to drive.
    explicit RuntimeScene (bool withDefaultLighting = true)
        : defaultLighting_(withDefaultLighting) {}

    void Load   () override;
    void Update (float dt) override;
    void Unload () override;

private:
    bool defaultLighting_ = true;

    // What already existed when this scene was loaded. Anything outside this set at
    // Unload() time was created during its lifetime and goes with it.
    std::unordered_set<wi::ecs::Entity> preexisting_;

    wi::ecs::Entity sunEntity_     = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity weatherEntity_ = wi::ecs::INVALID_ENTITY;
};

} // namespace st
