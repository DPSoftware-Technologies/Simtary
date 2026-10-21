#include "net/Replication.h"
#include "net/NetSession.h"

#include "scene/ObjectTemplate.h"

#include "wiScene.h"
#include "wiBacklog.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st::net {

namespace {

constexpr float kSendInterval = 1.0f / 20.0f;   // transforms, both ways
constexpr float kKeyframeInterval = 1.0f;       // everything re-sent, unchanged or not
constexpr double kInterpDelay = 0.1;            // clients play back this far behind the server
constexpr size_t kMaxSamples = 32;
constexpr size_t kMaxPerMessage = 256;
constexpr float kStateInterval = 0.1f;          // component state, server -> clients

// The components replicated as state: the ones that hold no entity references, so their bytes
//	mean the same thing on every machine. Index = what MsgNetState carries.
const char* const kStateComponents[4] = {
    "wi::scene::Scene::layers",      // visibility
    "wi::scene::Scene::lights",
    "wi::scene::Scene::metadatas",   // native component parameters, EntityGUID
    "wi::scene::Scene::names",
};

void Log (const std::string& s, wi::backlog::LogLevel level = wi::backlog::LogLevel::Default) {
    wi::backlog::post("[net] " + s, level);
}

XMFLOAT4 Slerp (const XMFLOAT4& a, const XMFLOAT4& b, float t) {
    XMFLOAT4 r;
    XMStoreFloat4(&r, XMQuaternionSlerp(XMLoadFloat4(&a), XMLoadFloat4(&b), t));
    return r;
}

NetTransform Lerp (const NetTransform& a, const NetTransform& b, float t) {
    NetTransform r = b;
    r.px = a.px + (b.px - a.px) * t;
    r.py = a.py + (b.py - a.py) * t;
    r.pz = a.pz + (b.pz - a.pz) * t;
    r.rotation = Slerp(a.rotation, b.rotation, t);
    r.scale = XMFLOAT3(a.scale.x + (b.scale.x - a.scale.x) * t, a.scale.y + (b.scale.y - a.scale.y) * t,
                       a.scale.z + (b.scale.z - a.scale.z) * t);
    return r;
}

} // namespace

// -------------------------------------------------------------------- transform ---

NetTransform NetTransform::From (const wi::scene::TransformComponent& t) {
    NetTransform n;
    n.largeWorld = t.IsLargeWorld();
    if (n.largeWorld) {
        n.px = t.GetWorldPositionX();
        n.py = t.GetWorldPositionY();
        n.pz = t.GetWorldPositionZ();
    } else {
        n.px = t.translation_local.x;
        n.py = t.translation_local.y;
        n.pz = t.translation_local.z;
    }
    n.rotation = t.rotation_local;
    n.scale = t.scale_local;
    return n;
}

void NetTransform::ApplyTo (wi::scene::TransformComponent& t) const {
    if (largeWorld) {
        t.SetWorldPosition(px, py, pz);
    } else {
        t.translation_local = XMFLOAT3((float)px, (float)py, (float)pz);
    }
    t.rotation_local = rotation;
    t.scale_local = scale;
    t.SetDirty();
}

bool NetTransform::Near (const NetTransform& o) const {
    const double dp = std::abs(px - o.px) + std::abs(py - o.py) + std::abs(pz - o.pz);
    const float dot = std::abs(rotation.x * o.rotation.x + rotation.y * o.rotation.y +
                               rotation.z * o.rotation.z + rotation.w * o.rotation.w);
    const float ds = std::abs(scale.x - o.scale.x) + std::abs(scale.y - o.scale.y) + std::abs(scale.z - o.scale.z);
    return largeWorld == o.largeWorld && dp < 1e-3 && dot > 1.0f - 1e-8f && ds < 1e-4f;
}

void NetTransform::Write (Writer& w) const {
    const bool hasScale = scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f;
    w.U8(uint8_t((largeWorld ? 1 : 0) | (hasScale ? 2 : 0)));
    if (largeWorld) {
        w.F64(px); w.F64(py); w.F64(pz);
    } else {
        w.F32((float)px); w.F32((float)py); w.F32((float)pz);
    }
    w.F32(rotation.x); w.F32(rotation.y); w.F32(rotation.z); w.F32(rotation.w);
    if (hasScale) { w.F32(scale.x); w.F32(scale.y); w.F32(scale.z); }
}

