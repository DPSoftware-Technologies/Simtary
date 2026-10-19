#include "net/VehicleSync.h"

#include "net/NetSession.h"
#include "devui/imcomponents.h"
#include "vehicle/VehicleComponent.h"

#include "wiBacklog.h"
#include "wiPhysics.h"
#include "wiScene.h"

#include <algorithm>
#include <string>

using namespace wi::ecs;
using namespace wi::scene;

namespace st::net {

namespace {

constexpr float kSnapshotInterval = 1.0f / 30.0f;
constexpr float kInputInterval = 1.0f / 60.0f;
constexpr float kInputTimeout = 1.0f;        // server: no input this long = hands off
constexpr float kTeleportDistance = 4.0f;    // client: further off than this = jump, not pull
constexpr float kSlotSpacing = 6.0f;         // metres between spawned vehicles

void Log (const std::string& msg) { wi::backlog::post("Net: " + msg); }

st::VehicleComponent* VehicleOf (Scene& scene, Entity e) {
    return static_cast<st::VehicleComponent*>(
        scene.nativeComponents.Get(e, wi::scene::GetNativeTypeID<st::VehicleComponent>()));
}

// Detach native components by registration name: drop NCI_<id> and every NCA_<id>_* key.
// Done on a fresh copy, before its instances exist, so nothing has to be torn down.
void StripNative (MetadataComponent& md, std::initializer_list<const char*> names) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < md.string_values.names.size(); ++i) {
        const std::string& key = md.string_values.names[i];
        if (key.rfind("NCI_", 0) != 0) continue;
        const std::string& value = md.string_values.values[i];
        for (const char* n : names) {
            if (value == n) ids.push_back(key.substr(4));
        }
    }
    for (const std::string& id : ids) {
        md.string_values.erase("NCI_" + id);
        const std::string prefix = "NCA_" + id + "_";
        auto eraseArgs = [&](auto& values) {
            std::vector<std::string> doomed;
            for (const std::string& k : values.names) {
                if (k.rfind(prefix, 0) == 0) doomed.push_back(k);
            }
            for (const std::string& k : doomed) values.erase(k);
        };
        eraseArgs(md.string_values);
        eraseArgs(md.float_values);
        eraseArgs(md.int_values);
        eraseArgs(md.bool_values);
    }
}

Entity FindTemplate (Scene& scene) {
    for (size_t i = 0; i < scene.metadatas.GetCount(); ++i) {
        const MetadataComponent& md = scene.metadatas[i];
        for (size_t k = 0; k < md.string_values.names.size(); ++k) {
            if (md.string_values.names[k].rfind("NCI_", 0) == 0 && md.string_values.values[k] == "stVehicle") {
                return scene.metadatas.GetEntity(i);
            }
        }
    }
    return INVALID_ENTITY;
}

} // namespace

void VehicleSync::Reset () {
    vehicles_.clear();
    template_ = wi::Archive();
    haveTemplate_ = false;
    nextSlot_ = 0;
}

void VehicleSync::DespawnAll () {
    Scene& scene = GetScene();
    for (const Vehicle& v : vehicles_) {
        if (scene.transforms.Contains(v.entity)) scene.Entity_Remove(v.entity, true);
    }
    vehicles_.clear();
}

bool VehicleSync::CaptureTemplate () {
    Scene& scene = GetScene();
    const Entity e = FindTemplate(scene);
    if (e == INVALID_ENTITY) return false;
    const TransformComponent* t = scene.transforms.GetComponent(e);
    if (t == nullptr) return false;
    templatePos_ = t->GetPosition();
    const XMMATRIX w = XMLoadFloat4x4(&t->world);
    XMStoreFloat3(&templateRight_, XMVector3Normalize(w.r[0]));

    // What Scene::Entity_Duplicate does, with the write kept: one archive, read per spawn.
    template_ = wi::Archive();
    template_.SetReadModeAndResetPos(false);
    EntitySerializer seri;
    scene.Entity_Serialize(template_, seri, e, Scene::EntitySerializeFlags::RECURSIVE);
    scene.Entity_Remove(e, true);
    haveTemplate_ = true;
    Log("vehicle template captured (" + std::to_string(e) + "), removed from the scene");
    return true;
}

