#pragma once
// Replication - any entity in sync between the server and every client (MULTIPLAYER.md, v2).
//
// An entity takes part by carrying the "stNetObject" native component (net/NetObjectComponent),
// or by being spawned through Spawn() on the server. The server's scene is the truth:
//
//   identity   objects placed in the map are matched by their EntityGUID (metadata, saved
//              with the map) - or, lacking one, by their name path - since every machine loads
//              the same map; objects spawned at runtime travel as a template path (.sttd) and
//              every client instantiates the same template. Net ids are the server's.
//   transform  the server sends what changed at 20 Hz (unreliable, a full keyframe every
//              second); clients play it back 100 ms behind the server's clock, interpolated.
//              Local transforms are sent - both ends have the same hierarchy.
//   ownership  every object has an owner: the server (0) or a client id. With
//              `ownerAuthority`, the owning client moves it itself and sends its transform up;
//              the server applies it and passes it on. Everything a machine does not drive is
//              a kinematic proxy there (its rigid body stops simulating).
//
// state      what else changes on a server object reaches clients, per component, for the
//              components that hold no entity references (entity ids differ between machines):
//              layer (visibility), light, metadata (native component parameters), name. Each is
//              archived at 10 Hz and re-sent reliably when its bytes changed.
// NetVars      small named values on a net object (SetVar on the server, GetVar anywhere),
//              with a change callback - game state that is not a component.
//
// A joining client gets the whole state (binds, spawns, transforms). Offline, nothing happens:
// objects register and wait, so a session started at runtime picks them up.
//
// Main thread only. No exceptions, no RTTI.

#include "net/NetMessage.h"
#include "net/Transport.h"

#include "wiECS.h"
#include "wiMath.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wi::scene { struct Scene; struct TransformComponent; }

namespace st { class ObjectTemplate; }

namespace st::net {

class NetSession;

enum ReplicationMessage : uint8_t {
    MsgNetBind = 20,             // S->C reliable: u32 net id, str key, u32 owner
    MsgNetSpawn = 21,            // S->C reliable: u32 net id, str template, u32 owner, transform
    MsgNetDespawn = 22,          // S->C reliable: u32 net id
    MsgNetTransforms = 23,       // S->C unreliable: f64 server time, u16 n, n x (u32 net id, transform)
    MsgNetOwnerTransforms = 24,  // C->S unreliable: u16 n, n x (u32 net id, transform)
    MsgNetOwner = 25,            // S->C reliable: u32 net id, u32 owner
    MsgNetState = 26,            // S->C reliable: u32 net id, u8 component, u32 n, n bytes (a wi::Archive)
    MsgNetVar = 27,              // S->C reliable: u32 net id, str name, value
};

// A NetVar's value: what a script can put on a net object and read everywhere.
struct NetValue {
    enum class Type : uint8_t { Nil, Bool, Int, Float, String, Vector2, Vector3, Quaternion, Color };
    Type type = Type::Nil;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    float v[4] = { 0, 0, 0, 0 };

    void Write (Writer& w) const;
    bool Read (Reader& r);
    bool operator== (const NetValue& o) const;
};

// A local transform as it travels. Large-world roots carry their absolute double position.
struct NetTransform {
    bool largeWorld = false;
    double px = 0, py = 0, pz = 0;
    XMFLOAT4 rotation = XMFLOAT4(0, 0, 0, 1);
    XMFLOAT3 scale = XMFLOAT3(1, 1, 1);

    static NetTransform From (const wi::scene::TransformComponent& t);
    void ApplyTo (wi::scene::TransformComponent& t) const;
    bool Near (const NetTransform& o) const;   // within a mm / a hundredth of a degree
    void Write (Writer& w) const;
    bool Read (Reader& r);
};

class Replication {
public:
    static Replication& Get ();

    struct Options {
        bool syncTransform = true;
        bool ownerAuthority = false;   // the owning client drives the transform
        // Native components enabled only on the machine that owns the object - a player's
        //	input, camera, HUD, character controller - comma separated. Everywhere else they
        //	are switched off (each machine keeps its own NCE_ enable flags).
        std::string localOnly;
    };

    // --- objects (any mode; from stNetObject's Start / Destroy)
    void Register (wi::ecs::Entity e, const Options& options);
    void Unregister (wi::ecs::Entity e);