bool NetTransform::Read (Reader& r) {
    const uint8_t flags = r.U8();
    largeWorld = flags & 1;
    if (largeWorld) {
        px = r.F64(); py = r.F64(); pz = r.F64();
    } else {
        px = r.F32(); py = r.F32(); pz = r.F32();
    }
    rotation.x = r.F32(); rotation.y = r.F32(); rotation.z = r.F32(); rotation.w = r.F32();
    if (flags & 2) { scale.x = r.F32(); scale.y = r.F32(); scale.z = r.F32(); }
    else scale = XMFLOAT3(1, 1, 1);
    return r.ok();
}

// ---------------------------------------------------------------------- values ---

void NetValue::Write (Writer& w) const {
    w.U8(uint8_t(type));
    switch (type) {
    case Type::Nil: break;
    case Type::Bool: w.U8(b ? 1 : 0); break;
    case Type::Int: w.U32(uint32_t(uint64_t(i) >> 32)); w.U32(uint32_t(i)); break;
    case Type::Float: w.F64(f); break;
    case Type::String: w.Str(s); break;
    case Type::Vector2: w.F32(v[0]); w.F32(v[1]); break;
    case Type::Vector3: w.F32(v[0]); w.F32(v[1]); w.F32(v[2]); break;
    case Type::Quaternion:
    case Type::Color: w.F32(v[0]); w.F32(v[1]); w.F32(v[2]); w.F32(v[3]); break;
    }
}

bool NetValue::Read (Reader& r) {
    *this = NetValue();
    type = Type(r.U8());
    switch (type) {
    case Type::Nil: break;
    case Type::Bool: b = r.U8() != 0; break;
    case Type::Int: {
        const uint64_t hi = r.U32(), lo = r.U32();
        i = int64_t((hi << 32) | lo);
        break;
    }
    case Type::Float: f = r.F64(); break;
    case Type::String: s = r.Str(); break;
    case Type::Vector2: v[0] = r.F32(); v[1] = r.F32(); break;
    case Type::Vector3: v[0] = r.F32(); v[1] = r.F32(); v[2] = r.F32(); break;
    case Type::Quaternion:
    case Type::Color: v[0] = r.F32(); v[1] = r.F32(); v[2] = r.F32(); v[3] = r.F32(); break;
    default: return false;
    }
    return r.ok();
}

bool NetValue::operator== (const NetValue& o) const {
    return type == o.type && b == o.b && i == o.i && f == o.f && s == o.s &&
           v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3];
}

// ---------------------------------------------------------------------- basics ---

Replication& Replication::Get () {
    static Replication r;
    return r;
}

double Replication::Now () const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

bool Replication::IsServer () const { return session_ != nullptr && session_->IsServer(); }
bool Replication::IsClient () const { return session_ != nullptr && session_->IsClient(); }
uint32_t Replication::LocalPlayer () const { return session_ ? session_->LocalClientId() : 0; }

Replication::Object* Replication::Find (Entity e) {
    for (Object& o : objects_) if (o.entity == e) return &o;
    return nullptr;
}

const Replication::Object* Replication::Find (Entity e) const {
    for (const Object& o : objects_) if (o.entity == e) return &o;
    return nullptr;
}

Replication::Object* Replication::FindNet (uint32_t netId) {
    if (netId == 0) return nullptr;
    for (Object& o : objects_) if (o.netId == netId) return &o;
    return nullptr;
}

// The same key on every machine that loaded the same map: the EntityGUID the editor stamped,
//	else the names from the root down ("Village/House 3/Door").
std::string Replication::KeyOf (wi::scene::Scene& scene, Entity e) const {
    if (const wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(e))
        if (m->string_values.has("EntityGUID")) return "guid:" + m->string_values.get("EntityGUID");
    std::string path;
    for (Entity x = e; x != INVALID_ENTITY;) {
        const wi::scene::NameComponent* n = scene.names.GetComponent(x);
        path = (n ? n->name : std::string("?")) + (path.empty() ? "" : "/" + path);
        const wi::scene::HierarchyComponent* h = scene.hierarchy.GetComponent(x);
        x = h ? h->parentID : INVALID_ENTITY;
    }
    return "path:" + path;
}

bool Replication::Drives (const Object& o) const {
    if (session_ == nullptr || !session_->IsOnline()) return true;
    const bool remoteAuthority = o.options.ownerAuthority && o.owner != 0;
    if (IsServer()) return !(remoteAuthority && o.owner != LocalPlayer());
    return remoteAuthority && o.owner == LocalPlayer();
}

