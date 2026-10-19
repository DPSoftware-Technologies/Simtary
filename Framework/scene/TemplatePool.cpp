#include "scene/TemplatePool.h"
#include "io/asset/StHash.h"
#include "wiPhysics.h"
#include "wiBacklog.h"

#include <algorithm>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st {

namespace {

// Far below anything a map builds, one 25 m cell per copy so parked bodies never touch.
XMFLOAT3 ParkingSpot (uint32_t slot) {
    return XMFLOAT3((float)(slot % 64) * 25.0f, -50000.0f, (float)(slot / 64) * 25.0f);
}

constexpr uint32_t kPlaying = 1u; // PLAYING is bit 0 on scripts, animations and sounds

// "templates/x.sttd", "assets/templates/x.sttd" and "Assets\Templates\X.sttd" are one pool.
std::string PoolKey (const std::string& path) {
    std::string key = asset::CanonicalPath(asset::NormalizePath(path));
    if (key.rfind("assets/", 0) == 0) key = key.substr(7);
    return key;
}

} // namespace

bool TemplatePool::Load (const std::string& path, std::string* error) {
    return template_.Load(path, error);
}

TemplatePool::Instance* TemplatePool::Create (Scene& scene) {
    auto inst = std::make_unique<Instance>();
    inst->slot = nextSlot_++;
    wi::vector<Entity> created;
    inst->root = template_.Instantiate(scene, ParkingSpot(inst->slot), XMFLOAT4(0, 0, 0, 1), &created);
    if (inst->root == INVALID_ENTITY) return nullptr;
    inst->entities.assign(created.begin(), created.end());
    ++counters_.created;
    // Parked from the start: a body that has not been built yet comes up asleep (see Park).
    Park(scene, *inst);
    instances_.push_back(std::move(inst));
    return instances_.back().get();
}

void TemplatePool::Park (Scene& scene, Instance& inst) {
    ParkState& p = inst.park;
    p = ParkState{};
    for (Entity e : inst.entities) {
        if (LayerComponent* layer = scene.layers.GetComponent(e)) {
            p.layers.emplace_back(e, layer->layerMask);
            layer->layerMask = 0;
        } else if (scene.objects.Contains(e) || scene.lights.Contains(e) || scene.emitters.Contains(e) ||
                   scene.hairs.Contains(e) || scene.decals.Contains(e)) {
            // No layer means "every layer" - give it one at mask 0 for the length of the park.
            scene.layers.Create(e).layerMask = 0;
            p.addedLayers.push_back(e);
        }
        if (MetadataComponent* m = scene.metadatas.GetComponent(e)) {
            for (size_t i = 0; i < m->string_values.names.size(); ++i) {
                const std::string& name = m->string_values.names[i];
                if (name.rfind("NCI_", 0) != 0) continue;
                const std::string key = "NCE_" + name.substr(4);
                ParkState::Native n{ e, key, m->bool_values.has(key), true };
                if (n.had) n.value = m->bool_values.get(key);
                p.natives.push_back(n);
            }
            for (const ParkState::Native& n : p.natives)
                if (n.entity == e) m->bool_values.set(n.key, false);
        }
        if (ScriptComponent* s = scene.scripts.GetComponent(e)) {
            p.scripts.emplace_back(e, s->_flags);
            s->_flags &= ~kPlaying;
        }
        if (AnimationComponent* a = scene.animations.GetComponent(e)) {
            p.animations.emplace_back(e, a->_flags);
            a->_flags &= ~kPlaying;
        }
        if (SoundComponent* s = scene.sounds.GetComponent(e)) {
            p.sounds.emplace_back(e, s->_flags);
            s->_flags &= ~kPlaying;
        }
        if (wi::EmittedParticleSystem* em = scene.emitters.GetComponent(e)) {
            p.emitters.emplace_back(e, em->IsPaused());
            em->SetPaused(true);
        }
        if (CharacterComponent* c = scene.characters.GetComponent(e)) {
            p.characters.emplace_back(e, c->IsActive());
            c->SetActive(false);
        }
        if (RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(e)) {
            // A body built while parked starts asleep instead of falling forever.
            p.startDeactivated.emplace_back(e, rb->IsStartDeactivated());
            rb->SetStartDeactivated(true);
        }
    }
    MoveTo(scene, inst, ParkingSpot(inst.slot), XMFLOAT4(0, 0, 0, 1), false);
    inst.active = false;
}

void TemplatePool::Unpark (Scene& scene, Instance& inst) {
    ParkState& p = inst.park;
    for (const auto& kv : p.layers)
        if (LayerComponent* layer = scene.layers.GetComponent(kv.first)) layer->layerMask = kv.second;
    for (Entity e : p.addedLayers)
        scene.layers.Remove(e);
    for (const ParkState::Native& n : p.natives) {
        MetadataComponent* m = scene.metadatas.GetComponent(n.entity);
        if (m == nullptr) continue;
        if (n.had) m->bool_values.set(n.key, n.value);
        else       m->bool_values.erase(n.key);
    }
    for (const auto& kv : p.scripts)
        if (ScriptComponent* s = scene.scripts.GetComponent(kv.first)) s->_flags = kv.second;
    for (const auto& kv : p.animations)
        if (AnimationComponent* a = scene.animations.GetComponent(kv.first)) a->_flags = kv.second;
    for (const auto& kv : p.sounds)
        if (SoundComponent* s = scene.sounds.GetComponent(kv.first)) s->_flags = kv.second;
    for (const auto& kv : p.emitters)
        if (wi::EmittedParticleSystem* em = scene.emitters.GetComponent(kv.first)) em->SetPaused(kv.second);
    for (const auto& kv : p.characters)
        if (CharacterComponent* c = scene.characters.GetComponent(kv.first)) c->SetActive(kv.second);
    for (const auto& kv : p.startDeactivated)
        if (RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(kv.first)) rb->SetStartDeactivated(kv.second);
    p = ParkState{};
}

