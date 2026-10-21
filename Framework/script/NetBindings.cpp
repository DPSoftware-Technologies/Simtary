#include "script/NetBindings.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include "net/NetSession.h"
#include "net/Replication.h"

#include "StTaipan.h"
#include "wiScene.h"
#include "wiBacklog.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tb = st::taipan;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st::script {

namespace {

constexpr uint8_t kMsgEventToServer = 30;   // C->S reliable: str name, u8 n, n x value
constexpr uint8_t kMsgEventToClient = 31;   // S->C reliable: str name, u8 n, n x value
constexpr int kMaxArgs = 16;

// A GameObject handle is its entity id with a kind tag in the top byte (ScriptBindings.cpp).
constexpr uint64_t kIdMask = (1ull << 56) - 1;
Entity EntityOfHandle (void* h) { return Entity(reinterpret_cast<uintptr_t>(h) & kIdMask); }

struct Sub {
    std::string name;
    Entity target;
    std::string handler;
    bool server;   // OnServer (else OnClient)
};
struct VarSub {
    Entity object;
    std::string name;
    Entity target;
    std::string handler;
};

std::vector<Sub> g_subs;
std::vector<VarSub> g_varSubs;
std::unordered_set<std::string> g_warned;
bool g_hooked = false;

net::NetSession& Session () { return net::NetSession::Get(); }

// ------------------------------------------------------------------ arguments ---

enum class Tag : uint8_t { Nil, Bool, Int, Float, String, Vector2, Vector3, Quaternion, Color, Object };

// One script argument, owned (strings outlive the call that built the value).
struct Arg {
    Tag tag = Tag::Nil;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    float v[4] = { 0, 0, 0, 0 };
    uint32_t netId = 0;
};

bool ReadArg (tb::Call* call, int index, Arg& a) {
    switch (tb::ArgType(call, index)) {
    case tb::Type::Null: a.tag = Tag::Nil; return true;
    case tb::Type::Bool: a.tag = Tag::Bool; a.b = tb::ArgBool(call, index); return true;
    case tb::Type::Int: a.tag = Tag::Int; a.i = tb::ArgInt(call, index); return true;
    case tb::Type::Float: a.tag = Tag::Float; a.f = tb::ArgFloat(call, index); return true;
    case tb::Type::String: a.tag = Tag::String; a.s = tb::ArgString(call, index); return true;
    case tb::Type::Vector2: { const tb::Vec2 v = tb::ArgVector2(call, index); a.tag = Tag::Vector2; a.v[0] = v.x; a.v[1] = v.y; return true; }
    case tb::Type::Vector3: { const tb::Vec3 v = tb::ArgVector3(call, index); a.tag = Tag::Vector3; a.v[0] = v.x; a.v[1] = v.y; a.v[2] = v.z; return true; }
    case tb::Type::Quaternion: { const tb::Quat q = tb::ArgQuaternion(call, index); a.tag = Tag::Quaternion; a.v[0] = q.x; a.v[1] = q.y; a.v[2] = q.z; a.v[3] = q.w; return true; }
    case tb::Type::Color: { const tb::Rgba c = tb::ArgColor(call, index); a.tag = Tag::Color; a.v[0] = c.r; a.v[1] = c.g; a.v[2] = c.b; a.v[3] = c.a; return true; }
    case tb::Type::Object: {
        void* h = tb::ArgObject(call, index, "GameObject");
        a.tag = Tag::Object;
        a.netId = h ? net::Replication::Get().NetId(EntityOfHandle(h)) : 0;
        if (h && a.netId == 0) {
            tb::Fail(call, "Net: a GameObject argument must be a network object (stNetObject or Net.Spawn)");
            return false;
        }
        return true;
    }
    default:
        tb::Fail(call, "Net: arguments can be int, float, bool, str, Vector2/3, Quaternion, Color or GameObject");
        return false;
    }
}

void WriteArg (net::Writer& w, const Arg& a) {
    w.U8(uint8_t(a.tag));
    switch (a.tag) {
    case Tag::Nil: break;
    case Tag::Bool: w.U8(a.b ? 1 : 0); break;
    case Tag::Int: w.U32(uint32_t(uint64_t(a.i) >> 32)); w.U32(uint32_t(a.i)); break;
    case Tag::Float: w.F64(a.f); break;
    case Tag::String: w.Str(a.s); break;
    case Tag::Vector2: w.F32(a.v[0]); w.F32(a.v[1]); break;
    case Tag::Vector3: w.F32(a.v[0]); w.F32(a.v[1]); w.F32(a.v[2]); break;
    case Tag::Quaternion:
    case Tag::Color: for (int k = 0; k < 4; ++k) w.F32(a.v[k]); break;
    case Tag::Object: w.U32(a.netId); break;
    }
}

bool ReadArgWire (net::Reader& r, Arg& a) {
    a.tag = Tag(r.U8());
    switch (a.tag) {
    case Tag::Nil: break;
    case Tag::Bool: a.b = r.U8() != 0; break;
    case Tag::Int: { const uint64_t hi = r.U32(), lo = r.U32(); a.i = int64_t((hi << 32) | lo); break; }
    case Tag::Float: a.f = r.F64(); break;
    case Tag::String: a.s = r.Str(); break;
    case Tag::Vector2: a.v[0] = r.F32(); a.v[1] = r.F32(); break;
    case Tag::Vector3: a.v[0] = r.F32(); a.v[1] = r.F32(); a.v[2] = r.F32(); break;
    case Tag::Quaternion:
    case Tag::Color: for (int k = 0; k < 4; ++k) a.v[k] = r.F32(); break;
    case Tag::Object: a.netId = r.U32(); break;
    default: return false;
    }
    return r.ok();
}

tb::Value ToValue (const Arg& a) {
    switch (a.tag) {
    case Tag::Bool: return tb::Value::Bool(a.b);
    case Tag::Int: return tb::Value::Int(a.i);
    case Tag::Float: return tb::Value::Float(a.f);
    case Tag::String: return tb::Value::String(a.s.c_str());
    case Tag::Vector2: return tb::Value::Vector2(tb::Vec2{ a.v[0], a.v[1] });
    case Tag::Vector3: return tb::Value::Vector3(tb::Vec3{ a.v[0], a.v[1], a.v[2] });
    case Tag::Quaternion: return tb::Value::Quaternion(tb::Quat{ a.v[0], a.v[1], a.v[2], a.v[3] });
    case Tag::Color: return tb::Value::Color(tb::Rgba{ a.v[0], a.v[1], a.v[2], a.v[3] });
    case Tag::Object: {
        const Entity e = net::Replication::Get().EntityOf(a.netId);
        return e == INVALID_ENTITY ? tb::Value::Null() : EntityValue(e);
    }
    default: return tb::Value::Null();
    }
}

// --------------------------------------------------------------------- deliver ---

// Calls `handler(prefix..., args...)` on every stScript of `target`.
bool CallOn (Entity target, const std::string& handler, const std::vector<tb::Value>& values) {
    wi::scene::Scene& scene = wi::scene::GetScene();
    wi::vector<wi::scene::NativeComponent*> scripts;
    scene.nativeComponents.GetAll(target, wi::scene::GetNativeTypeID<st::ScriptComponent>(), scripts);
    bool handled = false;
    for (wi::scene::NativeComponent* c : scripts)
        handled |= static_cast<st::ScriptComponent*>(c)->CallEvent(handler.c_str(), values.data(), (int)values.size());
    return handled;
}

void Deliver (const std::string& name, bool toServer, uint32_t player, const std::vector<Arg>& args) {
    std::vector<tb::Value> values;
    if (toServer) values.push_back(tb::Value::Int(player));
    for (const Arg& a : args) values.push_back(ToValue(a));
    const std::vector<Sub> subs = g_subs;   // a handler may subscribe or unsubscribe
    bool any = false;
    for (const Sub& s : subs) {
        if (s.server != toServer || s.name != name) continue;
        any = true;
        if (!CallOn(s.target, s.handler, values) && g_warned.insert(s.handler + "@" + std::to_string(s.target)).second)
            wi::backlog::post("[net] no running stScript on entity " + std::to_string(s.target) + " defines func " + s.handler,
                              wi::backlog::LogLevel::Warning);
    }
    if (!any && g_warned.insert(std::string(toServer ? "server:" : "client:") + name).second)
        wi::backlog::post("[net] event \"" + name + "\" arrived " + (toServer ? "at the server" : "at a client") +
                          " but nothing listens (Net." + (toServer ? "OnServer" : "OnClient") + ")",
                          wi::backlog::LogLevel::Warning);
}

net::Writer EventMessage (uint8_t type, const std::string& name, const std::vector<Arg>& args) {
    net::Writer w(type);
    w.Str(name);
    w.U8(uint8_t(args.size()));
    for (const Arg& a : args) WriteArg(w, a);
    return w;
}

void OnWire (uint32_t from, uint8_t type, net::Reader& r) {
    if (type != kMsgEventToServer && type != kMsgEventToClient) return;
    const std::string name = r.Str();
    const uint8_t n = r.U8();
    std::vector<Arg> args(n);
    for (Arg& a : args) if (!ReadArgWire(r, a)) return;
    if (!r.ok()) return;
    if (type == kMsgEventToServer && Session().IsServer()) Deliver(name, true, from, args);
    else if (type == kMsgEventToClient && Session().IsClient()) Deliver(name, false, 0, args);
}

void OnPlayer (uint32_t clientId, bool joined) {
    // Every running script on the server hears it (stScript instances that define the function).
    wi::scene::Scene& scene = wi::scene::GetScene();
    const tb::Value v = tb::Value::Int(clientId);
    for (size_t i = 0; i < scene.metadatas.GetCount(); ++i) {
        wi::vector<wi::scene::NativeComponent*> scripts;
        scene.nativeComponents.GetAll(scene.metadatas.GetEntity(i), wi::scene::GetNativeTypeID<st::ScriptComponent>(), scripts);
        for (wi::scene::NativeComponent* c : scripts)
            static_cast<st::ScriptComponent*>(c)->CallEvent(joined ? "OnPlayerJoined" : "OnPlayerLeft", &v, 1);
    }
}

void Hook () {
    if (g_hooked) return;
    g_hooked = true;
    Session().SetScriptSink(&OnWire);
    Session().AddPlayerListener(&OnPlayer);
    net::Replication::Get().AddVarListener([](Entity object, const std::string& name, const net::NetValue&) {
        const std::vector<VarSub> subs = g_varSubs;
        for (const VarSub& s : subs) {
            if (s.object != object || s.name != name) continue;
            std::vector<tb::Value> values = { EntityValue(object), tb::Value::String(name.c_str()) };
            CallOn(s.target, s.handler, values);
        }
    });
}

bool ReadArgs (tb::Call* call, int first, std::vector<Arg>& out) {
    const int n = tb::ArgCount(call) - first;
    if (n > kMaxArgs) { tb::Fail(call, "Net: at most 16 event arguments"); return false; }
    out.resize(std::max(0, n));
    for (int i = 0; i < n; ++i) if (!ReadArg(call, first + i, out[i])) return false;
    return true;
}

Entity ObjectArg (tb::Call* call, int index) {
    void* h = tb::ArgObject(call, index, "GameObject");
    return h ? EntityOfHandle(h) : INVALID_ENTITY;
}

// ---------------------------------------------------------------------- natives ---

void NetIsServer (tb::Call* call, void*) { tb::ReturnBool(call, Session().IsServer()); }
void NetIsClient (tb::Call* call, void*) { tb::ReturnBool(call, Session().IsClient()); }
void NetIsOnline (tb::Call* call, void*) { tb::ReturnBool(call, Session().IsOnline()); }
void NetIsHost (tb::Call* call, void*) { tb::ReturnBool(call, Session().IsHost()); }
void NetLocalPlayer (tb::Call* call, void*) { tb::ReturnInt(call, Session().LocalClientId()); }

void NetPlayerCount (tb::Call* call, void*) {
    int64_t n = Session().HasLocalPlayer() ? 1 : 0;
    if (Session().IsServer()) for (const net::Peer& p : Session().Peers()) n += p.clientId != 0 ? 1 : 0;
    tb::ReturnInt(call, n);
}

void NetPlayerId (tb::Call* call, void*) {
    int64_t i = tb::ArgInt(call, 0);
    if (Session().HasLocalPlayer()) {
        if (i == 0) { tb::ReturnInt(call, Session().LocalClientId()); return; }
        --i;
    }
    for (const net::Peer& p : Session().Peers()) {
        if (p.clientId == 0) continue;
        if (i-- == 0) { tb::ReturnInt(call, p.clientId); return; }
    }
    tb::ReturnInt(call, 0);
}

void NetPlayerName (tb::Call* call, void*) {
    const uint32_t id = uint32_t(tb::ArgInt(call, 0));
    if (id != 0 && id == Session().LocalClientId()) { tb::ReturnString(call, Session().PlayerName().c_str()); return; }
    for (const net::Peer& p : Session().Peers())
        if (p.clientId == id && id != 0) { tb::ReturnString(call, p.name.c_str()); return; }
    tb::ReturnString(call, "");
}

void Subscribe (tb::Call* call, bool server) {
    Hook();
    const std::string name = tb::ArgString(call, 0);
    const Entity target = ObjectArg(call, 1);
    const std::string handler = tb::ArgString(call, 2);
    if (tb::Failed(call)) return;
    if (name.empty() || target == INVALID_ENTITY || handler.empty()) { tb::ReturnBool(call, false); return; }
    for (const Sub& s : g_subs)
        if (s.name == name && s.target == target && s.handler == handler && s.server == server) { tb::ReturnBool(call, true); return; }
    g_subs.push_back({ name, target, handler, server });
    tb::ReturnBool(call, true);
}
void NetOnServer (tb::Call* call, void*) { Subscribe(call, true); }
void NetOnClient (tb::Call* call, void*) { Subscribe(call, false); }

void NetOff (tb::Call* call, void*) {
    const std::string name = tb::ArgString(call, 0);
    const Entity target = ObjectArg(call, 1);
    if (tb::Failed(call)) return;
    const size_t before = g_subs.size();
    g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(), [&](const Sub& s) { return s.name == name && s.target == target; }),
                 g_subs.end());
    tb::ReturnInt(call, int64_t(before - g_subs.size()));
}