// Something this machine does not drive must not simulate either: its rigid body becomes
//	kinematic (moved by the transform the network gives it) until that changes.
// Components that belong to whoever owns the object (input, camera, HUD, controller): on on the
//	owner's machine (and offline), off everywhere else. Written as the NCE_ enable flags, which
//	state replication leaves to each machine.
void Replication::UpdateLocalOnly (Object& o) {
    if (o.options.localOnly.empty()) return;
    const bool online = session_ != nullptr && session_->IsOnline();
    const int want = (!online || (o.owner != 0 && o.owner == LocalPlayer())) ? 1 : 0;
    if (want == o.localOnlyState) return;
    wi::scene::Scene& scene = wi::scene::GetScene();
    wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(o.entity);
    if (m == nullptr) return;   // its metadata may not be in yet - try again next frame
    o.localOnlyState = want;
    const std::string list = "," + o.options.localOnly + ",";
    for (size_t i = 0; i < m->string_values.names.size(); ++i) {
        const std::string& key = m->string_values.names[i];
        if (key.rfind("NCI_", 0) != 0) continue;
        const std::string name = m->string_values.get(i);
        if (list.find("," + name + ",") == std::string::npos) continue;
        m->bool_values.set("NCE_" + key.substr(4), want == 1);
    }
}

void Replication::UpdateProxy (Object& o) {
    UpdateLocalOnly(o);
    wi::scene::Scene& scene = wi::scene::GetScene();
    wi::scene::RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(o.entity);
    const bool want = rb != nullptr && o.options.syncTransform && !Drives(o);
    if (want && !o.proxy) {
        o.wasKinematic = rb->IsKinematic();
        rb->SetKinematic(true);
        rb->physicsobject = {};   // rebuilt as kinematic on the next physics update
        o.proxy = true;
    } else if (!want && o.proxy) {
        ReleaseProxy(o);
    }
}

void Replication::ReleaseProxy (Object& o) {
    if (!o.proxy) return;
    o.proxy = false;
    wi::scene::Scene& scene = wi::scene::GetScene();
    if (wi::scene::RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(o.entity)) {
        rb->SetKinematic(o.wasKinematic);
        rb->physicsobject = {};
    }
}

void Replication::SendTo (ConnectionId c, const Writer& w, Delivery d) {
    if (session_ && session_->GetTransport()) session_->GetTransport()->Send(c, w.Bytes().data(), w.Bytes().size(), d);
}

void Replication::Broadcast (const Writer& w, Delivery d) {
    if (session_) session_->Broadcast(w, d);
}

void Replication::WriteBind (Writer& w, const Object& o) const {
    w.U32(o.netId);
    w.Str(o.key);
    w.U32(o.owner);
}

void Replication::WriteSpawn (Writer& w, const Object& o) const {
    w.U32(o.netId);
    w.Str(o.templatePath);
    w.U32(o.owner);
    const wi::scene::TransformComponent* t = wi::scene::GetScene().transforms.GetComponent(o.entity);
    (t ? NetTransform::From(*t) : NetTransform()).Write(w);
    w.U8(uint8_t((o.options.syncTransform ? 1 : 0) | (o.options.ownerAuthority ? 2 : 0)));
    w.Str(o.options.localOnly);
}

// ---------------------------------------------------------------------- objects ---

void Replication::Register (Entity e, const Options& options) {
    if (e == INVALID_ENTITY) return;
    if (Object* o = Find(e)) {   // spawned through the network: its stNetObject adds to the spawn's options
        o->options.syncTransform = options.syncTransform;
        o->options.ownerAuthority = o->options.ownerAuthority || options.ownerAuthority;
        if (!options.localOnly.empty()) o->options.localOnly = options.localOnly;
        o->localOnlyState = -1;
        o->registered = true;
        UpdateProxy(*o);
        return;
    }
    Object o;
    o.entity = e;
    o.options = options;
    o.registered = true;
    o.key = KeyOf(wi::scene::GetScene(), e);
    if (IsServer()) {
        o.netId = nextNetId_++;
        Writer w(MsgNetBind);
        WriteBind(w, o);
        Broadcast(w, Delivery::Reliable);
    } else if (IsClient()) {
        auto it = pendingByKey_.find(o.key);
        if (it != pendingByKey_.end()) {
            o.netId = it->second.first;
            o.owner = it->second.second;
            o.bound = true;
            pendingByKey_.erase(it);
        }
    }
    objects_.push_back(o);
    if (objects_.back().bound) Bound(objects_.back());
    UpdateProxy(objects_.back());
}

