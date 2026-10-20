# Multiplayer

Status (2026-10-16): **steps 1-6 done** for the v1 scope - the vehicle proving ground (Milistry
`vehicletest`), every player driving a vehicle, dedicated server on Windows AND Linux, UDP (GNS)
and TCP. Characters join later as one more replicated thing.

## Run it

```
# dedicated server (headless: no window, no GPU, 60 Hz loop, log on stdout, console on stdin)
Milistry.exe --scene=vehicletest --server[=27015] [--server-config=server.cfg] [--transport=tcp]
# Linux: the packaged folder (below) -> ./run_server.sh [same extra args]

# player
Milistry.exe --scene=vehicletest --connect=<host>[:port] --name=<name> [--transport=tcp]
```

```
# integrated host: this process runs the world AND a local player (client id 1)
Milistry.exe --scene=vehicletest --host            # single player, not open to the network
Milistry.exe --scene=vehicletest --host=27020      # same, also open to other players
```

- `--host`: `NetSession::StartHost()`, then `OpenToLan(port)` / `CloseLan()` at runtime (bridge
  `net.host {open: port}` / `{close: true}`). The local player has no connection: its vehicle is
  driven by the local keyboard, remote players join like on a dedicated server, and closing the
  network drops them (their vehicles are removed). `IsServer()` is true for a host as well as a
  dedicated server; `HasLocalPlayer()` tells them apart. `SendToServer` on a host runs the game
  message handler in place.
- `--server-window` keeps the server's window (debugging). `--transport` must match on both ends.
- GNS (UDP, default) needs a numeric IP in `--connect`; TCP also resolves host names.
- `server.cfg`: `port`, `max_players`, `name`, `transport` (key = value, `#` comments); the
  command line overrides it.
- Server console (stdin / dev bridge `net.console {line}`): `help`, `status`, `kick <client id>`,
  `quit`. SIGTERM / Ctrl+C shut it down cleanly.
- Dev bridge: `net.status` (mode, peers + ping, vehicles + positions). A second instance on the
  same folder takes `--devbridge-port=57231` (info file `devbridge_57231.json`).
- B / Shift+B on a client asks the SERVER for the explosion; it detonates and broadcasts it.

## In-game window (F10 in Milistry)