void NetFireServer (tb::Call* call, void*) {
    Hook();
    const std::string name = tb::ArgString(call, 0);
    std::vector<Arg> args;
    if (tb::Failed(call) || !ReadArgs(call, 1, args)) return;
    net::NetSession& s = Session();
    if (s.IsClient()) {
        s.SendToServer(EventMessage(kMsgEventToServer, name, args), net::Delivery::Reliable);
    } else {
        // Offline or the server itself (a host's player): the server is right here.
        Deliver(name, true, s.LocalClientId(), args);
    }
}

void NetFireClient (tb::Call* call, void*) {
    Hook();
    const uint32_t player = uint32_t(tb::ArgInt(call, 0));
    const std::string name = tb::ArgString(call, 1);
    std::vector<Arg> args;
    if (tb::Failed(call) || !ReadArgs(call, 2, args)) return;
    net::NetSession& s = Session();
    if (s.IsClient()) { tb::Fail(call, "Net.FireClient: only the server sends to clients"); return; }
    if (!s.IsOnline() || (s.HasLocalPlayer() && player == s.LocalClientId())) {
        Deliver(name, false, 0, args);
        return;
    }
    tb::ReturnBool(call, s.SendToClient(player, EventMessage(kMsgEventToClient, name, args), net::Delivery::Reliable));
}

