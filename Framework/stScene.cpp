#include "stScene.h"
#include "eventBus.h"

void Scene::ReportProgress(int percent, const std::string& status) {
    // Same "<percent>|<status>" payload the framework has always used, so scenes that
    // still Emit("loading.progress", ...) by hand keep working. st::App subscribes and
    // routes it to the startup window / loading screen.
    EventBus::Get().Emit("loading.progress", std::to_string(percent) + "|" + status);
}