Entity VehicleSync::Spawn (uint32_t netId, uint32_t owner, const XMFLOAT3& position) {
    Scene& scene = GetScene();
    template_.SetReadModeAndResetPos(true);
    EntitySerializer seri;
    const Entity root = scene.Entity_Serialize(template_, seri, INVALID_ENTITY,
        Scene::EntitySerializeFlags::RECURSIVE | Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
    if (root == INVALID_ENTITY) return root;
    ::PlaceEntityAt(scene, root, position);

    const bool dedicated = session_.IsServer() && !session_.HasLocalPlayer();
    const bool mine = owner != 0 && owner == session_.LocalClientId();
    if (MetadataComponent* md = scene.metadatas.GetComponent(root)) {
        if (dedicated) {
            StripNative(*md, { "stChaseCamera", "stVehicleHud", "stInput", "VehicleSound" });
        } else if (!mine) {
            StripNative(*md, { "stChaseCamera", "stVehicleHud", "stInput" });
        }
    }
    if (NameComponent* n = scene.names.GetComponent(root)) {
        n->name += mine ? " (you)" : " #" + std::to_string(owner);
    }

    Vehicle v;
    v.netId = netId;
    v.owner = owner;
    v.entity = root;
    vehicles_.push_back(v);
    Log("spawned vehicle " + std::to_string(netId) + " for client " + std::to_string(owner) + (mine ? " (local)" : ""));
    return root;
}

void VehicleSync::Despawn (uint32_t netId) {
    auto it = std::find_if(vehicles_.begin(), vehicles_.end(), [&](const Vehicle& v) { return v.netId == netId; });
    if (it == vehicles_.end()) return;
    GetScene().Entity_Remove(it->entity, true);
    Log("removed vehicle " + std::to_string(netId));
    vehicles_.erase(it);
}

VehicleSync::Vehicle* VehicleSync::Find (uint32_t netId) {
    for (Vehicle& v : vehicles_) {
        if (v.netId == netId) return &v;
    }
    return nullptr;
}

VehicleSync::Vehicle* VehicleSync::FindByOwner (uint32_t clientId) {
    for (Vehicle& v : vehicles_) {
        if (v.owner == clientId) return &v;
    }
    return nullptr;
}

void VehicleSync::Broadcast (const Writer& w, Delivery d) {
    Transport* t = session_.GetTransport();
    if (t == nullptr) return;
    for (const Peer& p : session_.Peers()) {
        if (p.clientId != 0) t->Send(p.connection, w.Bytes().data(), w.Bytes().size(), d);
    }
}

void VehicleSync::OnPlayerJoined (uint32_t clientId, ConnectionId connection) {
    if (!haveTemplate_ && !CaptureTemplate()) {
        Log("no vehicle in the scene to spawn from");
        return;
    }
    // The new player hears about everyone already here first (the host's own player has no
    // connection: it sees the scene directly) ...
    Transport* t = session_.GetTransport();
    Scene& scene = GetScene();
    for (const Vehicle& v : vehicles_) {
        if (connection == kNoConnection || t == nullptr) break;
        const TransformComponent* tr = scene.transforms.GetComponent(v.entity);
        const XMFLOAT3 p = tr != nullptr ? tr->GetPosition() : templatePos_;
        Writer w(MsgSpawn);
        w.U32(v.netId); w.U32(v.owner); w.F32(p.x); w.F32(p.y); w.F32(p.z);
        t->Send(connection, w.Bytes().data(), w.Bytes().size(), Delivery::Reliable);
    }
    // ... then everyone, it included, about its vehicle - side by side with the template's spot.
    const float offset = kSlotSpacing * float(nextSlot_++);
    const XMFLOAT3 pos(templatePos_.x + templateRight_.x * offset, templatePos_.y + 0.5f,
                       templatePos_.z + templateRight_.z * offset);
    const uint32_t netId = nextNetId_++;
    Spawn(netId, clientId, pos);
    Writer w(MsgSpawn);
    w.U32(netId); w.U32(clientId); w.F32(pos.x); w.F32(pos.y); w.F32(pos.z);
    Broadcast(w, Delivery::Reliable);
}

void VehicleSync::OnPlayerLeft (uint32_t clientId) {
    Vehicle* v = FindByOwner(clientId);
    if (v == nullptr) return;
    const uint32_t netId = v->netId;
    Despawn(netId);
    Writer w(MsgDespawn);
    w.U32(netId);
    Broadcast(w, Delivery::Reliable);
}

void VehicleSync::OnMessage (ConnectionId from, uint8_t type, Reader& r) {
    if (session_.IsServer()) {
        if (type != MsgInput) return;
        uint32_t clientId = 0;
        for (const Peer& p : session_.Peers()) {
            if (p.connection == from) clientId = p.clientId;
        }
        Vehicle* v = FindByOwner(clientId);
        if (v == nullptr) return;
        st::VehicleComponent::DriverInput in;
        in.throttle = r.F32();
        in.brake = r.F32();
        in.steer = r.F32();
        in.handbrake = r.U8() != 0;
        const uint8_t reset = r.U8();
        if (!r.ok()) return;
        if (st::VehicleComponent* vc = VehicleOf(GetScene(), v->entity)) {
            vc->remoteInput = true;
            vc->remote = in;
            if (reset != v->lastReset) vc->ResetUpright();
        }
        v->lastReset = reset;
        v->lastInputAge = 0.0f;
        return;
    }

    // client
    switch (type) {
        case MsgSpawn: {
            const uint32_t netId = r.U32();
            const uint32_t owner = r.U32();
            XMFLOAT3 p;
            p.x = r.F32(); p.y = r.F32(); p.z = r.F32();
            if (!r.ok() || Find(netId) != nullptr) break;
            if (!haveTemplate_ && !CaptureTemplate()) break;
            Spawn(netId, owner, p);
            break;
        }
        case MsgDespawn: {
            const uint32_t netId = r.U32();
            if (r.ok()) Despawn(netId);
            break;
        }
        case MsgSnapshot:
            ApplySnapshot(r);
            break;
        default:
            break;
    }
}

void VehicleSync::Update (float dt) {
    Scene& scene = GetScene();

    // The scene went away (reload, other scene): everything we spawned went with it.
    for (const Vehicle& v : vehicles_) {
        if (!scene.transforms.Contains(v.entity)) {
            Log("scene changed - vehicle sync reset");
            Reset();
            break;
        }
    }

    // Take the template out as soon as the scene has one, on both sides: a client must not
    // drive the map's own vehicle, and a dedicated server has nobody to drive it.
    if (!haveTemplate_) CaptureTemplate();

    if (session_.IsServer()) {
        // Integrated host: its own player gets a vehicle as soon as the scene has a template.
        if (session_.HasLocalPlayer() && haveTemplate_ && FindByOwner(session_.LocalClientId()) == nullptr) {
            OnPlayerJoined(session_.LocalClientId(), kNoConnection);
        }
        for (Vehicle& v : vehicles_) {
            v.lastInputAge += dt;
            st::VehicleComponent* vc = VehicleOf(scene, v.entity);
            if (vc == nullptr) continue;
            if (session_.HasLocalPlayer() && v.owner == session_.LocalClientId()) {
                vc->remoteInput = false;   // driven by this machine's own input
                continue;
            }
            vc->remoteInput = true;
            if (v.lastInputAge > kInputTimeout) vc->remote = {};
        }
        bool anyRemote = false;
        for (const Peer& p : session_.Peers()) anyRemote = anyRemote || p.clientId != 0;
        snapshotTimer_ += dt;
        if (snapshotTimer_ >= kSnapshotInterval && anyRemote) {
            snapshotTimer_ = std::fmod(snapshotTimer_, kSnapshotInterval);
            SendSnapshot();
        }
    } else if (session_.IsClient() && session_.IsReady()) {
        for (Vehicle& v : vehicles_) {
            if (v.owner == session_.LocalClientId()) continue;
            if (st::VehicleComponent* vc = VehicleOf(scene, v.entity)) vc->remoteInput = true;
        }
        inputTimer_ += dt;
        if (inputTimer_ >= kInputInterval) {
            inputTimer_ = std::fmod(inputTimer_, kInputInterval);
            SendInput();
        }
    }
}

void VehicleSync::SendInput () {
    Vehicle* own = FindByOwner(session_.LocalClientId());
    if (own == nullptr) return;
    st::VehicleComponent* vc = VehicleOf(GetScene(), own->entity);
    if (vc == nullptr) return;
    Writer w(MsgInput);
    w.F32(vc->wanted.throttle);
    w.F32(vc->wanted.brake);
    w.F32(vc->wanted.steer);
    w.U8(vc->wanted.handbrake ? 1 : 0);
    w.U8(vc->resetCount);
    const auto& peers = session_.Peers();
    if (!peers.empty()) {
        session_.GetTransport()->Send(peers[0].connection, w.Bytes().data(), w.Bytes().size(), Delivery::Unreliable);
    }
}

void VehicleSync::SendSnapshot () {
    Scene& scene = GetScene();
    Writer w(MsgSnapshot);
    w.U32(++tick_);
    std::vector<const Vehicle*> ready;
    for (const Vehicle& v : vehicles_) {
        const RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(v.entity);
        if (rb != nullptr && rb->physicsobject != nullptr) ready.push_back(&v);
    }
    w.U16(uint16_t(ready.size()));
    for (const Vehicle* v : ready) {
        RigidBodyPhysicsComponent& rb = *scene.rigidbodies.GetComponent(v->entity);
        const XMFLOAT3 p = wi::physics::GetPosition(rb);
        const XMFLOAT4 q = wi::physics::GetRotation(rb);
        const XMFLOAT3 lv = wi::physics::GetVelocity(rb);
        const XMFLOAT3 av = wi::physics::GetAngularVelocity(rb);
        w.U32(v->netId);
        w.F32(p.x); w.F32(p.y); w.F32(p.z);
        w.F32(q.x); w.F32(q.y); w.F32(q.z); w.F32(q.w);
        w.F32(lv.x); w.F32(lv.y); w.F32(lv.z);
        w.F32(av.x); w.F32(av.y); w.F32(av.z);
        st::VehicleComponent::DriverInput in;
        if (const st::VehicleComponent* vc = VehicleOf(scene, v->entity)) in = vc->wanted;
        w.F32(in.throttle); w.F32(in.brake); w.F32(in.steer); w.U8(in.handbrake ? 1 : 0);
    }
    Broadcast(w, Delivery::Unreliable);
}

void VehicleSync::ApplySnapshot (Reader& r) {
    Scene& scene = GetScene();
    r.U32();   // tick - ordering is not needed yet: unreliable newest-wins is close enough
    const uint16_t n = r.U16();
    const auto& peers = session_.Peers();
    const int ping = peers.empty() ? 0 : std::max(0, session_.GetTransport()->Stats(peers[0].connection).pingMs);
    const float lead = float(ping) * 0.0005f;   // half the round trip, seconds

    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        const uint32_t netId = r.U32();
        XMFLOAT3 p, lv, av;
        XMFLOAT4 q;
        p.x = r.F32(); p.y = r.F32(); p.z = r.F32();
        q.x = r.F32(); q.y = r.F32(); q.z = r.F32(); q.w = r.F32();
        lv.x = r.F32(); lv.y = r.F32(); lv.z = r.F32();
        av.x = r.F32(); av.y = r.F32(); av.z = r.F32();
        st::VehicleComponent::DriverInput in;
        in.throttle = r.F32(); in.brake = r.F32(); in.steer = r.F32(); in.handbrake = r.U8() != 0;
        if (!r.ok()) break;

        Vehicle* v = Find(netId);
        if (v == nullptr) continue;   // its Spawn has not arrived yet
        RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(v->entity);
        if (rb == nullptr || rb->physicsobject == nullptr) continue;
        const bool mine = v->owner == session_.LocalClientId();
        st::VehicleComponent* vc = VehicleOf(scene, v->entity);
        if (vc != nullptr && !mine) vc->remote = in;

        // Where the server's vehicle is NOW: the snapshot is half a round trip old.
        const XMVECTOR target = XMVectorAdd(XMLoadFloat3(&p), XMVectorScale(XMLoadFloat3(&lv), lead));
        const XMFLOAT3 curP = wi::physics::GetPosition(*rb);
        const XMFLOAT4 curQ = wi::physics::GetRotation(*rb);
        const XMFLOAT3 curLv = wi::physics::GetVelocity(*rb);
        const XMFLOAT3 curAv = wi::physics::GetAngularVelocity(*rb);
        const XMVECTOR here = XMLoadFloat3(&curP);
        const float error = XMVectorGetX(XMVector3Length(XMVectorSubtract(target, here)));
        XMFLOAT3 np, nlv, nav;
        XMFLOAT4 nq;
        if (error > kTeleportDistance) {
            XMStoreFloat3(&np, target);
            nq = q;
            nlv = lv;
            nav = av;
        } else {
            // Pull, do not jump: the own vehicle gently (it is already right, give or take),
            // other people's harder (they run on a delayed copy of the driver's input).
            const float k = mine ? 0.1f : 0.3f;
            XMStoreFloat3(&np, XMVectorLerp(here, target, k));
            XMStoreFloat4(&nq, XMQuaternionSlerp(XMLoadFloat4(&curQ), XMLoadFloat4(&q), k));
            XMStoreFloat3(&nlv, XMVectorLerp(XMLoadFloat3(&curLv), XMLoadFloat3(&lv), std::min(1.0f, k * 2.0f)));
            XMStoreFloat3(&nav, XMVectorLerp(XMLoadFloat3(&curAv), XMLoadFloat3(&av), std::min(1.0f, k * 2.0f)));
        }
        wi::physics::SetPositionAndRotation(*rb, np, nq);
        wi::physics::SetLinearVelocity(*rb, nlv);
        wi::physics::SetAngularVelocity(*rb, nav);
    }
}

} // namespace st::net