void NetFireAllClients (tb::Call* call, void*) {
    Hook();
    const std::string name = tb::ArgString(call, 0);
    std::vector<Arg> args;
    if (tb::Failed(call) || !ReadArgs(call, 1, args)) return;
    net::NetSession& s = Session();
    if (s.IsClient()) { tb::Fail(call, "Net.FireAllClients: only the server sends to clients"); return; }
    if (s.IsServer()) s.Broadcast(EventMessage(kMsgEventToClient, name, args), net::Delivery::Reliable);
    if (!s.IsOnline() || s.HasLocalPlayer()) Deliver(name, false, 0, args);
}

void NetSpawn (tb::Call* call, void*) {
    const std::string path = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    XMFLOAT3 pos(0, 0, 0);
    XMFLOAT4 rot(0, 0, 0, 1);
    if (tb::ArgCount(call) > 1 && tb::ArgType(call, 1) == tb::Type::Vector3) { const tb::Vec3 v = tb::ArgVector3(call, 1); pos = XMFLOAT3(v.x, v.y, v.z); }
    if (tb::ArgCount(call) > 2 && tb::ArgType(call, 2) == tb::Type::Quaternion) { const tb::Quat q = tb::ArgQuaternion(call, 2); rot = XMFLOAT4(q.x, q.y, q.z, q.w); }
    const uint32_t owner = tb::ArgCount(call) > 3 ? uint32_t(tb::ArgInt(call, 3)) : 0;
    if (tb::Failed(call)) return;
    const Entity e = net::Replication::Get().Spawn(path, pos, rot, owner);
    tb::Return(call, e == INVALID_ENTITY ? tb::Value::Null() : EntityValue(e));
}