`Framework/net/NetUI` draws a window for host / join / leave: your name, UDP or TCP, `host:port` to join, a
port and "open to other players" to host, the player list with ping and a Kick button, Leave / Stop. A game
calls `st::net::DrawMultiplayerWindow(open, reloadScene)` from `RenderUI()` (Milistry: F10). It uses
`NetSession::StartHost / OpenToLan / StartClient / Leave` (so the same things work at runtime from code and from
the bridge: `net.join {address, name, transport}` / `{leave: true}`), and remembers the form in the options
store (`net.*`). Both sides must be in the same scene; Leave reloads the scene because the session took the
vehicle template away. Verified through the bridge (host started at runtime, a second instance joined, both
saw both vehicles, leaving removed the guest's vehicle); the window itself has NOT been looked at yet.

## Using it in a project

Nothing to switch on: every game built on Simtary has it. The command line decides the role
(`--server` / `--connect`), and the game asks `st::net::NetSession::Get()`:

| call | |
|---|---|
| `IsServer()` / `IsClient()` / `IsOnline()` | the role; offline both are false |
| `IsReady()` | client: welcomed by the server |
| `LocalClientId()` / `Peers()` | who I am, who is connected (`Peer::clientId`, `name`) |
| `SetHandler(type, fn(fromClientId, Reader&))` | receive a game message (`type >= kFirstGameMessage`, 64..255) |
| `SendToServer(Writer, Delivery)` | client -> server |
| `Broadcast(Writer, Delivery)` | server -> every client |
| `Kick(clientId, reason)` | server |
| `st::IsHeadless()` (`stRun.h`) | dedicated server: skip anything visual / audible |

**Vehicles are automatic**: a scene with an `stVehicle` entity is synced - one copy per player.

**Anything else** is a game message. Pattern (Milistry's explosion, `src/main.cpp`): the client
ASKS, the server DECIDES, then tells everyone.

```cpp
#include "net/NetSession.h"

constexpr uint8_t kMsgDoorOpen = st::net::kFirstGameMessage + 1;   // each game picks its own numbers

void RequestDoor (uint32_t doorId) {              // called by gameplay, on any machine
    auto& net = st::net::NetSession::Get();
    st::net::Writer w(kMsgDoorOpen);
    w.U32(doorId);
    if (net.IsClient()) { net.SendToServer(w, st::net::Delivery::Reliable); return; }
    OpenDoor(doorId);                             // offline or server: do it
    if (net.IsServer()) net.Broadcast(w, st::net::Delivery::Reliable);
}

// once, e.g. in App::OnInitialize
st::net::NetSession::Get().SetHandler(kMsgDoorOpen, [](uint32_t from, st::net::Reader& r) {
    const uint32_t doorId = r.U32();
    if (!r.ok()) return;                          // malformed: ignore
    if (st::net::NetSession::Get().IsServer()) RequestDoor(doorId);   // a client asked: validate here
    else OpenDoor(doorId);                        // the server says it happened
});
```

Reliable for things that must arrive (doors, spawns, hits); Unreliable for frequent state where
only the newest matters. Keep messages small - every byte goes to every player.

## Design as built

**Topology: dedicated, authoritative, not peer-to-peer.** The server runs the real scene and
Jolt physics; clients send driver input. GNS is built with `ENABLE_ICE=OFF`.

**Every machine simulates every vehicle** (`Framework/net/VehicleSync`). Jolt is not
bit-identical across machines, so the server's state is the truth, but between snapshots each
client runs its own physics on each vehicle - wheels, suspension and `VehicleSound` all work
unchanged. Per frame:

- client -> server, unreliable, 60 Hz: its vehicle's `stVehicle::wanted` input + reset count
- server: drives each vehicle with its owner's latest input (zeroed after 1 s of silence)
- server -> clients, unreliable, 30 Hz: snapshot = per vehicle position, rotation, linear +
  angular velocity AND the driver input, so remote copies are driven by the same input
- client: extrapolates each snapshot by half the ping, pulls its copy toward it (own vehicle
  10 % per snapshot, others 30 %), teleports if more than 4 m off. Its own vehicle answers its
  input immediately - that is the whole of v1's prediction.

The scene's first `stVehicle` entity is the **template**: captured into an archive and removed
on both sides; every player's vehicle is a copy, spawned side by side. Copies that are not the
local player's lose `stChaseCamera`, `stVehicleHud`, `stInput`; the server also drops
`VehicleSound`. Measured agreement server vs clients: within 0.1 m while driving.

**Game messages**: types >= `kFirstGameMessage` (64) go to handlers the game registers
(`NetSession::SetHandler`, `SendToServer`, `Broadcast`). Milistry's explosion is the first
(`kMsgExplosion`, `src/main.cpp`); a headless server keeps only its physics push.

**Sessions**: Hello {protocol, name} -> Welcome {client id, tick rate, server name}. The server
drops a connection that sends no Hello within 5 s, refuses a wrong protocol version and a full
server. A client that loses the server reconnects every 3 s - except after a kick, "server full"
or a version mismatch.

## Transports (`Framework/net/Transport.h`)

- **GNS** (`GnsTransport.cpp`) - Valve GameNetworkingSockets 1.6.1, prebuilt once per machine:
  `tools/build_gns.ps1` (Windows: BCrypt, protobuf 3.21.12 static -> `deps/gns-install`) and
  `tools/build_gns.sh` (Linux, inside the chroot: OpenSSL + protobuf static ->
  `deps/gns-install-linux`). Imported as `SimtaryGNS`; its exceptions stay behind the .dll/.so.
  Close without linger: a lingering close dies with `GameNetworkingSockets_Kill`.
- **TCP** (`TcpTransport.cpp`) - plain sockets, no library. Frames `u8 kind | u32 length |
  payload` (data, ping, pong, close). Unreliable messages are DROPPED while more than 64 KB are
  still queued (a late snapshot is worse than none); TCP_NODELAY; 10 s receive timeout;
  `MSG_NOSIGNAL` so a dead peer cannot SIGPIPE the server. Ping reads high on one machine
  (~30 ms) because both ends poll once a frame.

## Headless (`st::IsHeadless`, `--server`)

`wi::graphics::GraphicsDevice_Null` (Engine, SIMTARY): every create succeeds, UPLOAD / READBACK
resources get CPU memory, command lists record nothing, `ShaderFormat::NONE` (so
`wi::renderer::LoadShader` loads nothing). The whole engine - scene, terrain with its
heightfield physics, native components - runs unchanged. Around it: SDL without video / audio,
no window (`Application::SetHeadless`), no splash, no loading window, no ImGui platform backend
(ImGui still runs on a fixed display size), the game path not updated or rendered, the terrain's
virtual texture streaming skipped, the loop paced to 60 Hz. ~385 MB, ~15-18 % of one core idle.

## Linux server - portable build

Built in a **Debian 11 chroot** (glibc 2.31) inside WSL so the binary runs on Debian 11+ /
Ubuntu 20.04+; verified running on Debian 13 with nothing installed, a Windows client connected.

```
sudo debootstrap --variant=minbase bullseye /opt/sbx      # once (see tools/linux_chroot.sh)
sudo Simtary/tools/linux_chroot.sh                          # build deps + cmake 3.31 inside
bash Simtary/tools/build_gns.sh                             # once, inside the chroot
cd Milistry && cmake --preset linux_x86-64 -B /root/mbuild -DSIMTARY_NO_SHADERS=ON
cmake --build /root/mbuild --config Release -j16
bash Simtary/tools/package_server_linux.sh /root/mbuild/Release /root/linux_server Milistry vehicletest
```

- `SIMTARY_NO_SHADERS`: the vendored `libdxcompiler.so` needs a newer libstdc++ than Debian 11
  has, and a server renders nothing - so that build compiles no shaders and can only `--server`.
- RUNPATH `$ORIGIN`; SDL2 / OpenAL are copied under their SONAME; the packager copies every
  non-glibc, non-libstdc++ library the binaries load (libX11 + libxcb and friends included).
- sentry is built with `SENTRY_TRANSPORT=none` (reports stay on disk), so no libcurl.
- glibc < 2.34 keeps pthread separate: `stpack` links `Threads::Threads`; gcc 10's library has
  no `std::sinf`/`std::expf` (use `std::sin`/`std::exp`).
- Output: `Milistry/build/linux_server/` and `milistry_server_linux_x86-64.tar.gz` (~99 MB).
  Open UDP 27015 (or the TCP port) on the host.

## Hosting notes (learned on a real VPS through PufferPanel)

- PufferPanel template: `Milistry/tools/pufferpanel/milistry-server.json` (host environment;
  downloads the tar.gz, writes `server.cfg` from the panel variables on every start, stop = `quit`
  on stdin). One-step portable build: `Simtary/tools/build_server_linux.sh` (in the chroot).
- Intel VMs may leave CPUID leaf 0xB empty: `Engine/Utility/cpuinfo.hpp` divided by the 0
  threads-per-core it reports - SIGFPE before the first engine log line. Guarded.
- The console thread reads stdin with raw `read` / `ReadFile`: a thread parked in
  `std::getline(std::cin)` holds the stream lock the runtime needs at exit, so `quit` hung.
- Headless stdout is line buffered (a pipe is block buffered and a crash ate the last lines);
  a headless crash prints signal + faulting module/offset + stack to stderr (no reporter GUI).
- The panel's sandbox hides `/sys`; only crashpad complains, the game does not need it.

## Not done / next

- Client-side prediction with replay (own vehicle reacts at once already; corrections are soft)
- Snapshot delta compression and interest management (fine for 2-16 players as is)
- Scene choice from the server (both ends launch `--scene=vehicletest` today)
- Characters, then weapons
- Steam relay / NAT traversal (not wanted for dedicated servers so far)

## v2: replication and scripted networking (design, 2026-10-20)

**Step 1 done (2026-10-20)**: `net/Replication.*` + `net/NetObjectComponent.cpp` ("stNetObject").
Verified with a headless server and a client on one PC (bridge `net.objects / net.spawn /
net.despawn / net.owner`): a spawned template appears on the client and its copy follows the
server's physics ~0.1 s behind; despawn removes it; after `net.owner` the client moves it and
the server follows as a kinematic proxy; a placed object (matched by key `path:SunLight`) binds
whichever side registers first. Found on the way: the Windows headless server fast-failed in
`setvbuf(stdout, nullptr, _IOLBF, 0)` (MSVC rejects it) - now unbuffered on Windows. A headless
server only builds terrain around its camera/vehicles: objects elsewhere fall through.

Goal: Roblox's "build it once and it is multiplayer". v1 synchronises vehicles only; anything
else is a hand-written game message. v2 makes ANY entity replicable, gives Taipan scripts a
networking API, and lets the editor start a server plus clients on one PC. Built on what v1
has: `NetSession` (roles, handshake, transports, game messages), `Writer` / `Reader`, the
headless server. VehicleSync stays as it is for now; vehicles move onto v2 once it is proven.

### The model

- **Server authoritative.** The server's scene is the truth. Clients load the same map, so
  objects placed in it already exist on every machine; objects made at runtime are spawned by
  the server and appear on clients.
- **What is replicated is chosen per entity**: the `stNetObject` native component on an entity
  (root or child). Everything else stays local (effects, UI, the camera).
- **Ownership**: every net object has an owner - the server (0) or one client. The owner of a
  player's character / vehicle can be given authority over its transform (it moves at once,
  the server re-sends it to everyone else). Server-owned objects are only ever moved by the
  server; on clients they are kinematic proxies.

### Identity

| object | key on the wire | how a client finds its copy |
|---|---|---|
| placed in the map | the entity's `EntityGUID` (metadata, saved with the map; the editor stamps it when `stNetObject` is attached) | `FindEntityByGUID` in its own copy of the map |
| spawned at runtime | a net id + the template path (`.sttd`) | instantiates the template (`ObjectTemplate`) |

Net ids are the server's, 32-bit, never reused within a session.

### Steps

| # | What | Done when |
|---|---|---|
| 1 | **Transform replication.** `stNetObject` (fields: `syncTransform`, `ownerAuthority`); `net/Replication.*` owned by `NetSession`. Server: binds placed objects (GUID), spawns template objects, sends transforms at 20 Hz (unreliable, changed objects + a full keyframe every second), `Despawn` when an object goes. Client: binds / spawns / removes, interpolates 100 ms behind the server clock, makes non-owned rigid bodies kinematic. Owner authority: the owning client sends its object's transform up, the server applies it and passes it on. Joining clients get the whole world state. | Two instances: a box pushed on the server moves on the client; a spawned template appears on both; owner-moved object moves on the other client. |
| 2 | **State replication.** Whatever else changes on a server object (material colour, light, a component field) reaches clients: per object, the components are archived (as `SceneSnapshot` does) at 10 Hz, compared with what was last sent, and re-sent reliably when they differ. `NetVar`s: small named values on a net object (int / float / bool / string / vector / colour), set on the server, readable everywhere, with a change callback. | Changing a light colour on the server shows on the client; a NetVar set by a server script is read by a client script. |
| 3 | **Taipan networking.** `Net.IsServer / IsClient / IsOnline / LocalPlayer / PlayerCount / PlayerId / PlayerName`; remote events `Net.FireServer(name, ...)`, `Net.FireClient(player, name, ...)`, `Net.FireAllClients(name, ...)`, received through `Net.On(name, self, "Handler")` (server handlers get the sender's player id first); `Net.Spawn(template, pos, rot, owner)`, `Net.Despawn(go)`, `Net.Owner / IsOwner / SetOwner`; `Net.SetVar / GetVar`; `OnPlayerJoined(int)` / `OnPlayerLeft(int)` called on server scripts. Arguments: int, float, bool, str, Vector2/3, Quaternion, Color, GameObject (sent as its net id). Script role per stScript: `runOn` = Both / Server / Client. | A client script fires an event, the server script answers every client, each logs it. |
| 4 | **Test from the editor.** Toolbar "Play > Server + N clients": saves the scene to a temp map, starts a headless server and N client windows on 127.0.0.1 (tiled, `--name=Player1..N`, own bridge ports), Stop closes them. The editor itself stays in Edit mode. | One click gives a server and two clients of the open map. |
| 5 | **Players.** A player template per scene (`stNetPlayerSpawn`: template + spawn points): the server spawns one per joining client, owned by it with transform authority; Milistry's soldier and vehicles use it. | Two players walking/driving in the same world. |

### Messages (types 20..39; VehicleSync keeps 10..19)

| id | dir | delivery | content |
|---|---|---|---|
| 20 Bind | S->C | reliable | u32 net id, str guid, u32 owner |
| 21 Spawn | S->C | reliable | u32 net id, str template, u32 owner, transform |
| 22 Despawn | S->C | reliable | u32 net id |
| 23 Transforms | S->C | unreliable | f64 server time, u16 n, n x (u32 net id, transform) |
| 24 OwnerTransforms | C->S | unreliable | u16 n, n x (u32 net id, transform) |
| 25 Owner | S->C | reliable | u32 net id, u32 owner |
| 26 State | S->C | reliable | u32 net id, archive bytes (step 2) |
| 27 Var | S->C | reliable | u32 net id, str name, value (step 2) |
| 30 Event | both | reliable | str name, u8 n, n x value (step 3) |

Transform on the wire: u8 flags (large-world position, has scale), position (f64 x3 for a
large-world transform, else f32 x3 `translation_local`), rotation f32 x4, scale f32 x3 if
flagged. Local transforms are sent, not world: both machines have the same hierarchy.

### Not in v2
Client-side prediction with rollback, lag compensation (hit rewind), delta compression,
interest management / relevancy, cheat validation beyond speed limits. All later, all behind
the same messages.
