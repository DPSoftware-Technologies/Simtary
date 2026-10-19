#include "scene/SceneSnapshot.h"
#include "scene/SceneFile.h"     // GatherSceneEntities
#include "wiPhysics.h"
#include "wiBacklog.h"
#include "wiTimer.h"

#include <cstring>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st {

namespace {

constexpr Scene::EntitySerializeFlags kFlags = Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES;

void WriteEntity (Scene& scene, Entity e, wi::Archive& archive) {
    archive.SetReadModeAndResetPos(false);
    wi::ecs::EntitySerializer seri;
    seri.allow_remap = false;
    scene.Entity_Serialize(archive, seri, e, kFlags);
}

bool SameBytes (const wi::Archive& a, const wi::Archive& b) {
    // GetSize() is the allocation; the written length is the write position.
    const size_t na = a.GetPos(), nb = b.GetPos();
    return na == nb && std::memcmp(a.GetData(), b.GetData(), na) == 0;
}

void ClearEntityComponents (Scene& scene, Entity e) {
    for (auto& kv : scene.componentLibrary.entries)
        if (kv.second.component_manager && kv.second.component_manager->Contains(e))
            kv.second.component_manager->Remove(e);
}

} // namespace

void SceneSnapshot::Clear () {
    entries_.clear();
    valid_ = false;
    bytes_ = 0;
}

void SceneSnapshot::GatherExcluded (Scene& scene, std::unordered_set<Entity>& out) const {
    for (size_t i = 0; i < scene.terrains.GetCount(); ++i) {
        const Entity terrain = scene.terrains.GetEntity(i);
        out.insert(terrain);
        const Entity group = scene.terrains[i].chunkGroupEntity;
        if (group != INVALID_ENTITY) out.insert(group);
        for (size_t h = 0; h < scene.hierarchy.GetCount(); ++h) {
            const Entity e = scene.hierarchy.GetEntity(h);
            if (scene.Entity_IsDescendant(e, terrain) ||
                (group != INVALID_ENTITY && scene.Entity_IsDescendant(e, group)))
                out.insert(e);
        }
    }
}

void SceneSnapshot::Capture (Scene& scene) {
    wi::Timer timer;
    Clear();
    std::vector<Entity> all;
    GatherSceneEntities(scene, all);
    std::unordered_set<Entity> excluded;
    GatherExcluded(scene, excluded);

    entries_.reserve(all.size());
    for (Entity e : all) {
        if (excluded.count(e)) continue;
        Entry& entry = entries_.emplace_back();
        entry.entity = e;
        WriteEntity(scene, e, entry.archive);
        bytes_ += entry.archive.GetPos();
    }
    valid_ = true;
    wi::backlog::post("PlayMode: snapshot of " + std::to_string(entries_.size()) + " entities (" +
                      std::to_string(bytes_ / 1024) + " KB) in " +
                      std::to_string((int)timer.elapsed_milliseconds()) + " ms");
}

SceneSnapshot::RestoreReport SceneSnapshot::Restore (Scene& scene) {
    RestoreReport r;
    if (!valid_) return r;
    wi::Timer timer;

    // Instances first: component state lives in them, and a restore that left them would
    //	start the next Play from wherever this one ended.
    scene.nativeComponents.Clear();

    std::unordered_set<Entity> captured;
    captured.reserve(entries_.size());
    for (const Entry& entry : entries_) captured.insert(entry.entity);
    std::unordered_set<Entity> excluded;
    GatherExcluded(scene, excluded);

    std::vector<Entity> now;
    GatherSceneEntities(scene, now);
    for (Entity e : now) {
        if (captured.count(e) || excluded.count(e)) continue;
        scene.Entity_Remove(e, false);
        ++r.removed;
    }

    wi::Archive current;
    for (Entry& entry : entries_) {
        bool exists = false;
        for (auto& kv : scene.componentLibrary.entries)
            if (kv.second.component_manager && kv.second.component_manager->Contains(entry.entity)) { exists = true; break; }
        if (exists) {
            WriteEntity(scene, entry.entity, current);
            if (SameBytes(current, entry.archive)) {
                ++r.unchanged;
                continue;
            }
            ClearEntityComponents(scene, entry.entity);
            ++r.restored;
        } else {
            ++r.revived;
        }
        entry.archive.SetReadModeAndResetPos(true);
        wi::ecs::EntitySerializer seri;
        seri.allow_remap = false;
        scene.Entity_Serialize(entry.archive, seri, INVALID_ENTITY, kFlags);
    }

    wi::physics::ResetPhysicsObjects(scene);
    Clear();
    wi::backlog::post("PlayMode: restored " + std::to_string(r.restored) + " changed, " +
                      std::to_string(r.revived) + " deleted-in-play, removed " +
                      std::to_string(r.removed) + " made-in-play, " + std::to_string(r.unchanged) +
                      " untouched, in " + std::to_string((int)timer.elapsed_milliseconds()) + " ms");
    return r;
}

void StepSceneFrozen (Scene& scene, const CameraComponent& camera) {
    // A scene that has only ever been updated with dt = 0 draws no meshes at all - the
    //	main scene never showed it because it always got real steps. So a frozen scene IS
    //	stepped, by a microsecond, with everything that would act on that step held back
    //	for the length of the call: physics off, and every script, animation, sound and
    //	video paused (sound and video play whatever dt is). What is left - springs,
    //	particles - moves 0.2 s per hour.
    std::vector<uint32_t> held;
    held.reserve(scene.scripts.GetCount() + scene.animations.GetCount() +
                 scene.sounds.GetCount() + scene.videos.GetCount());
    auto hold = [&held](auto& manager) {
        for (size_t i = 0; i < manager.GetCount(); ++i) {
            held.push_back(manager[i]._flags);
            manager[i]._flags &= ~1u; // PLAYING is bit 0 on all four
        }
    };
    size_t at = 0;
    auto restore = [&held, &at](auto& manager) {
        for (size_t i = 0; i < manager.GetCount() && at < held.size(); ++i)
            manager[i]._flags = held[at++];
    };
    hold(scene.scripts);
    hold(scene.animations);
    hold(scene.sounds);
    hold(scene.videos);

    // Scene::Update picks each object's LOD from scene.camera; left default, that view
    //	resolves to an empty LOD.
    scene.camera = camera;
    const bool physics = wi::physics::IsEnabled();
    wi::physics::SetEnabled(false);
    scene.Update(1e-6f);
    wi::physics::SetEnabled(physics);

    restore(scene.scripts);
    restore(scene.animations);
    restore(scene.sounds);
    restore(scene.videos);
}

} // namespace st