void NetDespawn (tb::Call* call, void*) { tb::ReturnBool(call, net::Replication::Get().Despawn(ObjectArg(call, 0))); }
void NetOwner (tb::Call* call, void*) { tb::ReturnInt(call, net::Replication::Get().OwnerOf(ObjectArg(call, 0))); }
void NetHasAuthority (tb::Call* call, void*) { tb::ReturnBool(call, net::Replication::Get().HasAuthority(ObjectArg(call, 0))); }

void NetIsOwner (tb::Call* call, void*) {
    const Entity e = ObjectArg(call, 0);
    const uint32_t owner = net::Replication::Get().OwnerOf(e);
    tb::ReturnBool(call, net::Replication::Get().NetId(e) != 0 && owner != 0 && owner == Session().LocalClientId());
}

void NetSetOwner (tb::Call* call, void*) {
    tb::ReturnBool(call, net::Replication::Get().SetOwner(ObjectArg(call, 0), uint32_t(tb::ArgInt(call, 1))));
}

void NetSetVar (tb::Call* call, void*) {
    const Entity e = ObjectArg(call, 0);
    const std::string name = tb::ArgString(call, 1);
    Arg a;
    if (tb::Failed(call) || !ReadArg(call, 2, a)) return;
    net::NetValue v;
    switch (a.tag) {
    case Tag::Nil: v.type = net::NetValue::Type::Nil; break;
    case Tag::Bool: v.type = net::NetValue::Type::Bool; v.b = a.b; break;
    case Tag::Int: v.type = net::NetValue::Type::Int; v.i = a.i; break;
    case Tag::Float: v.type = net::NetValue::Type::Float; v.f = a.f; break;
    case Tag::String: v.type = net::NetValue::Type::String; v.s = a.s; break;
    case Tag::Vector2: v.type = net::NetValue::Type::Vector2; break;
    case Tag::Vector3: v.type = net::NetValue::Type::Vector3; break;
    case Tag::Quaternion: v.type = net::NetValue::Type::Quaternion; break;
    case Tag::Color: v.type = net::NetValue::Type::Color; break;
    case Tag::Object: tb::Fail(call, "Net.SetVar: a GameObject is not a NetVar value - store its name or an id"); return;
    }
    for (int k = 0; k < 4; ++k) v.v[k] = a.v[k];
    tb::ReturnBool(call, net::Replication::Get().SetVar(e, name, v));
}

