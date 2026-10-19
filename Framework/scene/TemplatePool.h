#pragma once
// TemplatePool - reuse placed copies of a template instead of creating and destroying them.
//
// Instantiating a template is a component-library deserialize plus a scene merge, and a
// physics body build on the next update; removing one tears all of that down again. For
// things that come and go all the time - bullets, debris, AI soldiers, pickups - that churn
// is the cost, so a pool makes them once (Prewarm, ideally behind a loading screen) and then
// only PARKS and UNPARKS them.
//
// A parked copy is still in the scene but costs next to nothing:
//   - every LayerComponent in it is set to mask 0 - not drawn, no shadows, no lights, not
//     picked, not in a raycast that filters by layer;
//   - its rigid bodies are moved to a parking spot far below the world and put to sleep
//     (a sleeping body is skipped by the solver until something touches it, and nothing
//     is down there);
//   - native components are disabled through NCE_<id> = false (OnDisable fires, Update
//     stops), Lua scripts, animations and sounds stop, emitters pause, characters go
//     inactive.
// Spawn puts every one of those back exactly as it was before the copy was parked, moves
// the copy to the spawn point (bodies teleported, velocities zeroed, woken), and returns
// its root. A parked copy keeps its state - health, ammo, whatever a component holds - so a
// component that must start fresh should reset itself in OnEnable.
//
//     st::TemplatePool& pool = *st::TemplatePools::Get().Acquire("templates/bullet.sttd");
//     pool.Prewarm(scene, 64);
//     Entity b = pool.Spawn(scene, muzzle, aim);
//     ...
//     pool.Release(scene, b);
//
// Handles are checked on every use: a copy whose root no longer exists (scene unloaded,
// deleted in the editor) is dropped from the pool rather than handed out.
//
// No exceptions, no RTTI.

#include "scene/ObjectTemplate.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace st {

struct TemplatePoolStats {
    uint32_t active   = 0;  // spawned and in use
    uint32_t parked   = 0;  // ready for the next Spawn
    uint32_t created  = 0;  // copies ever instantiated
    uint32_t reused   = 0;  // spawns served from the parked set
    uint32_t recycled = 0;  // active copies taken back because the pool was at maxActive
};

class TemplatePool {
public:
    bool Load (const std::string& path, std::string* error = nullptr);
    bool IsLoaded () const { return template_.IsLoaded(); }
    const std::string& Path () const { return template_.Path(); }
    const std::string& Name () const { return template_.Name(); }

    // Make parked copies until there are `count` of them. Returns how many exist now.
    uint32_t Prewarm (wi::scene::Scene& scene, uint32_t count);

    // A copy at `position` / `rotation` (render space), parked one reused when there is
    // one. INVALID_ENTITY when the pool is at maxActive and recycleOldest is off.
    wi::ecs::Entity Spawn (wi::scene::Scene& scene, const XMFLOAT3& position,
                           const XMFLOAT4& rotation = XMFLOAT4(0, 0, 0, 1));

    // Park a copy this pool spawned. False when `root` is not one of its active copies.
    bool Release (wi::scene::Scene& scene, wi::ecs::Entity root);
    void ReleaseAll (wi::scene::Scene& scene);

    // Delete every copy, active and parked, from the scene.
    void Clear (wi::scene::Scene& scene);
    // Drop every copy WITHOUT touching the scene - it already lost them (a Play-mode
    //	restore, a reload). The template stays loaded.
    void Forget () { instances_.clear(); nextSlot_ = 0; }

    bool IsActive (wi::ecs::Entity root) const;
    TemplatePoolStats Stats () const;

    uint32_t maxActive     = 0;     // 0 = unlimited
    bool     recycleOldest = false; // at maxActive, Spawn takes back the oldest active copy

private:
    // What a park changed, so the spawn can put it back exactly.
    struct ParkState {
        std::vector<std::pair<wi::ecs::Entity, uint32_t>> layers;      // entity, original mask
        std::vector<wi::ecs::Entity>                      addedLayers; // had none; one was added
        struct Native { wi::ecs::Entity entity; std::string key; bool had; bool value; };
        std::vector<Native>                               natives;
        std::vector<std::pair<wi::ecs::Entity, uint32_t>> scripts, animations, sounds; // _flags
        std::vector<std::pair<wi::ecs::Entity, bool>>     emitters;   // was paused
        std::vector<std::pair<wi::ecs::Entity, bool>>     characters; // was active
        std::vector<std::pair<wi::ecs::Entity, bool>>     startDeactivated; // rigid body flag
    };
    struct Instance {
        wi::ecs::Entity              root = wi::ecs::INVALID_ENTITY;
        std::vector<wi::ecs::Entity> entities;   // every entity of the copy, root first
        uint32_t                     slot = 0;   // parking spot index
        uint64_t                     spawnSerial = 0;
        bool                         active = false;
        ParkState                    park;
    };

    Instance* Create (wi::scene::Scene& scene);
    void Park (wi::scene::Scene& scene, Instance& inst);
    void Unpark (wi::scene::Scene& scene, Instance& inst);
    void MoveTo (wi::scene::Scene& scene, Instance& inst, const XMFLOAT3& position,
                 const XMFLOAT4& rotation, bool wake);
    void DropDead (wi::scene::Scene& scene);

    ObjectTemplate template_;
    std::vector<std::unique_ptr<Instance>> instances_;
    uint32_t nextSlot_ = 0;
    uint64_t nextSerial_ = 1;
    TemplatePoolStats counters_;
};

// One pool per template path, so a script, a native component and the editor bridge asking
// for "templates/bullet.sttd" all share the same copies.
class TemplatePools {
public:
    static TemplatePools& Get ();

    // The pool for `path`, loading the template on first use. nullptr if it will not load.
    TemplatePool* Acquire (const std::string& path, std::string* error = nullptr);
    TemplatePool* Find (const std::string& path);
    // The pool that spawned `root`, if any.
    TemplatePool* Owner (wi::ecs::Entity root);

    // Delete every pooled copy and forget the pools. A scene unload makes the handles
    // meaningless; call this (or let the dead-handle check drop them lazily).
    void ClearAll (wi::scene::Scene& scene);
    // Pools are a Play-mode thing: Stop forgets every copy and keeps the templates loaded.
    void ForgetInstances () { for (auto& kv : pools_) kv.second->Forget(); }

    template <typename F>
    void ForEach (F&& f) { for (auto& kv : pools_) f(kv.first, *kv.second); }

private:
    std::unordered_map<std::string, std::unique_ptr<TemplatePool>> pools_;
};

} // namespace st