    // --- server
    // Instantiate `templatePath` on every machine. Returns the root here; INVALID_ENTITY when
    //	not the server or the template does not load (logged).
    wi::ecs::Entity Spawn (const std::string& templatePath, const XMFLOAT3& position,
                           const XMFLOAT4& rotation, uint32_t owner, const Options* options = nullptr);
    // Remove a net object everywhere (the entity and its children).
    bool Despawn (wi::ecs::Entity e);
    bool SetOwner (wi::ecs::Entity e, uint32_t owner);
    // Server (or offline): set a NetVar; every client gets it. False for a non-net entity.
    bool SetVar (wi::ecs::Entity e, const std::string& name, const NetValue& value);
    const NetValue* GetVar (wi::ecs::Entity e, const std::string& name) const;
    // Called on every machine when a NetVar arrives / is set. Returns a handle for RemoveVarListener.
    using VarListener = std::function<void(wi::ecs::Entity, const std::string&, const NetValue&)>;
    int AddVarListener (VarListener fn);
    void RemoveVarListener (int handle);

    // --- queries (any mode)
    uint32_t NetId (wi::ecs::Entity e) const;
    wi::ecs::Entity EntityOf (uint32_t netId) const;
    uint32_t OwnerOf (wi::ecs::Entity e) const;   // 0 = the server
    // This machine drives the object's transform (the server for server-owned objects, the
    //	owning client for owner-authority ones; offline: always).
    bool HasAuthority (wi::ecs::Entity e) const;

    struct Info {
        uint32_t netId;
        wi::ecs::Entity entity;
        uint32_t owner;
        std::string key;        // placed: GUID / name path
        std::string templatePath;
        bool bound;             // client: matched to the server's object
    };
    std::vector<Info> List () const;

    // --- driven by NetSession
    void OnSessionStarted (NetSession& session);
    void OnSessionEnded ();
    void OnPlayerJoined (uint32_t clientId, ConnectionId connection);
    void OnPlayerLeft (uint32_t clientId);
    void OnMessage (uint32_t fromClientId, uint8_t type, Reader& r);
    // Once a frame (NetSession::Update): send / receive / interpolate.
    void Update (float dt);

private:
    Replication () = default;

    struct Sample {
        double time;
        NetTransform transform;
    };
    struct Object {
        uint32_t netId = 0;
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        uint32_t owner = 0;
        Options options;
        std::string key;
        std::string templatePath;
        bool registered = false;      // its stNetObject (or Spawn) said it exists here
        bool bound = false;           // client: the server named it
        NetTransform lastSent;
        bool sentOnce = false;
        std::deque<Sample> samples;   // client: received transforms, oldest first
        bool proxy = false;           // its rigid body was made kinematic here
        int localOnlyState = -1;      // last enable state applied to options.localOnly (-1 = never)
        bool wasKinematic = false;
        std::vector<uint8_t> lastState[4];   // server: last bytes sent per state component
        std::unordered_map<std::string, NetValue> vars;
    };

    Object* Find (wi::ecs::Entity e);
    const Object* Find (wi::ecs::Entity e) const;
    Object* FindNet (uint32_t netId);
    std::string KeyOf (wi::scene::Scene& scene, wi::ecs::Entity e) const;
    bool IsServer () const;
    bool IsClient () const;
    uint32_t LocalPlayer () const;
    bool Drives (const Object& o) const;
    void UpdateProxy (Object& o);
    void UpdateLocalOnly (Object& o);
    void ReleaseProxy (Object& o);
    void AssignServerIds ();
    void SendTo (ConnectionId c, const Writer& w, Delivery d);
    void Broadcast (const Writer& w, Delivery d);
    void WriteBind (Writer& w, const Object& o) const;
    void WriteSpawn (Writer& w, const Object& o) const;
    void ServerSendTransforms (bool keyframe);
    void ServerSendState (bool force);
    void ApplyState (Object& o, uint8_t component, const uint8_t* data, size_t size);
    void FireVar (wi::ecs::Entity e, const std::string& name, const NetValue& v);
    void ClientSendOwned ();
    void ClientPlayback ();
    wi::ecs::Entity Instantiate (const std::string& templatePath, const NetTransform& t);
    double Now () const;

    NetSession* session_ = nullptr;
    std::vector<Object> objects_;
    // Client: placed objects the server named before their stNetObject started here.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> pendingByKey_;   // key -> (net id, owner)
    // Client: state and NetVars that arrived for a net id not bound here yet (a joiner gets the
    //	whole world before its own copies of the placed objects have started).
    struct Pending {
        std::vector<uint8_t> state[4];
        std::unordered_map<std::string, NetValue> vars;
    };
    std::unordered_map<uint32_t, Pending> pendingNet_;
    void Bound (Object& o);   // client: o just got its net id - apply what waited for it
    std::unordered_map<std::string, std::unique_ptr<ObjectTemplate>> templates_;
    uint32_t nextNetId_ = 1;
    double epoch_ = 0;
    double serverOffset_ = 0;     // client: server time - local time, smoothed
    bool haveOffset_ = false;
    float sendTimer_ = 0;
    float stateTimer_ = 0;
    std::vector<std::pair<int, VarListener>> varListeners_;
    int nextListener_ = 1;
    float keyframeTimer_ = 0;
};

} // namespace st::net