void Replication::Bound (Object& o) {
    auto it = pendingNet_.find(o.netId);
    if (it == pendingNet_.end()) return;
    Pending p = std::move(it->second);
    pendingNet_.erase(it);
    for (uint8_t c = 0; c < 4; ++c)
        if (!p.state[c].empty()) ApplyState(o, c, p.state[c].data(), p.state[c].size());
    for (const auto& kv : p.vars) {
        o.vars[kv.first] = kv.second;
        FireVar(o.entity, kv.first, kv.second);
    }
}

void Replication::Unregister (Entity e) {
    auto it = std::find_if(objects_.begin(), objects_.end(), [e](const Object& o) { return o.entity == e; });
    if (it == objects_.end()) return;
    if (IsServer() && it->netId != 0) {
        Writer w(MsgNetDespawn);
        w.U32(it->netId);
        Broadcast(w, Delivery::Reliable);
    }
    ReleaseProxy(*it);
    objects_.erase(it);
}

Entity Replication::Instantiate (const std::string& path, const NetTransform& t) {
    std::unique_ptr<ObjectTemplate>& tpl = templates_[path];
    if (!tpl) {
        tpl = std::make_unique<ObjectTemplate>();
        std::string error;
        if (!tpl->Load(path, &error)) {
            Log("template " + path + " did not load: " + error, wi::backlog::LogLevel::Error);
            tpl.reset();
            templates_.erase(path);
            return INVALID_ENTITY;
        }
    }
    wi::scene::Scene& scene = wi::scene::GetScene();
    const Entity root = tpl->Instantiate(scene, XMFLOAT3((float)t.px, (float)t.py, (float)t.pz), t.rotation);
    if (wi::scene::TransformComponent* tr = scene.transforms.GetComponent(root)) {
        t.ApplyTo(*tr);
        tr->UpdateTransform();
    }
    return root;
}

Entity Replication::Spawn (const std::string& templatePath, const XMFLOAT3& position, const XMFLOAT4& rotation, uint32_t owner,
                           const Options* options) {
    if (session_ != nullptr && session_->IsOnline() && !IsServer()) {
        Log("Spawn: only the server spawns network objects", wi::backlog::LogLevel::Warning);
        return INVALID_ENTITY;
    }
    NetTransform t;
    t.px = position.x; t.py = position.y; t.pz = position.z;
    t.rotation = rotation;
    const Entity root = Instantiate(templatePath, t);
    if (root == INVALID_ENTITY) return INVALID_ENTITY;
    Object o;
    o.netId = nextNetId_++;
    o.entity = root;
    o.owner = owner;
    o.templatePath = templatePath;
    o.registered = true;
    if (options) o.options = *options;
    objects_.push_back(o);
    if (IsServer()) {
        Writer w(MsgNetSpawn);
        WriteSpawn(w, objects_.back());
        Broadcast(w, Delivery::Reliable);
    }
    UpdateProxy(objects_.back());
    return root;
}

bool Replication::Despawn (Entity e) {
    auto it = std::find_if(objects_.begin(), objects_.end(), [e](const Object& o) { return o.entity == e; });
    if (it == objects_.end() || (session_ && session_->IsOnline() && !IsServer())) return false;
    if (IsServer() && it->netId != 0) {
        Writer w(MsgNetDespawn);
        w.U32(it->netId);
        Broadcast(w, Delivery::Reliable);
    }
    ReleaseProxy(*it);
    objects_.erase(it);   // first: removing the entity runs stNetObject's Destroy -> Unregister
    wi::scene::GetScene().Entity_Remove(e);
    return true;
}

bool Replication::SetOwner (Entity e, uint32_t owner) {
    Object* o = Find(e);
    if (o == nullptr || (session_ && session_->IsOnline() && !IsServer())) return false;
    o->owner = owner;
    o->localOnlyState = -1;
    if (IsServer() && o->netId != 0) {
        Writer w(MsgNetOwner);
        w.U32(o->netId);
        w.U32(owner);
        Broadcast(w, Delivery::Reliable);
    }
    o->samples.clear();
    UpdateProxy(*o);
    return true;
}

bool Replication::SetVar (Entity e, const std::string& name, const NetValue& value) {
    Object* o = Find(e);
    if (o == nullptr || name.empty() || (session_ && session_->IsOnline() && !IsServer())) return false;
    auto it = o->vars.find(name);
    if (it != o->vars.end() && it->second == value) return true;
    o->vars[name] = value;
    if (IsServer() && o->netId != 0) {
        Writer w(MsgNetVar);
        w.U32(o->netId);
        w.Str(name);
        value.Write(w);
        Broadcast(w, Delivery::Reliable);
    }
    FireVar(e, name, value);
    return true;
}

