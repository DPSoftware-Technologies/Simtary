#include "scene/SceneFile.h"
#include "io/asset/AssetSystem.h"
#include "wiHelper.h"
#include "wiBacklog.h"

#include <unordered_set>

namespace st {

bool IsSceneFileExtension (const std::string& extensionLower) {
    return extensionLower == "stsd" || extensionLower == "wiscene";
}

std::string SceneNameFromFile (const std::string& path) {
    return wi::helper::RemoveExtension(wi::helper::GetFileNameFromPath(path));
}

std::string ResolveSceneFile (const std::string& pathOrStem, const std::string& searchFolder) {
    // Anything with a directory in it is taken at face value; a bare stem is looked up in
    // the scene folder. Either way the extension is re-decided here, because which of the
    // two forms of a map exists depends on whether the build has packed yet.
    std::string base = pathOrStem;
    const std::string ext = wi::helper::toLower(wi::helper::GetExtensionFromFileName(base));
    if (IsSceneFileExtension(ext))
        base = wi::helper::RemoveExtension(base);

    if (base.find('/') == std::string::npos && base.find('\\') == std::string::npos) {
        std::string folder = searchFolder;
        if (!folder.empty() && folder.back() != '/' && folder.back() != '\\')
            folder += '/';
        base = folder + base;
    }

    // The packed descriptor wins: it is what ships, and its textures come out of the
    // content pack instead of off disk.
    const std::string stsd = base + ".stsd";
    if (st::AssetSystem::Get().CanLoadScene(stsd))
        return stsd;

    const std::string wiscene = base + ".wiscene";
    if (wi::helper::FileExists(wiscene))
        return wiscene;

    return {};
}

void GatherSceneEntities (wi::scene::Scene& scene, std::vector<wi::ecs::Entity>& out) {
    std::unordered_set<wi::ecs::Entity> seen;
    for (auto& entry : scene.componentLibrary.entries) {
        const auto* manager = entry.second.component_manager.get();
        if (manager == nullptr)
            continue;
        for (wi::ecs::Entity entity : manager->GetEntityArray()) {
            if (seen.insert(entity).second)
                out.push_back(entity);
        }
    }
}

LoadedSceneFile LoadSceneFileFlat (wi::scene::Scene& target,
                                   const std::string& path,
                                   const XMMATRIX& transform,
                                   wi::scene::LoadModelProgressCallback progress) {
    LoadedSceneFile out;
    out.path = path;

    const std::string extension = wi::helper::toLower(wi::helper::GetExtensionFromFileName(path));

    // Read into a temporary scene and merge, rather than straight into the live one. This
    // is what the hand-written scene loads have always done: Merge() moves the component
    // managers across with the entity IDs intact and hands the already-constructed native
    // component instances over, so nothing gets Start()ed a second time.
    wi::scene::Scene temporary;

    // attached = false is the whole point of this function - see the header.
    if (extension == "stsd") {
        st::AssetSystem::Get().LoadScene(temporary, path, transform, false, progress, &out.error);
    } else {
        wi::scene::LoadModel(temporary, path, transform, false, progress);
    }

    GatherSceneEntities(temporary, out.entities);
    if (out.entities.empty()) {
        if (out.error.empty())
            out.error = "could not load " + path;
        wi::backlog::post("SceneFile: " + out.error, wi::backlog::LogLevel::Error);
        return out;
    }

    // The roots are the transforms nobody parented - after a flat load that is exactly the
    // set of rows the Hierarchy will show at its top level.
    for (size_t i = 0; i < temporary.transforms.GetCount(); ++i) {
        const wi::ecs::Entity entity = temporary.transforms.GetEntity(i);
        if (!temporary.hierarchy.Contains(entity))
            out.roots.push_back(entity);
    }

    target.Merge(temporary);
    out.ok = true;
    return out;
}

void UnloadSceneFile (wi::scene::Scene& target, LoadedSceneFile& loaded) {
    // Entity_Remove is recursive and a no-op on an entity that is already gone, so walking
    // the full list is safe even though parents take their children with them.
    for (wi::ecs::Entity entity : loaded.entities)
        target.Entity_Remove(entity);
    loaded.Clear();
}

} // namespace st
