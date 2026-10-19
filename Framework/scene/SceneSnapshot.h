#pragma once
// SceneSnapshot - what Play mode puts back when it stops (Unity's play-mode semantics).
//
// Capture() writes every entity of the scene into its own in-memory archive with its
// entity id kept (allow_remap = false), so the editor's selection, its undo history and a
// C++ scene's own entity handles all still mean the same thing after a restore. No file,
// no resource bytes - textures and meshes are referenced by name, as in any save.
//
// Restore() is a DIFF, not a reload:
//   - entities that exist now but were not captured were made during play: removed;
//   - every captured entity is serialized again and compared byte for byte with its
//     snapshot; only the ones that differ are cleared and read back. A 30 MB map in which
//     a vehicle drove around restores the vehicle, not the map - mesh data that did not
//     change is never rebuilt;
//   - every native component instance is destroyed (OnDisable / Destroy for the ones that
//     ran), because component state lives in the instance, not in the entity. They are
//     rebuilt from metadata, unstarted, by the next update - Edit mode does not run them;
//   - physics bodies are reset to where they were created.
//
// Terrain is left out of both, as Unity leaves TerrainData alone: its chunks and their
// GPU state are rebuilt from the authored data rather than restored (an archive-restored
// chunk renders black, see EditorUI::SaveScene), and a sculpt made while playing is kept.
//
// StepSceneFrozen() is how a scene that must not run is still drawn: Edit mode and the
// template preview both use it. See the comment in the .cpp for why the step is not 0.
//
// No exceptions, no RTTI.

#include "wiScene.h"
#include "wiArchive.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace st {

class SceneSnapshot {
public:
    void Capture (wi::scene::Scene& scene);
    struct RestoreReport {
        uint32_t restored = 0;  // changed during play, read back
        uint32_t removed  = 0;  // made during play, deleted
        uint32_t revived  = 0;  // deleted during play, recreated
        uint32_t unchanged = 0;
    };
    RestoreReport Restore (wi::scene::Scene& scene);

    bool Valid () const { return valid_; }
    void Clear ();
    size_t Bytes () const { return bytes_; }
    uint32_t Entities () const { return (uint32_t)entries_.size(); }

private:
    struct Entry {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        wi::Archive     archive;
    };
    void GatherExcluded (wi::scene::Scene& scene, std::unordered_set<wi::ecs::Entity>& out) const;

    std::vector<Entry> entries_;
    bool   valid_ = false;
    size_t bytes_ = 0;
};

// Step `scene` so it renders, without anything in it acting: physics off, every script,
// animation, sound and video paused for the length of the call, native components left
// as they are (callers turn their lifecycle off). `camera` stands in as scene.camera, which
// Scene::Update picks LODs from.
void StepSceneFrozen (wi::scene::Scene& scene, const wi::scene::CameraComponent& camera);

} // namespace st