const NetValue* Replication::GetVar (Entity e, const std::string& name) const {
    const Object* o = Find(e);
    if (o == nullptr) return nullptr;
    auto it = o->vars.find(name);
    return it != o->vars.end() ? &it->second : nullptr;
}

int Replication::AddVarListener (VarListener fn) {
    varListeners_.emplace_back(nextListener_, std::move(fn));
    return nextListener_++;
}

void Replication::RemoveVarListener (int handle) {
    varListeners_.erase(std::remove_if(varListeners_.begin(), varListeners_.end(),
                                       [handle](const auto& l) { return l.first == handle; }), varListeners_.end());
}

void Replication::FireVar (Entity e, const std::string& name, const NetValue& v) {
    // A copy: a listener may add or remove listeners.
    const auto listeners = varListeners_;
    for (const auto& l : listeners) l.second(e, name, v);
}

uint32_t Replication::NetId (Entity e) const {
    const Object* o = Find(e);
    return o ? o->netId : 0;
}

Entity Replication::EntityOf (uint32_t netId) const {
    for (const Object& o : objects_) if (netId != 0 && o.netId == netId) return o.entity;
    return INVALID_ENTITY;
}

uint32_t Replication::OwnerOf (Entity e) const {
    const Object* o = Find(e);
    return o ? o->owner : 0;
}

bool Replication::HasAuthority (Entity e) const {
    const Object* o = Find(e);
    return o == nullptr || Drives(*o);
}

std::vector<Replication::Info> Replication::List () const {
    std::vector<Info> out;
    for (const Object& o : objects_) out.push_back({ o.netId, o.entity, o.owner, o.key, o.templatePath, o.bound });
    return out;
}

// --------------------------------------------------------------------- session ---

void Replication::AssignServerIds () {
    for (Object& o : objects_) {
        if (o.netId == 0) o.netId = nextNetId_++;
        o.sentOnce = false;
    }
}

void Replication::OnSessionStarted (NetSession& session) {
    session_ = &session;
    epoch_ = Now();
    haveOffset_ = false;
    pendingByKey_.clear();
    pendingNet_.clear();
    if (IsServer()) {
        AssignServerIds();
    } else {
        for (Object& o : objects_) {
            o.netId = 0;
            o.bound = false;
            o.samples.clear();
        }
    }
    for (Object& o : objects_) UpdateProxy(o);
}

void Replication::OnSessionEnded () {
    if (session_ == nullptr) return;
    const bool wasClient = IsClient();
    wi::scene::Scene& scene = wi::scene::GetScene();
    std::vector<Entity> remove;
    for (Object& o : objects_) {
        ReleaseProxy(o);
        o.samples.clear();
        o.bound = false;
        if (wasClient) {
            o.netId = 0;
            if (!o.templatePath.empty()) remove.push_back(o.entity);   // the server's spawns go with it
        }
    }
    session_ = nullptr;
    for (Entity e : remove) {
        objects_.erase(std::remove_if(objects_.begin(), objects_.end(), [e](const Object& o) { return o.entity == e; }),
                       objects_.end());
        scene.Entity_Remove(e);
    }
    pendingByKey_.clear();
    pendingNet_.clear();
}

void Replication::OnPlayerJoined (uint32_t, ConnectionId connection) {
    // The whole world, reliably and in order: names for what the map already has, spawns for
    //	the rest. Transforms follow in the next keyframe.
    for (const Object& o : objects_) {
        if (o.netId == 0) continue;
        if (o.templatePath.empty()) {
            Writer w(MsgNetBind);
            WriteBind(w, o);
            SendTo(connection, w, Delivery::Reliable);
        } else {
            Writer w(MsgNetSpawn);
            WriteSpawn(w, o);
            SendTo(connection, w, Delivery::Reliable);
        }
    }
    keyframeTimer_ = kKeyframeInterval;   // next send is a full one
    // Component state and NetVars as they are now.
    for (const Object& o : objects_) {
        if (o.netId == 0) continue;
        for (uint8_t c = 0; c < 4; ++c) {
            if (o.lastState[c].empty()) continue;
            Writer w(MsgNetState);
            w.U32(o.netId);
            w.U8(c);
            w.Blob(o.lastState[c].data(), o.lastState[c].size());
            SendTo(connection, w, Delivery::Reliable);
        }
        for (const auto& kv : o.vars) {
            Writer w(MsgNetVar);
            w.U32(o.netId);
            w.Str(kv.first);
            kv.second.Write(w);
            SendTo(connection, w, Delivery::Reliable);
        }
    }
}

