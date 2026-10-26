// The "stDamage" native component - see damage/Damage.h.

#include "damage/Damage.h"

#include "stNativeComponent.h"
#include "input/InputComponent.h"
#if SIMTARY_HAS_JSBSIM
#include "flight/FlightComponent.h"
#endif

#include "wiScene.h"
#include "wiPhysics.h"
#include "wiBacklog.h"
#include "wiRandom.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace wi::scene;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st {

namespace {

st::damage::PartBrokenHandler g_handler;

XMMATRIX WorldOf (const Scene& scene, Entity e) {
    const TransformComponent* t = scene.transforms.GetComponent(e);
    return t ? XMLoadFloat4x4(&t->world) : XMMatrixIdentity();
}

XMFLOAT3 Cross (const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

} // namespace

struct DamageComponent : NativeComponent {
    // Debris bodies and the scene hierarchy are main-thread work.
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    std::string parts = "TailRotor,Blade_1,Blade_2,Blade_3,Blade_4,Stabilizer,EngineDeck,"
                        "Wheel_Main_L,Wheel_Main_R,Wheel_Tail";
    float partHealth = 100.0f;
    float maxEjectSpeed = 60.0f;    // m/s, cap on how fast debris leaves the vehicle (a blade tip is faster)
    float debrisDensity = 15.0f;    // kg per m2 of the part's surface: skin plus the structure under it
                                    //	(a UH-60 main blade comes out ~120 kg, the real one is 116)
    bool  crashDamage = true;       // damage from touchdowns and rotor strikes (needs a sibling stFlight)
    float hardLanding = 3.0f;       // m/s sink at touchdown: below this, no damage
    float crashSpeed = 10.0f;       // m/s sink at touchdown: at or above this, the vehicle is destroyed

    struct Part {
        std::string name;
        float health = 100.0f;
        bool broken = false;
        Entity debris = INVALID_ENTITY;
    };
    std::vector<Part> parts_;

    static std::vector<DamageComponent*>& Registry () {
        static std::vector<DamageComponent*> all;
        return all;
    }

    void Start () override {
        Bind(parts, "parts");
        Bind(partHealth, "partHealth");
        Bind(maxEjectSpeed, "maxEjectSpeed");
        Bind(debrisDensity, "debrisDensity");
        Bind(crashDamage, "crashDamage");
        Bind(hardLanding, "hardLanding");
        Bind(crashSpeed, "crashSpeed");
        parts_.clear();
        std::stringstream ss(parts);
        std::string name;
        while (std::getline(ss, name, ',')) {
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name.empty()) continue;
            Part p; p.name = name; p.health = partHealth;
            parts_.push_back(p);
        }
        Registry().push_back(this);
        if (const TransformComponent* t = GetComponent<TransformComponent>()) lastPos_ = t->GetPosition();
    }

    void Destroy () override {
        auto& r = Registry();
        r.erase(std::remove(r.begin(), r.end(), this), r.end());
        // The wreckage belongs to the vehicle: made at runtime, nothing else would ever remove
        //	it - a scene reload unloads only what came out of the map file.
        if (scene != nullptr) {
            for (Part& p : parts_) {
                if (p.debris != INVALID_ENTITY && scene->transforms.Contains(p.debris)) scene->Entity_Remove(p.debris, true);
                p.debris = INVALID_ENTITY;
            }
        }
    }