void TemplatePool::MoveTo (Scene& scene, Instance& inst, const XMFLOAT3& position, const XMFLOAT4& rotation, bool wake) {
    if (TransformComponent* t = scene.transforms.GetComponent(inst.root)) {
        t->translation_local = position;
        t->rotation_local    = rotation;
        t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
        t->SetDirty();
        t->UpdateTransform();
    }
    // Bodies own their transforms while simulated, so they are teleported to where the
    //	hierarchy now puts them; one not built yet will be built from the transform.
    for (Entity e : inst.entities) {
        RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(e);
        if (rb == nullptr || !scene.transforms.Contains(e)) continue;
        XMVECTOR s, q, p;
        XMMatrixDecompose(&s, &q, &p, scene.ComputeEntityMatrixRecursive(e));
        XMFLOAT3 pos;
        XMFLOAT4 rot;
        XMStoreFloat3(&pos, p);
        XMStoreFloat4(&rot, q);
        wi::physics::SetPositionAndRotation(*rb, pos, rot);
        wi::physics::SetLinearVelocity(*rb, XMFLOAT3(0, 0, 0));
        wi::physics::SetAngularVelocity(*rb, XMFLOAT3(0, 0, 0));
        wi::physics::SetActivationState(*rb, wake ? wi::physics::ActivationState::Active
                                                  : wi::physics::ActivationState::Inactive);
    }
}

void TemplatePool::DropDead (Scene& scene) {
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
        [&scene](const std::unique_ptr<Instance>& i) { return !scene.transforms.Contains(i->root); }),
        instances_.end());
}

uint32_t TemplatePool::Prewarm (Scene& scene, uint32_t count) {
    if (!IsLoaded()) return 0;
    DropDead(scene);
    uint32_t parked = 0;
    for (const auto& i : instances_) parked += i->active ? 0 : 1;
    while (parked < count && Create(scene) != nullptr) ++parked;
    return parked;
}

Entity TemplatePool::Spawn (Scene& scene, const XMFLOAT3& position, const XMFLOAT4& rotation) {
    if (!IsLoaded()) return INVALID_ENTITY;
    DropDead(scene);

    Instance* use = nullptr;
    uint32_t active = 0;
    for (const auto& i : instances_) {
        if (i->active) ++active;
        else if (use == nullptr) use = i.get();
    }
    if (maxActive > 0 && active >= maxActive) {
        if (!recycleOldest) return INVALID_ENTITY;
        Instance* oldest = nullptr;
        for (const auto& i : instances_)
            if (i->active && (oldest == nullptr || i->spawnSerial < oldest->spawnSerial)) oldest = i.get();
        if (oldest == nullptr) return INVALID_ENTITY;
        Park(scene, *oldest);
        use = oldest;
        ++counters_.recycled;
    }
    if (use != nullptr) {
        ++counters_.reused;
    } else {
        use = Create(scene);
        if (use == nullptr) return INVALID_ENTITY;
    }

    Unpark(scene, *use);
    MoveTo(scene, *use, position, rotation, true);
    use->active = true;
    use->spawnSerial = nextSerial_++;
    return use->root;
}

bool TemplatePool::Release (Scene& scene, Entity root) {
    for (const auto& i : instances_) {
        if (i->root != root) continue;
        if (!i->active) return false;
        if (!scene.transforms.Contains(root)) {
            DropDead(scene);
            return false;
        }
        Park(scene, *i);
        return true;
    }
    return false;
}

void TemplatePool::ReleaseAll (Scene& scene) {
    DropDead(scene);
    for (const auto& i : instances_)
        if (i->active) Park(scene, *i);
}

void TemplatePool::Clear (Scene& scene) {
    for (const auto& i : instances_) {
        if (!scene.transforms.Contains(i->root)) continue;
        scene.Entity_Remove(i->root, true);
        // Anything the copy made that was not under the root (a template is built with all
        //	of it parented, but a component may have added more since).
        for (Entity e : i->entities)
            scene.Entity_Remove(e, false);
    }
    instances_.clear();
    nextSlot_ = 0;
}

bool TemplatePool::IsActive (Entity root) const {
    for (const auto& i : instances_)
        if (i->root == root) return i->active;
    return false;
}

TemplatePoolStats TemplatePool::Stats () const {
    TemplatePoolStats s = counters_;
    s.active = s.parked = 0;
    for (const auto& i : instances_) (i->active ? s.active : s.parked) += 1;
    return s;
}

// registry

TemplatePools& TemplatePools::Get () {
    static TemplatePools pools;
    return pools;
}

TemplatePool* TemplatePools::Find (const std::string& path) {
    const auto it = pools_.find(PoolKey(path));
    return it == pools_.end() ? nullptr : it->second.get();
}

TemplatePool* TemplatePools::Acquire (const std::string& path, std::string* error) {
    if (TemplatePool* existing = Find(path)) return existing;
    auto pool = std::make_unique<TemplatePool>();
    if (!pool->Load(path, error)) return nullptr;
    TemplatePool* raw = pool.get();
    pools_[PoolKey(path)] = std::move(pool);
    return raw;
}

TemplatePool* TemplatePools::Owner (Entity root) {
    for (auto& kv : pools_)
        if (kv.second->IsActive(root)) return kv.second.get();
    return nullptr;
}

void TemplatePools::ClearAll (Scene& scene) {
    for (auto& kv : pools_) kv.second->Clear(scene);
    pools_.clear();
}

} // namespace st