void Replication::OnPlayerLeft (uint32_t clientId) {
    for (Object& o : objects_) {
        if (o.owner != clientId) continue;
        o.owner = 0;   // back to the server; a game that spawned a player object despawns it itself
        if (o.netId != 0) {
            Writer w(MsgNetOwner);
            w.U32(o.netId);
            w.U32(0);
            Broadcast(w, Delivery::Reliable);
        }
        UpdateProxy(o);
    }
}

// --------------------------------------------------------------------- messages ---

void Replication::OnMessage (uint32_t from, uint8_t type, Reader& r) {
    wi::scene::Scene& scene = wi::scene::GetScene();
    if (IsClient()) {
        switch (type) {
        case MsgNetBind: {
            const uint32_t netId = r.U32();
            const std::string key = r.Str();
            const uint32_t owner = r.U32();
            if (!r.ok()) return;
            for (Object& o : objects_) {
                if (o.templatePath.empty() && o.key == key) {
                    o.netId = netId;
                    o.owner = owner;
                    o.bound = true;
                    Bound(o);
                    UpdateProxy(o);
                    return;
                }
            }
            pendingByKey_[key] = { netId, owner };   // its stNetObject has not started here yet
            return;
        }
        case MsgNetSpawn: {
            const uint32_t netId = r.U32();
            const std::string path = r.Str();
            const uint32_t owner = r.U32();
            NetTransform t;
            if (!t.Read(r) || FindNet(netId) != nullptr) return;
            Options options;
            const uint8_t flags = r.U8();
            options.syncTransform = flags & 1;
            options.ownerAuthority = (flags & 2) != 0;
            options.localOnly = r.Str();
            if (!r.ok()) return;
            const Entity root = Instantiate(path, t);
            if (root == INVALID_ENTITY) return;
            Object* existing = Find(root);   // the template's own stNetObject may have registered already
            if (existing == nullptr) {
                objects_.emplace_back();
                existing = &objects_.back();
                existing->entity = root;
                existing->registered = true;
            }
            existing->netId = netId;
            existing->owner = owner;
            existing->templatePath = path;
            existing->options.syncTransform = options.syncTransform;
            existing->options.ownerAuthority = existing->options.ownerAuthority || options.ownerAuthority;
            if (!options.localOnly.empty()) existing->options.localOnly = options.localOnly;
            existing->localOnlyState = -1;
            existing->bound = true;   // placed where the server had it; transforms follow
            Bound(*existing);
            UpdateProxy(*existing);
            return;
        }
        case MsgNetDespawn: {
            const uint32_t netId = r.U32();
            if (!r.ok()) return;
            pendingNet_.erase(netId);
            auto it = std::find_if(objects_.begin(), objects_.end(), [netId](const Object& o) { return o.netId == netId; });
            if (it == objects_.end()) return;
            const Entity e = it->entity;
            ReleaseProxy(*it);
            objects_.erase(it);
            scene.Entity_Remove(e);
            return;
        }
        case MsgNetTransforms: {
            const double serverTime = r.F64();
            const uint16_t n = r.U16();
            if (!r.ok()) return;
            const double offset = serverTime - Now();
            // The smallest offset seen is the least delayed packet; ease toward newer ones so a
            //	clock that drifts is followed.
            if (!haveOffset_) { serverOffset_ = offset; haveOffset_ = true; }
            else serverOffset_ = offset < serverOffset_ ? offset : serverOffset_ + (offset - serverOffset_) * 0.02;
            for (uint16_t i = 0; i < n; ++i) {
                const uint32_t netId = r.U32();
                NetTransform t;
                if (!t.Read(r)) return;
                Object* o = FindNet(netId);
                if (o == nullptr || Drives(*o)) continue;
                if (!o->samples.empty() && o->samples.back().time >= serverTime) continue;   // out of order
                o->samples.push_back({ serverTime, t });
                while (o->samples.size() > kMaxSamples) o->samples.pop_front();
            }
            return;
        }
        case MsgNetOwner: {
            const uint32_t netId = r.U32();
            const uint32_t owner = r.U32();
            if (!r.ok()) return;
            if (Object* o = FindNet(netId)) {
                o->owner = owner;
                o->localOnlyState = -1;
                o->samples.clear();
                UpdateProxy(*o);
            }
            return;
        }
        case MsgNetState: {
            const uint32_t netId = r.U32();
            const uint8_t component = r.U8();
            size_t n = 0;
            const uint8_t* bytes = r.Blob(n);
            if (!r.ok() || component >= 4) return;
            if (Object* o = FindNet(netId)) ApplyState(*o, component, bytes, n);
            else pendingNet_[netId].state[component].assign(bytes, bytes + n);
            return;
        }
        case MsgNetVar: {
            const uint32_t netId = r.U32();
            const std::string name = r.Str();
            NetValue v;
            if (!v.Read(r)) return;
            if (Object* o = FindNet(netId)) {
                o->vars[name] = v;
                FireVar(o->entity, name, v);
            } else {
                pendingNet_[netId].vars[name] = v;
            }
            return;
        }
        default:
            return;
        }
    }

    if (IsServer() && type == MsgNetOwnerTransforms) {
        const uint16_t n = r.U16();
        for (uint16_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t netId = r.U32();
            NetTransform t;
            if (!t.Read(r)) return;
            Object* o = FindNet(netId);
            // Only the owner, and only for an object it has authority over.
            if (o == nullptr || o->owner != from || !o->options.ownerAuthority) continue;
            if (wi::scene::TransformComponent* tr = scene.transforms.GetComponent(o->entity)) t.ApplyTo(*tr);
        }
    }
}

