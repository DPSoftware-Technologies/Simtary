#pragma once
// NetSession - the game's network role for this process: off, dedicated SERVER, or CLIENT.
// Design and steps: Simtary/MULTIPLAYER.md.
//
//   --server[=port]              host (headless unless --server-window, see st::IsHeadless)
//   --server-config=<file>       key = value lines: port, max_players, name, transport
//   --connect=<host>[:port]      join a server (TCP resolves names; GNS wants an IP)
//   --transport=gns|tcp          UDP (GameNetworkingSockets, default) or TCP - both ends alike
//   --name=<player name>         what the server calls you (default "Player")
//
// Handshake: the client sends Hello {protocol, name}; the server answers Welcome {client id,
// tick rate, server name} or closes the connection with the reason (version, full). A
// connection that does not say Hello within 5 s is dropped; a client that loses the server
// reconnects every 3 s until it is back. Vehicles are VehicleSync; anything else the game
// sends goes through SetHandler / SendToServer / Broadcast with a type >= kFirstGameMessage.
//
// Server console (stdin, a terminal or a service's input): help, status, kick <id>, quit.
// A headless server also writes its log lines to stdout.
//
// Main thread only: Initialize() once, Update() once a frame, Shutdown() on exit.
// Dev build: bridge command `net.status`.

#include "net/NetMessage.h"
#include "net/Transport.h"
#include "net/VehicleSync.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace st::net {

enum class Mode { Off, Server, Client };

constexpr uint16_t kDefaultPort = 27015;
constexpr uint16_t kProtocolVersion = 1;

enum MessageType : uint8_t {
    MsgHello = 1,     // C->S reliable: u16 protocol, str name
    MsgWelcome = 2,   // S->C reliable: u32 client id, u16 tick rate, str server name
    // 10..19 VehicleSync
    kFirstGameMessage = 64,
};

struct Peer {
    ConnectionId connection = kNoConnection;
    uint32_t clientId = 0;     // 0 until the handshake finished
    std::string name;
    float age = 0.0f;          // seconds since the connection came up
};

struct ServerConfig {
    uint16_t port = kDefaultPort;
    uint32_t maxPlayers = 16;
    std::string name;          // shown to clients; default = the game's name
    std::string transport = "gns";
};

class NetSession {
public:
    static NetSession& Get ();

    void Initialize (const std::string& appName);
    void Update (float dt);
    void Shutdown ();

    Mode GetMode () const { return mode_; }
    bool IsServer () const { return mode_ == Mode::Server; }
    bool IsClient () const { return mode_ == Mode::Client; }
    bool IsOnline () const { return mode_ != Mode::Off; }
    // Client: handshake done. Server: always true once listening.
    bool IsReady () const { return ready_; }
    uint32_t LocalClientId () const { return localClientId_; }

    const std::vector<Peer>& Peers () const { return peers_; }
    Transport* GetTransport () const { return transport_.get(); }
    const std::string& LastError () const { return error_; }
    VehicleSync* Vehicles () const { return vehicles_.get(); }
    const ServerConfig& Config () const { return config_; }

    // Game messages (type >= kFirstGameMessage). fromClientId is 0 on a client (= the server).
    using Handler = std::function<void(uint32_t fromClientId, Reader& r)>;
    void SetHandler (uint8_t type, Handler fn);
    void SendToServer (const Writer& w, Delivery d);
    void Broadcast (const Writer& w, Delivery d);
    void Kick (uint32_t clientId, const char* reason);

private:
    void OnServerEvent (const Event& e);
    void OnClientEvent (const Event& e);
    void Dispatch (const Event& e, Peer* from);
    Peer* FindPeer (ConnectionId c);
    bool StartTransport ();
    void LoadConfig ();
    void RunConsole ();
    void ProcessConsole ();
    void RegisterBridge ();

    Mode mode_ = Mode::Off;
    ServerConfig config_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<VehicleSync> vehicles_;
    std::vector<Peer> peers_;            // server: every connection; client: the server
    std::vector<Event> events_;
    Handler handlers_[256];
    std::string appName_;
    std::string playerName_ = "Player";
    std::string connectTo_;
    std::string error_;
    uint32_t nextClientId_ = 1;
    uint32_t localClientId_ = 0;
    bool ready_ = false;
    bool connecting_ = false;            // client: a connect is in flight
    float reconnectTimer_ = -1.0f;       // client: >= 0 while waiting to retry
    float statusLogTimer_ = 0.0f;

    std::mutex consoleLock_;
    std::vector<std::string> consoleLines_;
    bool consoleStarted_ = false;
};

} // namespace st::net
