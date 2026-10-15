#pragma once
// VehicleSync - one vehicle per player, kept in step between the server and every client.
// MULTIPLAYER.md step 2.
//
// Every machine SIMULATES every vehicle (its own Jolt, its own wheels, suspension and sound);
// the server is the authority:
//   client -> server   the driver's input (stVehicle::wanted) at up to 60 Hz, unreliable
//   server             drives each vehicle with its owner's latest input
//   server -> clients  a snapshot at 30 Hz, unreliable: per vehicle the body state AND the
//                      driver input, so a client's copy of someone else's vehicle is driven
//                      by the same input between snapshots
//   client             pulls its copy toward the snapshot (extrapolated by half the ping);
//                      a copy that is far off is teleported. Its OWN vehicle answers its input
//                      at once and is only corrected - that is the whole of v1's prediction.
//
// The scene's first vehicle (an entity with "stVehicle") is the TEMPLATE: captured into an
// archive and removed as soon as the scene has it, then every player's vehicle is a copy.
// A copy that is not the local player's loses the components that only make sense for the
// one you drive (camera, HUD, keyboard input); the server strips sound too.

#include "net/Transport.h"
#include "net/NetMessage.h"

#include "wiArchive.h"
#include "wiECS.h"

#include <cstdint>
#include <vector>

namespace st::net {

class NetSession;

enum VehicleMessage : uint8_t {
    MsgSpawn = 10,      // S->C reliable: u32 net id, u32 owner client id, f32 x, y, z
    MsgDespawn = 11,    // S->C reliable: u32 net id
    MsgInput = 12,      // C->S unreliable: f32 throttle, brake, steer, u8 handbrake, u8 reset count
    MsgSnapshot = 13,   // S->C unreliable: u32 tick, u16 n, n x vehicle state
};

class VehicleSync {
public:
    explicit VehicleSync (NetSession& session) : session_(session) {}

    void Update (float dt);
    void OnMessage (ConnectionId from, uint8_t type, Reader& r);
    // server
    void OnPlayerJoined (uint32_t clientId, ConnectionId connection);
    void OnPlayerLeft (uint32_t clientId);
    void Reset ();
    // Client lost the server: drop every copy, keep the template for the next session.
    void DespawnAll ();

    struct Vehicle {
        uint32_t netId = 0;
        uint32_t owner = 0;               // client id
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        float lastInputAge = 0.0f;        // server: seconds since the owner's last input
        uint8_t lastReset = 0;            // server: the owner's reset count last applied
    };
    const std::vector<Vehicle>& Vehicles () const { return vehicles_; }

private:
    bool CaptureTemplate ();
    wi::ecs::Entity Spawn (uint32_t netId, uint32_t owner, const XMFLOAT3& position);
    void Despawn (uint32_t netId);
    Vehicle* Find (uint32_t netId);
    Vehicle* FindByOwner (uint32_t clientId);
    void SendSnapshot ();
    void SendInput ();
    void ApplySnapshot (Reader& r);
    void Broadcast (const Writer& w, Delivery d);

    NetSession& session_;
    std::vector<Vehicle> vehicles_;
    wi::Archive template_;
    bool haveTemplate_ = false;
    XMFLOAT3 templatePos_ = XMFLOAT3(0, 0, 0);
    XMFLOAT3 templateRight_ = XMFLOAT3(1, 0, 0);
    uint32_t nextNetId_ = 1;
    uint32_t nextSlot_ = 0;
    uint32_t tick_ = 0;
    float snapshotTimer_ = 0.0f;
    float inputTimer_ = 0.0f;
};

} // namespace st::net
