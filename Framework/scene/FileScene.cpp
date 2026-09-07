#include "scene/FileScene.h"
#include "wiBacklog.h"

namespace st {

FileScene::FileScene (std::string pathOrStem, std::string searchFolder)
    : requested_(std::move(pathOrStem)), folder_(std::move(searchFolder)) {}

void FileScene::Load () {
    wi::scene::Scene& scene = wi::scene::GetScene();

    const std::string path = ResolveSceneFile(requested_, folder_);
    if (path.empty()) {
        wi::backlog::post("FileScene: no .stsd or .wiscene for " + requested_,
                          wi::backlog::LogLevel::Warning);
        return;
    }

    // Blocks the main thread for as long as the map takes; ReportProgress drives the
    // native loading window and the in-game loading screen while it does.
    loaded_ = LoadSceneFileFlat(scene, path, XMMatrixIdentity(),
        [this](float fraction, const std::string& status) {
            ReportProgress(int(fraction * 100.0f), status);
        });
}

void FileScene::Update (float dt) {
    // RenderPath3D's own scene update is off (see st::App::Initialize), so every scene
    // steps the world itself and hands the render camera over first.
    wi::scene::Scene& scene = wi::scene::GetScene();
    scene.camera = wi::scene::GetCamera();
    scene.Update(dt);
}

void FileScene::Unload () {
    UnloadSceneFile(wi::scene::GetScene(), loaded_);
}

} // namespace st
