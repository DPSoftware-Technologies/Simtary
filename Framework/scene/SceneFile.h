#pragma once
// SceneFile - load a map file so its contents land at the ROOT of the hierarchy.
//
// wi::scene::LoadModel(..., attached = true) parents everything it reads to one freshly
// created entity, which is why a loaded map shows up in the Hierarchy as a single
// nameless "Entity 3" row that has to be expanded before anything is visible. That root
// is only useful when the file is being IMPORTED as a model into an existing scene (the
// editor's drag-and-drop path still wants it). A map loaded AS THE SCENE wants the
// opposite: its own top-level nodes are the scene's top-level nodes.
//
// LoadSceneFileFlat() loads with attached = false - the engine still builds a temporary
// root to apply the placement matrix through, then detaches its children and deletes it -
// and hands back the handles the caller needs to tear the whole thing down again:
//
//     loaded_ = st::LoadSceneFileFlat(wi::scene::GetScene(), "assets/scenes/s1map.stsd");
//     ...
//     st::UnloadSceneFile(wi::scene::GetScene(), loaded_);
//
// `entities` is EVERY entity the file created, not just the roots. A map brings in mesh,
// material and animation-data entities that carry no transform and therefore no parent,
// so removing the roots alone would leak them.

#include "wiScene.h"
#include <string>
#include <vector>

namespace st {

// What one LoadSceneFileFlat() call produced. Keep it for the lifetime of the scene;
// UnloadSceneFile() consumes it.
struct LoadedSceneFile {
    std::string                  path;      // the file that was actually opened
    std::vector<wi::ecs::Entity> roots;     // top-level nodes, now at the hierarchy root
    std::vector<wi::ecs::Entity> entities;  // everything the file created, roots included
    bool                         ok = false;
    std::string                  error;     // set when ok is false

    void Clear () {
        path.clear();
        roots.clear();
        entities.clear();
        ok = false;
        error.clear();
    }
};

// True for the extensions this loader understands, lowercase and without the dot.
bool IsSceneFileExtension (const std::string& extensionLower);

// Turn "assets/scenes/s1map", "s1map.wiscene" or "assets/scenes/s1map.stsd" into the file
// that should actually be opened: the packed .stsd when the build produced one, the source
// .wiscene otherwise. Returns an empty string when neither exists.
std::string ResolveSceneFile (const std::string& pathOrStem,
                              const std::string& searchFolder = "assets/scenes");

// The scene name a file maps to: its stem, lowercased for comparison purposes only by
// the caller - the returned string keeps the file's own casing.
std::string SceneNameFromFile (const std::string& path);

// Load `path` into `target` with nothing wrapped around it. Dispatches on the extension:
// .stsd goes through st::AssetSystem (pack-aware), .wiscene straight through the engine.
// The file is read into a temporary scene and merged, exactly like the hand-written scene
// loads it replaces, so entity IDs survive and native components are not Start()ed twice.
LoadedSceneFile LoadSceneFileFlat (wi::scene::Scene& target,
                                   const std::string& path,
                                   const XMMATRIX& transform = XMMatrixIdentity(),
                                   wi::scene::LoadModelProgressCallback progress = nullptr);

// Remove everything a LoadSceneFileFlat() call added and reset `loaded`.
void UnloadSceneFile (wi::scene::Scene& target, LoadedSceneFile& loaded);

// Every entity that currently has at least one component in `scene`. Used to bracket a
// load, and by RuntimeScene to work out what it has to clean up.
void GatherSceneEntities (wi::scene::Scene& scene, std::vector<wi::ecs::Entity>& out);

} // namespace st