void NetGetVar (tb::Call* call, void*) {
    const net::NetValue* v = net::Replication::Get().GetVar(ObjectArg(call, 0), tb::ArgString(call, 1));
    if (tb::Failed(call)) return;
    if (v == nullptr) { tb::Return(call, tb::Value::Null()); return; }
    switch (v->type) {
    case net::NetValue::Type::Bool: tb::ReturnBool(call, v->b); return;
    case net::NetValue::Type::Int: tb::ReturnInt(call, v->i); return;
    case net::NetValue::Type::Float: tb::ReturnFloat(call, v->f); return;
    case net::NetValue::Type::String: tb::ReturnString(call, v->s.c_str()); return;
    case net::NetValue::Type::Vector2: tb::ReturnVector2(call, tb::Vec2{ v->v[0], v->v[1] }); return;
    case net::NetValue::Type::Vector3: tb::ReturnVector3(call, tb::Vec3{ v->v[0], v->v[1], v->v[2] }); return;
    case net::NetValue::Type::Quaternion: tb::ReturnQuaternion(call, tb::Quat{ v->v[0], v->v[1], v->v[2], v->v[3] }); return;
    case net::NetValue::Type::Color: tb::ReturnColor(call, tb::Rgba{ v->v[0], v->v[1], v->v[2], v->v[3] }); return;
    default: tb::Return(call, tb::Value::Null()); return;
    }
}

