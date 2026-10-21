#pragma once
// NetSession - the game's network role for this process: off, dedicated SERVER, or CLIENT.
// Design and steps: Simtary/MULTIPLAYER.md.
//
//   --server[=port]              host (headless unless --server-window, see st::IsHeadless)
//   --server-config=<file>       key = value lines: port, max_players, name, transport
//   --connect=<host>[:port]      join a server (TCP resolves names; GNS wants an IP)
//   --host[=port]                INTEGRATED server: this process runs the world AND has a local
//                                player (client id 1); with a port it also opens to the network
//                                (same as OpenToLan at runtime). Single-player is this, closed.
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

enum class Mode { Off, Server, Client, Host };

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
    // Runtime entry to the integrated host (what --host does); only from Mode::Off.
    bool StartHost ();
    // Runtime entry to a game on another machine (what --connect does); only from Mode::Off. The client
    // reconnects by itself until Leave(). `transport` is "gns" (UDP) or "tcp" and must match the server's.
    bool StartClient (const std::string& address, const std::string& playerName, const std::string& transport = "gns");
    // Back to offline from any mode: drops the connection / the host, forgets every network vehicle. The
    // scene lost the vehicle template when the session first took it, so the caller reloads the scene.
    void Leave ();
    const std::string& PlayerName () const { return playerName_; }
    void SetPlayerName (const std::string& name) { if (!name.empty()) playerName_ = name; }
    void Update (float dt);
    void Shutdown ();

    Mode GetMode () const { return mode_; }
    // The world's authority: a dedicated server OR an integrated host.
    bool IsServer () const { return mode_ == Mode::Server || mode_ == Mode::Host; }
    bool IsHost () const { return mode_ == Mode::Host; }
    // The authority also has a player of its own on this machine (client id LocalClientId()).
    bool HasLocalPlayer () const { return mode_ == Mode::Host; }
    bool IsListening () const { return transport_ != nullptr && IsServer(); }
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
    const std::string& ConnectAddress () const { return connectTo_; }

    // Game messages (type >= kFirstGameMessage). fromClientId is 0 on a client (= the server).
    using Handler = std::function<void(uint32_t fromClientId, Reader& r)>;
    void SetHandler (uint8_t type, Handler fn);
    void SendToServer (const Writer& w, Delivery d);
    void Broadcast (const Writer& w, Delivery d);
    // Server: one client. False when there is no such (remote) client.
    bool SendToClient (uint32_t clientId, const Writer& w, Delivery d);

    // Players arriving and leaving (server / host; the host's own player "joins" when it starts).
    using PlayerListener = std::function<void(uint32_t clientId, bool joined)>;
    int AddPlayerListener (PlayerListener fn);
    void RemovePlayerListener (int handle);
    // Script-level messages (types 30..39: Taipan remote events), routed to whoever set this.
    using RawHandler = std::function<void(uint32_t fromClientId, uint8_t type, Reader& r)>;
    void SetScriptSink (RawHandler fn) { scriptSink_ = std::move(fn); }
    void Kick (uint32_t clientId, const char* reason);

    // Integrated host only. Open the world to other machines / close it again (remote players
    // are dropped). OpenToLan returns false with LastError() set when the port cannot be taken.
    bool OpenToLan (uint16_t port, const std::string& transport = "");
    void CloseLan ();

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
    void DropRemotePeers (const char* reason);

    Mode mode_ = Mode::Off;
    ServerConfig config_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<VehicleSync> vehicles_;
    std::vector<Peer> peers_;            // server: every connection; client: the server
    std::vector<Event> events_;
    Handler handlers_[256];
    RawHandler scriptSink_;
    std::vector<std::pair<int, PlayerListener>> playerListeners_;
    int nextPlayerListener_ = 1;
    void NotifyPlayer (uint32_t clientId, bool joined);
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
