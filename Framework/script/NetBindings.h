#pragma once
// NetBindings - Taipan's networking API (MULTIPLAYER.md v2, step 3).
//
//   Net.IsServer() / IsClient() / IsOnline() / IsHost()    the role of this machine
//   Net.LocalPlayer() -> int                               this machine's player id (0 offline / dedicated)
//   Net.PlayerCount() / PlayerId(i) / PlayerName(id)       who is connected (server)
//
//   remote events - Roblox's RemoteEvent, by name:
//     Net.FireServer("Name", args...)                      client -> server
//     Net.FireClient(player, "Name", args...)              server -> one client
//     Net.FireAllClients("Name", args...)                  server -> every client
//     Net.OnServer("Name", self, "Handler")                func Handler(int player, args...)
//     Net.OnClient("Name", self, "Handler")                func Handler(args...)
//     Net.Off("Name", self)
//   Arguments: int, float, bool, str, Vector2/3, Quaternion, Color, GameObject (a net object;
//   it arrives as the receiver's copy, null if it has none). Offline and on a host the event is
//   delivered in place, so a game written this way also runs single-player.
//
//   objects
//     Net.Spawn("templates/x.sttd", Vector3, Quaternion?, owner?) -> GameObject?   server
//     Net.Despawn(go)                                      server
//     Net.Owner(go) -> int / Net.IsOwner(go) / Net.HasAuthority(go) / Net.SetOwner(go, player)
//     Net.SetVar(go, "name", value) (server) / Net.GetVar(go, "name") -> any
//     Net.OnVar(go, "name", self, "Handler")               func Handler(GameObject obj, str name)
//
//   Server scripts get   func OnPlayerJoined(int player)   func OnPlayerLeft(int player)
//
// Main thread only. No exceptions, no RTTI.

#include "wiECS.h"

namespace st::taipan { struct VM; }

namespace st::script {

void RegisterNetBindings (st::taipan::VM* vm);
// An stScript on `target` is going away: drop its subscriptions.
void ForgetNetSubscriptions (wi::ecs::Entity target);

} // namespace st::script