void NetOnVar (tb::Call* call, void*) {
    Hook();
    const Entity object = ObjectArg(call, 0);
    const std::string name = tb::ArgString(call, 1);
    const Entity target = ObjectArg(call, 2);
    const std::string handler = tb::ArgString(call, 3);
    if (tb::Failed(call)) return;
    if (object == INVALID_ENTITY || target == INVALID_ENTITY || handler.empty()) { tb::ReturnBool(call, false); return; }
    for (const VarSub& s : g_varSubs)
        if (s.object == object && s.name == name && s.target == target && s.handler == handler) { tb::ReturnBool(call, true); return; }
    g_varSubs.push_back({ object, name, target, handler });
    tb::ReturnBool(call, true);
}

} // namespace

void ForgetNetSubscriptions (Entity target) {
    g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(), [target](const Sub& s) { return s.target == target; }), g_subs.end());
    g_varSubs.erase(std::remove_if(g_varSubs.begin(), g_varSubs.end(), [target](const VarSub& s) { return s.target == target; }),
                    g_varSubs.end());
}

void RegisterNetBindings (tb::VM* vm) {
    Hook();
    tb::RegisterFunction(vm, "Net.IsServer", &NetIsServer, nullptr, "() -> bool");
    tb::RegisterFunction(vm, "Net.IsClient", &NetIsClient, nullptr, "() -> bool");
    tb::RegisterFunction(vm, "Net.IsOnline", &NetIsOnline, nullptr, "() -> bool");
    tb::RegisterFunction(vm, "Net.IsHost", &NetIsHost, nullptr, "() -> bool");
    tb::RegisterFunction(vm, "Net.LocalPlayer", &NetLocalPlayer, nullptr, "() -> int");
    tb::RegisterFunction(vm, "Net.PlayerCount", &NetPlayerCount, nullptr, "() -> int");
    tb::RegisterFunction(vm, "Net.PlayerId", &NetPlayerId, nullptr, "(int) -> int");
    tb::RegisterFunction(vm, "Net.PlayerName", &NetPlayerName, nullptr, "(int) -> str");
    tb::RegisterFunction(vm, "Net.OnServer", &NetOnServer, nullptr, "(str, GameObject?, str) -> bool");
    tb::RegisterFunction(vm, "Net.OnClient", &NetOnClient, nullptr, "(str, GameObject?, str) -> bool");
    tb::RegisterFunction(vm, "Net.Off", &NetOff, nullptr, "(str, GameObject?) -> int");
    tb::RegisterFunction(vm, "Net.FireServer", &NetFireServer, nullptr, "(str, any...)");
    tb::RegisterFunction(vm, "Net.FireClient", &NetFireClient, nullptr, "(int, str, any...)");
    tb::RegisterFunction(vm, "Net.FireAllClients", &NetFireAllClients, nullptr, "(str, any...)");
    tb::RegisterFunction(vm, "Net.Spawn", &NetSpawn, nullptr, "(str, Vector3=, Quaternion=, int=) -> GameObject?");
    tb::RegisterFunction(vm, "Net.Despawn", &NetDespawn, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "Net.Owner", &NetOwner, nullptr, "(GameObject?) -> int");
    tb::RegisterFunction(vm, "Net.IsOwner", &NetIsOwner, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "Net.HasAuthority", &NetHasAuthority, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "Net.SetOwner", &NetSetOwner, nullptr, "(GameObject?, int) -> bool");
    tb::RegisterFunction(vm, "Net.SetVar", &NetSetVar, nullptr, "(GameObject?, str, any) -> bool");
    tb::RegisterFunction(vm, "Net.GetVar", &NetGetVar, nullptr, "(GameObject?, str) -> any");
    tb::RegisterFunction(vm, "Net.OnVar", &NetOnVar, nullptr, "(GameObject?, str, GameObject?, str) -> bool");
}

} // namespace st::script