// ----------------------------------------------------------------------- frame ---

void Replication::Update (float dt) {
    if (session_ == nullptr || !session_->IsOnline()) return;
    for (Object& o : objects_) UpdateProxy(o);   // a rigid body added later is caught here

    sendTimer_ += dt;
    keyframeTimer_ += dt;
    const bool send = sendTimer_ >= kSendInterval;
    if (send) sendTimer_ = 0;

    if (IsServer()) {
        stateTimer_ += dt;
        if (stateTimer_ >= kStateInterval) {
            stateTimer_ = 0;
            ServerSendState(false);
        }
        if (send) {
            const bool keyframe = keyframeTimer_ >= kKeyframeInterval;
            if (keyframe) keyframeTimer_ = 0;
            ServerSendTransforms(keyframe);
        }
    } else if (IsClient() && session_->IsReady()) {
        if (send) ClientSendOwned();
        ClientPlayback();
    }
}

void Replication::ServerSendTransforms (bool keyframe) {
    if (session_->GetTransport() == nullptr) return;
    wi::scene::Scene& scene = wi::scene::GetScene();
    struct Item { uint32_t netId; uint32_t owner; bool ownerAuthority; NetTransform t; };
    std::vector<Item> items;
    for (Object& o : objects_) {
        if (o.netId == 0 || !o.options.syncTransform) continue;
        const wi::scene::TransformComponent* tr = scene.transforms.GetComponent(o.entity);
        if (tr == nullptr) continue;
        const NetTransform t = NetTransform::From(*tr);
        if (!keyframe && o.sentOnce && t.Near(o.lastSent)) continue;
        o.lastSent = t;
        o.sentOnce = true;
        items.push_back({ o.netId, o.owner, o.options.ownerAuthority, t });
    }
    if (items.empty()) return;
    const double now = Now() - epoch_;
    for (const Peer& p : session_->Peers()) {
        if (p.clientId == 0) continue;
        // Not back to the client that drives it.
        std::vector<const Item*> mine;
        for (const Item& it : items)
            if (!(it.ownerAuthority && it.owner == p.clientId)) mine.push_back(&it);
        for (size_t first = 0; first < mine.size(); first += kMaxPerMessage) {
            const size_t n = std::min(kMaxPerMessage, mine.size() - first);
            Writer w(MsgNetTransforms);
            w.F64(now);
            w.U16(uint16_t(n));
            for (size_t i = 0; i < n; ++i) {
                w.U32(mine[first + i]->netId);
                mine[first + i]->t.Write(w);
            }
            SendTo(p.connection, w, Delivery::Unreliable);
        }
    }
}