    void Update (float dt) override {
        if (const TransformComponent* t = GetComponent<TransformComponent>()) {
            const XMFLOAT3 p = t->GetPosition();
            if (dt > 0.0f) velocity_ = XMFLOAT3((p.x - lastPos_.x) / dt, (p.y - lastPos_.y) / dt, (p.z - lastPos_.z) / dt);
            lastPos_ = p;
        }
        age_ += dt;
#if SIMTARY_HAS_JSBSIM
        if (crashDamage && !destroyed_) CheckCrash();
#endif
        // Debug key: break the next intact part.
        if (input_ == nullptr) {
            input_ = GetComponent<st::InputComponent>();
            if (input_ != nullptr) idBreak_ = input_->Find("BreakPart");
        }
        if (input_ != nullptr && idBreak_ >= 0 && input_->Pressed(idBreak_)) {
            for (const Part& p : parts_) {
                if (!p.broken && Break(p.name)) break;
            }
        }
        // A new body exists only after the physics system has run: give it its velocity then.
        for (size_t i = 0; i < pending_.size();) {
            Pending& k = pending_[i];
            RigidBodyPhysicsComponent* rb = scene->rigidbodies.GetComponent(k.debris);
            if (rb != nullptr && rb->physicsobject) {
                wi::physics::SetLinearVelocity(*rb, k.velocity);
                wi::physics::SetAngularVelocity(*rb, k.spin);
                pending_.erase(pending_.begin() + i);
            } else if (rb == nullptr || ++k.frames > 60) {
                pending_.erase(pending_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    Part* Find (const std::string& name) {
        for (Part& p : parts_) if (p.name == name) return &p;
        return nullptr;
    }

    bool Damage (const std::string& name, float amount) {
        Part* p = Find(name);
        if (p == nullptr || p->broken) return false;
        p->health -= amount;
        if (p->health <= 0.0f) Break(name);
        return true;
    }

    // Centre of the part's meshes in scene space, or false when it has none.
    bool PartCentre (Entity part, XMFLOAT3& out) const {
        XMFLOAT3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        bool any = false;
        for (size_t i = 0; i < scene->objects.GetCount(); ++i) {
            const Entity oe = scene->objects.GetEntity(i);
            if (oe != part && !scene->Entity_IsDescendant(oe, part)) continue;
            if (i >= scene->aabb_objects.size()) break;
            const wi::primitive::AABB* box = &scene->aabb_objects[i];   // parallel to objects
            lo = XMFLOAT3(std::min(lo.x, box->_min.x), std::min(lo.y, box->_min.y), std::min(lo.z, box->_min.z));
            hi = XMFLOAT3(std::max(hi.x, box->_max.x), std::max(hi.y, box->_max.y), std::max(hi.z, box->_max.z));
            any = true;
        }
        if (any) out = XMFLOAT3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
        return any;
    }

    bool Break (const std::string& name) {
        Part* p = Find(name);
        if (p == nullptr || p->broken) return false;
        const Entity part = scene->Entity_FindByName(name, entity);
        if (part == INVALID_ENTITY) {
            wi::backlog::post("stDamage: no part named '" + name + "' under this entity", wi::backlog::LogLevel::Warning);
            p->broken = true;   // nothing to break; do not ask again
            return false;
        }

        // What the part was doing, before it leaves the hierarchy.
        const XMMATRIX partWorld = WorldOf(*scene, part);
        XMFLOAT3 centre;
        if (!PartCentre(part, centre)) XMStoreFloat3(&centre, partWorld.r[3]);
        XMFLOAT3 spin(0, 0, 0), hub(0, 0, 0);
        bool tail = false, blade = false, stabilizer = false, engine = false;
#if SIMTARY_HAS_JSBSIM
        FlightComponent* flight = GetComponent<FlightComponent>();
        if (flight != nullptr) {
            const Entity mainRotor = scene->Entity_FindByName(flight->mainRotorNode, entity);
            tail = name == flight->tailRotorNode;
            blade = mainRotor != INVALID_ENTITY && scene->Entity_IsDescendant(part, mainRotor) &&
                    name.rfind("Blade", 0) == 0;
            if (tail) {
                spin = flight->TailRotorSpin();
                XMStoreFloat3(&hub, partWorld.r[3]);
            } else if (blade) {
                spin = flight->MainRotorSpin();
                XMStoreFloat3(&hub, WorldOf(*scene, mainRotor).r[3]);
            }
        }
#endif
        stabilizer = name.find("Stabilizer") != std::string::npos;
        engine = name.find("Engine") != std::string::npos;

        // Velocity at the part's centre: the vehicle's, the rotor's spin, and a kick outward.
        const XMFLOAT3 arm(centre.x - hub.x, centre.y - hub.y, centre.z - hub.z);
        const XMFLOAT3 swirl = (tail || blade) ? Cross(spin, arm) : XMFLOAT3(0, 0, 0);
        XMFLOAT3 away(centre.x - lastPos_.x, 0.0f, centre.z - lastPos_.z);
        const float awayLen = std::sqrt(away.x * away.x + away.z * away.z);
        if (awayLen > 1e-3f) { away.x /= awayLen; away.z /= awayLen; }
        XMFLOAT3 rel(swirl.x + away.x * 3.0f, swirl.y + 2.0f, swirl.z + away.z * 3.0f);
        const float relLen = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (relLen > maxEjectSpeed) {
            const float k = maxEjectSpeed / relLen;
            rel = XMFLOAT3(rel.x * k, rel.y * k, rel.z * k);
        }
        const XMFLOAT3 velocity(velocity_.x + rel.x, velocity_.y + rel.y, velocity_.z + rel.z);
        if (!(tail || blade)) {
            spin = XMFLOAT3(wi::random::GetRandom(-3.0f, 3.0f), wi::random::GetRandom(-3.0f, 3.0f),
                            wi::random::GetRandom(-3.0f, 3.0f));
        }

        // A new root exactly where the part is, and the part under it.
        const Entity debris = scene->Entity_CreateTransform("Debris_" + name);
        if (TransformComponent* dt = scene->transforms.GetComponent(debris)) {
            XMVECTOR s, r, t;
            XMMatrixDecompose(&s, &r, &t, partWorld);
            XMStoreFloat3(&dt->scale_local, s);
            XMStoreFloat4(&dt->rotation_local, r);
            XMStoreFloat3(&dt->translation_local, t);
            dt->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
            dt->SetDirty();
            dt->UpdateTransform();
        }
        scene->Component_Detach(part);
        scene->Component_Attach(part, debris);

        // Collision: a convex hull of the part's own vertices, in the debris root's space.
        const XMMATRIX toDebris = XMMatrixInverse(nullptr, WorldOf(*scene, debris));
        const Entity hull = scene->Entity_CreateTransform("DebrisHull_" + name);
        MeshComponent& mesh = scene->meshes.Create(hull);
        double area = 0.0;   // m2, for the mass
        for (size_t i = 0; i < scene->objects.GetCount(); ++i) {
            const Entity oe = scene->objects.GetEntity(i);
            if (oe != part && !scene->Entity_IsDescendant(oe, part)) continue;
            const MeshComponent* src = scene->meshes.GetComponent(scene->objects[i].meshID);
            if (src == nullptr) continue;
            const XMMATRIX m = WorldOf(*scene, oe) * toDebris;
            const size_t first = mesh.vertex_positions.size();
            for (const XMFLOAT3& v : src->vertex_positions) {
                XMFLOAT3 q;
                XMStoreFloat3(&q, XMVector3Transform(XMLoadFloat3(&v), m));
                mesh.vertex_positions.push_back(q);
            }
            // LOD 0 only: further levels repeat the same surface.
            uint32_t indexCount = uint32_t(src->indices.size());
            if (src->subsets_per_lod > 0 && !src->subsets.empty()) {
                indexCount = 0;
                for (uint32_t s = 0; s < src->subsets_per_lod && s < src->subsets.size(); ++s)
                    indexCount = std::max(indexCount, src->subsets[s].indexOffset + src->subsets[s].indexCount);
            }
            for (uint32_t t = 0; t + 2 < indexCount && t + 2 < src->indices.size(); t += 3) {
                const XMVECTOR a = XMLoadFloat3(&mesh.vertex_positions[first + src->indices[t]]);
                const XMVECTOR b = XMLoadFloat3(&mesh.vertex_positions[first + src->indices[t + 1]]);
                const XMVECTOR c = XMLoadFloat3(&mesh.vertex_positions[first + src->indices[t + 2]]);
                area += 0.5 * XMVectorGetX(XMVector3Length(XMVector3Cross(b - a, c - a)));
            }
        }
        scene->Component_Attach(hull, debris, true);

        RigidBodyPhysicsComponent& rb = scene->rigidbodies.Create(debris);
        if (!mesh.vertex_positions.empty()) {
            rb.shape = RigidBodyPhysicsComponent::CollisionShape::CONVEX_HULL;
            rb.mesh_source = RigidBodyPhysicsComponent::MeshSource::ProxyEntity;
            rb.collision_mesh_entity = hull;
            rb.mass = std::clamp(float(area) * debrisDensity, 5.0f, 600.0f);
        } else {
            rb.shape = RigidBodyPhysicsComponent::CollisionShape::SPHERE;
            rb.sphere.radius = 0.3f;
            rb.mass = 10.0f;
        }
        rb.friction = 0.6f;
        rb.restitution = 0.2f;
        pending_.push_back({ debris, velocity, spin, 0 });

        p->broken = true;
        p->health = 0.0f;
        p->debris = debris;
        wi::backlog::post("stDamage: '" + name + "' broke off (" + std::to_string(int(rb.mass)) + " kg debris)");

#if SIMTARY_HAS_JSBSIM
        if (flight != nullptr && (tail || blade || stabilizer || engine)) {
            FlightComponent::Damage d = flight->GetDamage();
            if (tail) d.tailRotorLost = true;
            if (blade) d.mainBladesLost++;
            if (stabilizer) d.stabilizerLost = true;
            if (engine) d.engineFailed = true;
            flight->SetDamage(d);
        }
#endif
        if (g_handler && !quiet_) {
            st::damage::PartBrokenEvent e;
            e.scene = scene; e.owner = entity; e.debris = debris; e.part = name; e.position = centre; e.engine = engine;
            g_handler(e);
        }
        return true;
    }

    // The whole vehicle: every part comes off and the game gets one big event, not ten small ones.
    void DestroyVehicle (const char* why) {
        if (destroyed_) return;
        destroyed_ = true;
        wi::backlog::post(std::string("stDamage: destroyed (") + why + ")", wi::backlog::LogLevel::Warning);
        XMFLOAT3 centre = lastPos_;
        centre.y += 1.5f;
        quiet_ = true;
        for (size_t i = 0; i < parts_.size(); ++i)
            if (!parts_[i].broken) Break(parts_[i].name);
        quiet_ = false;
#if SIMTARY_HAS_JSBSIM
        if (FlightComponent* flight = GetComponent<FlightComponent>()) {
            FlightComponent::Damage d = flight->GetDamage();
            d.engineFailed = true;
            flight->SetDamage(d);
        }
#endif
        if (g_handler) {
            st::damage::PartBrokenEvent e;
            e.scene = scene; e.owner = entity; e.part = "Crash"; e.position = centre; e.engine = true; e.crash = true;
            g_handler(e);
        }
    }

#if SIMTARY_HAS_JSBSIM
    // A blade's tip in its own object space: the vertex farthest from the rotor hub.
    bool BladeTip (Entity blade, const XMVECTOR& hubWorld, XMFLOAT3& tipLocal) const {
        const ObjectComponent* obj = scene->objects.GetComponent(blade);
        const MeshComponent* mesh = obj ? scene->meshes.GetComponent(obj->meshID) : nullptr;
        if (mesh == nullptr || mesh->vertex_positions.empty()) return false;
        const XMMATRIX w = WorldOf(*scene, blade);
        float best = -1.0f;
        for (const XMFLOAT3& v : mesh->vertex_positions) {
            const float d = XMVectorGetX(XMVector3Length(XMVector3Transform(XMLoadFloat3(&v), w) - hubWorld));
            if (d > best) { best = d; tipLocal = v; }
        }
        return true;
    }

    void CheckCrash () {
        FlightComponent* flight = GetComponent<FlightComponent>();
        if (flight == nullptr || !flight->ready || age_ < 2.0f) return;   // let it settle after spawning
        const FlightComponent::Telemetry& tm = flight->telemetry;

        // Touchdown: the moment the gear comes within a few cm of the ground, judged by how it
        //	was moving just before (JSBSim's sprung contacts spread the stop over several frames).
        const bool low = tm.agl < 0.3;
        const bool upsideDown = std::abs(tm.bank) > 100.0 || std::abs(tm.pitch) > 80.0;
        if (low && upsideDown) { DestroyVehicle("came down on its back"); return; }
        if (low && !wasLow_) {
            const float sink = std::max(0.0f, -prevClimb_);
            const float slide = prevGroundSpeed_;
            if (sink >= crashSpeed || slide >= crashSpeed * 2.5f) {
                char why[96];
                std::snprintf(why, sizeof(why), "hit the ground at %.1f m/s down, %.1f m/s along", sink, slide);
                DestroyVehicle(why);
                return;
            }
            if (sink > hardLanding) {
                const float f = (sink - hardLanding) / std::max(0.1f, crashSpeed - hardLanding);
                wi::backlog::post("stDamage: hard landing, " + std::to_string(sink).substr(0, 4) + " m/s");
                for (Part& p : parts_) {
                    if (p.broken) continue;
                    float hit = 0.0f;
                    if (p.name.find("Wheel") != std::string::npos || p.name.find("Gear") != std::string::npos) hit = 130.0f * f;
                    else if (p.name.find("Stabilizer") != std::string::npos) hit = 40.0f * f;
                    else if (p.name.rfind("Blade", 0) == 0) hit = 30.0f * f;   // blades droop into their stops
                    if (hit > 0.0f) Damage(p.name, hit);
                }
            }
        }
        wasLow_ = low;
        prevClimb_ = float(tm.climb);
        prevGroundSpeed_ = float(tm.groundSpeed);

        // Rotor strikes: only a turning rotor shatters.
        if (tm.rotorRpm > 30.0) {
            const Entity mainRotor = scene->Entity_FindByName(flight->mainRotorNode, entity);
            if (mainRotor != INVALID_ENTITY) {
                const XMVECTOR hub = WorldOf(*scene, mainRotor).r[3];
                bool struck = false;
                for (const Part& p : parts_) {
                    if (p.broken || p.name.rfind("Blade", 0) != 0) continue;
                    const Entity blade = scene->Entity_FindByName(p.name, entity);
                    if (blade == INVALID_ENTITY || !scene->Entity_IsDescendant(blade, mainRotor)) continue;
                    auto it = tips_.find(blade);
                    if (it == tips_.end()) {
                        XMFLOAT3 tip;
                        if (!BladeTip(blade, hub, tip)) continue;
                        it = tips_.emplace(blade, tip).first;
                    }
                    XMFLOAT3 w;
                    XMStoreFloat3(&w, XMVector3Transform(XMLoadFloat3(&it->second), WorldOf(*scene, blade)));
                    if (w.y < float(flight->GroundHeight(w.x, w.z)) + 0.05f) { struck = true; break; }
                }
                if (struck) {
                    wi::backlog::post("stDamage: main rotor strike", wi::backlog::LogLevel::Warning);
                    for (size_t i = 0; i < parts_.size(); ++i)
                        if (!parts_[i].broken && parts_[i].name.rfind("Blade", 0) == 0) Break(parts_[i].name);
                }
            }
        }
        if (tm.tailRpm > 30.0) {
            const Entity tail = scene->Entity_FindByName(flight->tailRotorNode, entity);
            Part* tp = Find(flight->tailRotorNode);
            if (tail != INVALID_ENTITY && tp != nullptr && !tp->broken) {
                if (tailRadius_ < 0.0f) {
                    // the farthest vertex of anything under the tail rotor node from its hub
                    tailRadius_ = 0.0f;
                    const XMVECTOR hub = WorldOf(*scene, tail).r[3];
                    for (size_t i = 0; i < scene->objects.GetCount(); ++i) {
                        const Entity oe = scene->objects.GetEntity(i);
                        if (!scene->Entity_IsDescendant(oe, tail)) continue;
                        const MeshComponent* m = scene->meshes.GetComponent(scene->objects[i].meshID);
                        if (m == nullptr) continue;
                        const XMMATRIX w = WorldOf(*scene, oe);
                        for (const XMFLOAT3& v : m->vertex_positions)
                            tailRadius_ = std::max(tailRadius_, XMVectorGetX(XMVector3Length(XMVector3Transform(XMLoadFloat3(&v), w) - hub)));
                    }
                }
                XMFLOAT3 hub;
                XMStoreFloat3(&hub, WorldOf(*scene, tail).r[3]);
                if (hub.y - tailRadius_ < float(flight->GroundHeight(hub.x, hub.z))) {
                    wi::backlog::post("stDamage: tail rotor strike", wi::backlog::LogLevel::Warning);
                    Break(tp->name);
                }
            }
        }
    }
#endif

    void ExplosionAt (const XMFLOAT3& at, float kg) {
        // Reach grows with the cube root of the charge, like the blast itself (10 kg: ~8.6 m).
        const float reach = 4.0f * std::cbrt(std::max(kg, 0.0f));
        if (reach <= 0.0f) return;
        std::vector<std::pair<std::string, float>> hits;
        for (const Part& p : parts_) {
            if (p.broken) continue;
            const Entity part = scene->Entity_FindByName(p.name, entity);
            XMFLOAT3 c;
            if (part == INVALID_ENTITY || !PartCentre(part, c)) continue;
            const float d = std::sqrt((c.x - at.x) * (c.x - at.x) + (c.y - at.y) * (c.y - at.y) + (c.z - at.z) * (c.z - at.z));
            if (d < reach) hits.push_back({ p.name, 250.0f * (1.0f - d / reach) });
        }
        // Breaking a part reparents it and adds entities: apply after the scan.
        for (const auto& h : hits) Damage(h.first, h.second);
    }

    void DrawDebug () override {
        ImGui::TextDisabled("Parts (break = leave the vehicle as debris)");
        for (const Part& p : parts_) {
            ImGui::PushID(p.name.c_str());
            if (p.broken) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%-14s broken", p.name.c_str());
            } else {
                ImGui::Text("%-14s %5.0f", p.name.c_str(), p.health);
                ImGui::SameLine();
                if (ImGui::SmallButton("Break")) Break(p.name);
            }
            ImGui::PopID();
        }
    }

private:
    struct Pending { Entity debris; XMFLOAT3 velocity; XMFLOAT3 spin; int frames; };
    std::vector<Pending> pending_;
    XMFLOAT3 lastPos_ = XMFLOAT3(0, 0, 0);
    XMFLOAT3 velocity_ = XMFLOAT3(0, 0, 0);
    st::InputComponent* input_ = nullptr;
    int idBreak_ = -1;
    float age_ = 0.0f;
    bool destroyed_ = false, quiet_ = false;
    bool wasLow_ = true;            // spawned on the ground
    float prevClimb_ = 0.0f, prevGroundSpeed_ = 0.0f;
    std::unordered_map<Entity, XMFLOAT3> tips_;   // blade -> tip in its object space
    float tailRadius_ = -1.0f;
};

namespace damage {

namespace {
DamageComponent* FindOn (Scene& scene, Entity owner) {
    for (DamageComponent* c : DamageComponent::Registry())
        if (c->scene == &scene && c->entity == owner) return c;
    return nullptr;
}
} // namespace

void SetPartBrokenHandler (PartBrokenHandler handler) { g_handler = std::move(handler); }

void ApplyExplosion (Scene& scene, const XMFLOAT3& at, float kg) {
    // Copy: a break can run the handler, which may create or destroy components.
    const std::vector<DamageComponent*> all = DamageComponent::Registry();
    for (DamageComponent* c : all)
        if (c->scene == &scene) c->ExplosionAt(at, kg);
}

bool BreakPart (Scene& scene, Entity owner, const std::string& part, std::string* error) {
    DamageComponent* c = FindOn(scene, owner);
    if (c == nullptr) { if (error) *error = "no running stDamage on that entity (Play first)"; return false; }
    if (!c->Break(part)) { if (error) *error = "no intact part '" + part + "'"; return false; }
    return true;
}

bool DamagePart (Scene& scene, Entity owner, const std::string& part, float amount, std::string* error) {
    DamageComponent* c = FindOn(scene, owner);
    if (c == nullptr) { if (error) *error = "no running stDamage on that entity (Play first)"; return false; }
    if (!c->Damage(part, amount)) { if (error) *error = "no intact part '" + part + "'"; return false; }
    return true;
}

std::string Status (Scene& scene, Entity owner) {
    DamageComponent* c = FindOn(scene, owner);
    if (c == nullptr) return "no running stDamage";
    std::string s;
    for (const DamageComponent::Part& p : c->parts_) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s  %.0f%s\n", p.name.c_str(), p.health, p.broken ? "  [broken]" : "");
        s += line;
    }
    return s;
}

} // namespace damage

} // namespace st

using StDamageComponent = st::DamageComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StDamageComponent, "stDamage", "Vehicle")
