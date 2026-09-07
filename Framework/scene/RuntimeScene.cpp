#include "scene/RuntimeScene.h"
#include "scene/SceneFile.h"
#include "scene/DayNight.h"

namespace st {

void RuntimeScene::Load () {
    wi::scene::Scene& scene = wi::scene::GetScene();

    // SceneManager unloads the previous scene before this runs, so in practice the world
    // is empty here - but the snapshot is what makes Unload() correct either way.
    preexisting_.clear();
    std::vector<wi::ecs::Entity> before;
    GatherSceneEntities(scene, before);
    preexisting_.insert(before.begin(), before.end());

    if (!defaultLighting_)
        return;

    // The daylight system builds the sun and the weather, and then drives them - so a new
    // scene comes up with a working day/night cycle rather than a light frozen wherever it
    // happened to be created. Its clock and place are in Simtary > Day / Night.
    sunEntity_     = DayNight::Get().Create(scene);
    weatherEntity_ = DayNight::Get().WeatherEntity();
}

void RuntimeScene::Update (float dt) {
    // RenderPath3D's own scene update is off (see st::App::Initialize), so every scene
    // steps the world itself and hands the render camera over first.
    wi::scene::Scene& scene = wi::scene::GetScene();
    scene.camera = wi::scene::GetCamera();
    scene.Update(dt);
}

void RuntimeScene::Unload () {
    wi::scene::Scene& scene = wi::scene::GetScene();

    std::vector<wi::ecs::Entity> current;
    GatherSceneEntities(scene, current);
    for (wi::ecs::Entity entity : current) {
        if (preexisting_.count(entity) == 0)
            scene.Entity_Remove(entity);
    }

    preexisting_.clear();
    sunEntity_     = wi::ecs::INVALID_ENTITY;
    weatherEntity_ = wi::ecs::INVALID_ENTITY;
}

} // namespace st