void Replication::ServerSendState (bool force) {
    wi::scene::Scene& scene = wi::scene::GetScene();
    for (Object& o : objects_) {
        if (o.netId == 0) continue;
        for (uint8_t c = 0; c < 4; ++c) {
            auto it = scene.componentLibrary.entries.find(kStateComponents[c]);
            if (it == scene.componentLibrary.entries.end() || !it->second.component_manager) continue;
            wi::Archive archive;
            wi::ecs::EntitySerializer seri;
            seri.allow_remap = false;
            seri.version = it->second.version;
            seri.library_versions[it->first] = it->second.version;
            it->second.component_manager->Component_Serialize(o.entity, archive, seri);
            std::vector<uint8_t> bytes(archive.GetData(), archive.GetData() + archive.GetPos());
            if (!force && bytes == o.lastState[c]) continue;
            const bool first = o.lastState[c].empty();
            o.lastState[c] = std::move(bytes);
            // A client that loaded the same map already has the placed object's components
            //	as they were saved: the first archive of one only matters once it differs.
            if (first && o.templatePath.empty() && !force) continue;
            Writer w(MsgNetState);
            w.U32(o.netId);
            w.U8(c);
            w.Blob(o.lastState[c].data(), o.lastState[c].size());
            Broadcast(w, Delivery::Reliable);
        }
    }
}

void Replication::ApplyState (Object& o, uint8_t component, const uint8_t* data, size_t size) {
    wi::scene::Scene& scene = wi::scene::GetScene();
    auto it = scene.componentLibrary.entries.find(kStateComponents[component]);
    if (it == scene.componentLibrary.entries.end() || !it->second.component_manager || data == nullptr) return;
    wi::ecs::ComponentManager_Interface& manager = *it->second.component_manager;
    // Enable flags (NCE_) are each machine's own - local-only components differ per machine.
    std::vector<std::pair<std::string, bool>> enables;
    if (component == 2)
        if (const wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(o.entity))
            for (size_t i = 0; i < m->bool_values.names.size(); ++i)
                if (m->bool_values.names[i].rfind("NCE_", 0) == 0) enables.push_back({ m->bool_values.names[i], m->bool_values.get(i) });
    // Component_Serialize creates the component when reading: the old one goes first.
    if (manager.Contains(o.entity)) manager.Remove(o.entity);
    wi::Archive archive(data, size);
    wi::ecs::EntitySerializer seri;
    seri.allow_remap = false;
    seri.version = it->second.version;
    seri.library_versions[it->first] = it->second.version;
    manager.Component_Serialize(o.entity, archive, seri);
    if (component == 2) {
        if (wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(o.entity))
            for (const auto& kv : enables) m->bool_values.set(kv.first, kv.second);
        o.localOnlyState = -1;
    }
}

void Replication::ClientSendOwned () {
    const std::vector<Peer>& peers = session_->Peers();
    if (peers.empty()) return;
    wi::scene::Scene& scene = wi::scene::GetScene();
    Writer w(MsgNetOwnerTransforms);
    std::vector<std::pair<uint32_t, NetTransform>> items;
    for (Object& o : objects_) {
        if (o.netId == 0 || !o.options.syncTransform || !o.options.ownerAuthority || o.owner != LocalPlayer()) continue;
        if (const wi::scene::TransformComponent* tr = scene.transforms.GetComponent(o.entity))
            items.push_back({ o.netId, NetTransform::From(*tr) });
    }
    if (items.empty()) return;
    w.U16(uint16_t(std::min(items.size(), kMaxPerMessage)));
    for (size_t i = 0; i < items.size() && i < kMaxPerMessage; ++i) {
        w.U32(items[i].first);
        items[i].second.Write(w);
    }
    SendTo(peers[0].connection, w, Delivery::Unreliable);
}

void Replication::ClientPlayback () {
    if (!haveOffset_) return;
    wi::scene::Scene& scene = wi::scene::GetScene();
    const double at = Now() + serverOffset_ - kInterpDelay;
    for (Object& o : objects_) {
        if (o.samples.empty() || Drives(o) || !o.options.syncTransform) continue;
        wi::scene::TransformComponent* tr = scene.transforms.GetComponent(o.entity);
        if (tr == nullptr) continue;
        // Drop what is behind the pair around `at`.
        while (o.samples.size() > 2 && o.samples[1].time <= at) o.samples.pop_front();
        NetTransform t = o.samples.front().transform;
        if (o.samples.size() >= 2 && at > o.samples[0].time) {
            const Sample& a = o.samples[0];
            const Sample& b = o.samples[1];
            const double span = b.time - a.time;
            const float k = span > 0 ? (float)std::clamp((at - a.time) / span, 0.0, 1.0) : 1.0f;
            t = Lerp(a.transform, b.transform, k);
        }
        t.ApplyTo(*tr);
    }
}

} // namespace st::net
