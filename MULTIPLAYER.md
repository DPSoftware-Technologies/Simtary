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

- `--server-window` keeps the server's window (debugging). `--transport` must match on both ends.
- GNS (UDP, default) needs a numeric IP in `--connect`; TCP also resolves host names.
- `server.cfg`: `port`, `max_players`, `name`, `transport` (key = value, `#` comments); the
  command line overrides it.
- Server console (stdin / dev bridge `net.console {line}`): `help`, `status`, `kick <client id>`,
  `quit`. SIGTERM / Ctrl+C shut it down cleanly.
- Dev bridge: `net.status` (mode, peers + ping, vehicles + positions). A second instance on the
  same folder takes `--devbridge-port=57231` (info file `devbridge_57231.json`).
- B / Shift+B on a client asks the SERVER for the explosion; it detonates and broadcasts it.

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
