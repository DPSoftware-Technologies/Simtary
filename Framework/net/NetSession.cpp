#include "net/NetSession.h"
#include "net/Replication.h"

#include "dev/DevBuild.h"
#include "dev/EditorBridge.h"
#include "stRun.h"

#include "wiArguments.h"
#include "wiBacklog.h"
#include "wiScene.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace st::net {

std::unique_ptr<Transport> CreateTcpTransport ();   // TcpTransport.cpp

namespace {

constexpr uint16_t kTickRate = 30;          // snapshots per second (VehicleSync)
constexpr float kHelloTimeout = 5.0f;       // server: connected but silent this long = dropped
constexpr float kWelcomeTimeout = 10.0f;    // client: connected but not welcomed = retry
constexpr float kReconnectDelay = 3.0f;

void Log (const std::string& msg, wi::backlog::LogLevel level = wi::backlog::LogLevel::Default) {
    wi::backlog::post("Net: " + msg, level);
    // A headless server's only window is its terminal / service log. (On Linux the engine's
    // backlog already echoes to stdout.)
#ifdef _WIN32
    if (st::IsHeadless()) {
        printf("Net: %s\n", msg.c_str());
        fflush(stdout);
    }
#endif
}

std::string Trim (const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

NetSession& NetSession::Get () {
    static NetSession instance;
    return instance;
}

void NetSession::LoadConfig () {
    config_.name = appName_;
    std::string value;
    if (wi::arguments::GetArgumentValue("--server-config=", value)) {
        std::ifstream f(value);
        if (!f) {
            Log("cannot read server config " + value, wi::backlog::LogLevel::Warning);
        }
        std::string line;
        while (std::getline(f, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
            if (key == "port") config_.port = uint16_t(atoi(v.c_str()));
            else if (key == "max_players") config_.maxPlayers = uint32_t(std::max(1, atoi(v.c_str())));
            else if (key == "name") config_.name = v;
            else if (key == "transport") config_.transport = v;
            else Log("server config: unknown key '" + key + "'", wi::backlog::LogLevel::Warning);
        }
    }
    // The command line wins over the file.
    if (wi::arguments::GetArgumentValue("--server=", value)) {
        const int p = atoi(value.c_str());
        if (p > 0 && p < 65536) config_.port = uint16_t(p);
    }
    if (wi::arguments::GetArgumentValue("--transport=", value)) config_.transport = value;
}

bool NetSession::StartTransport () {
    if (config_.transport == "tcp") {
        transport_ = CreateTcpTransport();
    } else {
        if (config_.transport != "gns") Log("unknown transport '" + config_.transport + "', using gns", wi::backlog::LogLevel::Warning);
        config_.transport = "gns";
        transport_ = CreateGnsTransport();
        if (!transport_) {
            error_ = "GNS is not in this build (tools/build_gns, then reconfigure) - try --transport=tcp";
            Log(error_, wi::backlog::LogLevel::Error);
            return false;
        }
    }
    return transport_ != nullptr;
}

void NetSession::Initialize (const std::string& appName) {
    appName_ = appName;
    RegisterBridge();

    std::string value;
    if (wi::arguments::GetArgumentValue("--name=", value) && !value.empty()) playerName_ = value;

    const bool server = wi::arguments::HasArgument("--server") ||
                        wi::arguments::GetArgumentValue("--server=", value);
    const bool client = wi::arguments::GetArgumentValue("--connect=", connectTo_);
    std::string hostPort;
    const bool host = wi::arguments::HasArgument("--host") || wi::arguments::GetArgumentValue("--host=", hostPort);
    if (host) {
        if (server || client) {
            error_ = "--host cannot be combined with --server or --connect";
            Log(error_, wi::backlog::LogLevel::Error);
            return;
        }
        LoadConfig();
        StartHost();
        const int hp = atoi(hostPort.c_str());
        if (hp > 0 && hp < 65536) OpenToLan(uint16_t(hp));
        return;
    }
    if (!server && !client) return;
    if (server && client) {
        error_ = "--server and --connect both given; pick one";
        Log(error_, wi::backlog::LogLevel::Error);
        return;
    }
    LoadConfig();
    if (server && st::IsHeadless()) RunConsole();   // before anything can fail: "quit" must work
    if (!StartTransport()) return;

    if (server) {
        if (!transport_->Listen(config_.port, error_)) {
            Log(error_, wi::backlog::LogLevel::Error);
            transport_.reset();
            // A dedicated server that cannot take its port is useless, and running on with no
            // network hides the cause (on a shared box another game often holds 27015). Stop,
            // so the panel / service shows it down with the reason as the last line.
            if (st::IsHeadless()) {
                Log("cannot run as a server - exiting. Pick a free port (--server=<port> / server.cfg)",
                    wi::backlog::LogLevel::Error);
                SDL_Event quit;
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
            }
            return;
        }
        mode_ = Mode::Server;
        ready_ = true;
        vehicles_ = std::make_unique<VehicleSync>(*this);
        Replication::Get().OnSessionStarted(*this);
        Log("server \"" + config_.name + "\" listening on " + (config_.transport == "tcp" ? "TCP " : "UDP ") +
            std::to_string(config_.port) + " (" + transport_->Name() + "), up to " +
            std::to_string(config_.maxPlayers) + " players. Console: help");
        RunConsole();
    } else {
        mode_ = Mode::Client;
        vehicles_ = std::make_unique<VehicleSync>(*this);
        reconnectTimer_ = 0.0f;   // connect on the first Update
    }
}

// Integrated server: the world and one local player in this process, nobody else unless the
// host opens it. The local player is an ordinary client id (1) with no connection, so the
// vehicle code treats it like any other owner - it just reads the keyboard.
bool NetSession::StartHost () {
    if (mode_ != Mode::Off) return false;
    if (config_.name.empty()) config_.name = appName_;
    mode_ = Mode::Host;
    ready_ = true;
    localClientId_ = nextClientId_++;
    vehicles_ = std::make_unique<VehicleSync>(*this);
    Replication::Get().OnSessionStarted(*this);
    NotifyPlayer(localClientId_, true);
    Log("integrated server \"" + config_.name + "\" started (local player #" + std::to_string(localClientId_) +
        ", not open to the network)");
    return true;
}

bool NetSession::StartClient (const std::string& address, const std::string& playerName, const std::string& transport) {
    if (mode_ != Mode::Off || address.empty()) return false;
    error_.clear();
    connectTo_ = address;
    if (!playerName.empty()) playerName_ = playerName;
    config_.transport = transport;
    if (!StartTransport()) return false;
    mode_ = Mode::Client;
    ready_ = false;
    vehicles_ = std::make_unique<VehicleSync>(*this);
    reconnectTimer_ = 0.0f;   // connect on the next Update
    Log("joining " + address + " as \"" + playerName_ + "\"");
    return true;
}

void NetSession::Leave () {
    if (mode_ == Mode::Off) return;
    Log("leaving the network game");
    if (vehicles_) vehicles_->DespawnAll();
    Replication::Get().OnSessionEnded();
    Shutdown();
    connecting_ = false;
    reconnectTimer_ = -1.0f;
    nextClientId_ = 1;
}

bool NetSession::OpenToLan (uint16_t port, const std::string& transport) {
    if (mode_ != Mode::Host) return false;
    if (transport_) return true;
    if (!transport.empty()) config_.transport = transport;
    config_.port = port;
    if (!StartTransport()) return false;
    if (!transport_->Listen(config_.port, error_)) {
        Log(error_, wi::backlog::LogLevel::Error);
        transport_.reset();
        return false;
    }
    Log(std::string("open to the network on ") + (config_.transport == "tcp" ? "TCP " : "UDP ") +
        std::to_string(config_.port) + " (" + transport_->Name() + "), up to " +
        std::to_string(config_.maxPlayers) + " players");
    return true;
}

void NetSession::DropRemotePeers (const char* reason) {
    for (const Peer& p : peers_) {
        if (p.clientId != 0 && vehicles_) vehicles_->OnPlayerLeft(p.clientId);
        if (p.clientId != 0) Replication::Get().OnPlayerLeft(p.clientId);
        if (p.clientId != 0) NotifyPlayer(p.clientId, false);
        if (transport_) transport_->Close(p.connection, reason);
    }
    peers_.clear();
}

void NetSession::CloseLan () {
    if (mode_ != Mode::Host || !transport_) return;
    DropRemotePeers("host closed the game to the network");
    transport_->Shutdown();
    transport_.reset();
    Log("closed to the network");
}

void NetSession::RunConsole () {
    if (consoleStarted_) return;
    consoleStarted_ = true;
    // Blocking stdin reads on their own thread; lines are handled on the main thread in
    // Update. RAW reads (read / ReadFile), not std::getline(std::cin): a thread parked inside
    // getline holds the stream's lock, and the C++ runtime takes that lock again when it tears
    // std::cin down at exit - so "quit" (or a panel's stop) hung the process after main()
    // returned. A raw read holds no library lock; the thread just dies with the process.
    // With no stdin (a GUI launch on Windows) the first read fails and the thread ends.
    std::thread([this] {
        std::string pending;
        char buf[512];
        for (;;) {
#ifdef _WIN32
            DWORD got = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof(buf), &got, nullptr) || got == 0) break;
            const size_t n = got;
#else
            const ssize_t r = read(0, buf, sizeof(buf));
            if (r <= 0) break;
            const size_t n = size_t(r);
#endif
            pending.append(buf, n);
            size_t eol;
            while ((eol = pending.find('\n')) != std::string::npos) {
                std::scoped_lock lck(consoleLock_);
                consoleLines_.push_back(pending.substr(0, eol));
                pending.erase(0, eol + 1);
            }
        }
    }).detach();
}

// The server console. Runs whatever state networking is in: a server that failed to take
// its port still has to answer "quit" (a panel's stop button is exactly that).
void NetSession::ProcessConsole () {
    std::vector<std::string> lines;
    {
        std::scoped_lock lck(consoleLock_);
        lines.swap(consoleLines_);
    }
    for (const std::string& raw : lines) {
        std::istringstream in(Trim(raw));
        std::string cmd;
        in >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "help") {
            Log("console: status | kick <client id> | quit");
        } else if (cmd == "status") {
            uint32_t players = 0;
            for (const Peer& p : peers_) players += p.clientId != 0 ? 1 : 0;
            Log(std::to_string(players) + "/" + std::to_string(config_.maxPlayers) + " players, " +
                std::to_string(vehicles_ ? vehicles_->Vehicles().size() : 0) + " vehicles");
            for (const Peer& p : peers_) {
                Log("  #" + std::to_string(p.clientId) + " " + (p.name.empty() ? "(handshaking)" : p.name) +
                    ", ping " + std::to_string(transport_ ? transport_->Stats(p.connection).pingMs : -1) + " ms");
            }
        } else if (cmd == "kick") {
            uint32_t id = 0;
            in >> id;
            Kick(id, "kicked by the server");
        } else if (cmd == "quit" || cmd == "exit") {
            Log("shutting down (console)");
            SDL_Event quit;
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
        } else {
            Log("unknown command '" + cmd + "' - help");
        }
    }

}

void NetSession::Update (float dt) {
    ProcessConsole();
    if (mode_ == Mode::Off) return;
    if (!transport_) {
        if (vehicles_) vehicles_->Update(dt);   // integrated host, not open to the network
        Replication::Get().Update(dt);
        return;
    }

    // client: (re)connect
    if (mode_ == Mode::Client && reconnectTimer_ >= 0.0f) {
        reconnectTimer_ -= dt;
        if (reconnectTimer_ <= 0.0f) {
            reconnectTimer_ = -1.0f;
            Log("connecting to " + connectTo_ + " as \"" + playerName_ + "\" (" + transport_->Name() + ")");
            if (transport_->Connect(connectTo_, kDefaultPort, error_)) {
                connecting_ = true;
            } else {
                Log(error_ + " - retrying in 3 s", wi::backlog::LogLevel::Warning);
                reconnectTimer_ = kReconnectDelay;
            }
        }
    }

    events_.clear();
    transport_->Poll(events_);
    for (const Event& e : events_) {
        if (IsServer()) OnServerEvent(e);
        else OnClientEvent(e);
    }

    // handshake timeouts
    for (Peer& p : peers_) p.age += dt;
    if (IsServer()) {
        for (const Peer& p : peers_) {
            if (p.clientId == 0 && p.age > kHelloTimeout) transport_->Close(p.connection, "no Hello");
        }
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
            [](const Peer& p) { return p.clientId == 0 && p.age > kHelloTimeout; }), peers_.end());
    } else if (!ready_ && !peers_.empty() && peers_[0].age > kWelcomeTimeout) {
        Log("no Welcome from the server - reconnecting", wi::backlog::LogLevel::Warning);
        transport_->Close(peers_[0].connection, "no Welcome");
        peers_.clear();
        reconnectTimer_ = kReconnectDelay;
    }

    if (vehicles_) vehicles_->Update(dt);
    Replication::Get().Update(dt);

    // A line every 60 s while anyone is connected: who, and how far away.
    statusLogTimer_ += dt;
    if (statusLogTimer_ >= 60.0f && !peers_.empty()) {
        statusLogTimer_ = 0.0f;
        for (const Peer& p : peers_) {
            const ConnectionStats s = transport_->Stats(p.connection);
            Log((p.name.empty() ? std::string("(handshaking)") : p.name) + " ping " + std::to_string(s.pingMs) + " ms");
        }
    }
}

Peer* NetSession::FindPeer (ConnectionId c) {
    for (Peer& p : peers_) {
        if (p.connection == c) return &p;
    }
    return nullptr;
}

void NetSession::Kick (uint32_t clientId, const char* reason) {
    if (!transport_) {
        Log("no client #" + std::to_string(clientId));
        return;
    }
    for (const Peer& p : peers_) {
        if (p.clientId == clientId && clientId != 0) {
            Log("kicking " + p.name + " (#" + std::to_string(clientId) + "): " + reason);
            transport_->Close(p.connection, reason);
            if (vehicles_) vehicles_->OnPlayerLeft(clientId);
            Replication::Get().OnPlayerLeft(clientId);
            NotifyPlayer(clientId, false);
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                [&](const Peer& x) { return x.clientId == clientId; }), peers_.end());
            return;
        }
    }
    Log("no client #" + std::to_string(clientId));
}

void NetSession::SetHandler (uint8_t type, Handler fn) { handlers_[type] = std::move(fn); }

void NetSession::SendToServer (const Writer& w, Delivery d) {
    if (mode_ == Mode::Host) {   // the local player is next to the authority: deliver in place
        const std::vector<uint8_t>& b = w.Bytes();
        if (b.empty() || b[0] < kFirstGameMessage || !handlers_[b[0]]) return;
        Reader r(b);
        r.U8();
        handlers_[b[0]](localClientId_, r);
        return;
    }
    if (mode_ != Mode::Client || !ready_ || peers_.empty()) return;
    transport_->Send(peers_[0].connection, w.Bytes().data(), w.Bytes().size(), d);
}

void NetSession::Broadcast (const Writer& w, Delivery d) {
    if (!IsServer() || !transport_) return;
    for (const Peer& p : peers_) {
        if (p.clientId != 0) transport_->Send(p.connection, w.Bytes().data(), w.Bytes().size(), d);
    }
}

bool NetSession::SendToClient (uint32_t clientId, const Writer& w, Delivery d) {
    if (!IsServer() || !transport_ || clientId == 0) return false;
    for (const Peer& p : peers_) {
        if (p.clientId != clientId) continue;
        transport_->Send(p.connection, w.Bytes().data(), w.Bytes().size(), d);
        return true;
    }
    return false;
}

int NetSession::AddPlayerListener (PlayerListener fn) {
    playerListeners_.emplace_back(nextPlayerListener_, std::move(fn));
    return nextPlayerListener_++;
}

void NetSession::RemovePlayerListener (int handle) {
    playerListeners_.erase(std::remove_if(playerListeners_.begin(), playerListeners_.end(),
                                          [handle](const auto& l) { return l.first == handle; }), playerListeners_.end());
}

void NetSession::NotifyPlayer (uint32_t clientId, bool joined) {
    const auto listeners = playerListeners_;   // a listener may remove itself
    for (const auto& l : listeners) l.second(clientId, joined);
}

void NetSession::Dispatch (const Event& e, Peer* from) {
    Reader r(e.data);
    const uint8_t type = r.U8();
    if (type >= 10 && type < 20) {
        if (vehicles_) vehicles_->OnMessage(e.connection, type, r);
    } else if (type >= 20 && type < 30) {
        Replication::Get().OnMessage(from != nullptr ? from->clientId : 0, type, r);
    } else if (type >= 30 && type < 40) {
        if (scriptSink_) scriptSink_(from != nullptr ? from->clientId : 0, type, r);
    } else if (type >= kFirstGameMessage && handlers_[type]) {
        handlers_[type](from != nullptr ? from->clientId : 0, r);
    }
}

void NetSession::OnServerEvent (const Event& e) {
    switch (e.type) {
        case Event::Type::Connected: {
            Peer p;
            p.connection = e.connection;
            peers_.push_back(p);
            Log("connection " + std::to_string(e.connection) + " up, waiting for Hello");
            break;
        }
        case Event::Type::Disconnected: {
            const Peer* p = FindPeer(e.connection);
            if (p == nullptr) break;
            Log((!p->name.empty() ? p->name : std::to_string(e.connection)) + " left (" + e.reason + ")");
            if (p->clientId != 0 && vehicles_) vehicles_->OnPlayerLeft(p->clientId);
            if (p->clientId != 0) Replication::Get().OnPlayerLeft(p->clientId);
            if (p->clientId != 0) NotifyPlayer(p->clientId, false);
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                [&](const Peer& x) { return x.connection == e.connection; }), peers_.end());
            break;
        }
        case Event::Type::Message: {
            Peer* p = FindPeer(e.connection);
            if (p == nullptr || e.data.empty()) break;
            if (e.data[0] != MsgHello) {
                if (p->clientId != 0) Dispatch(e, p);   // nothing but Hello before the handshake
                break;
            }
            if (p->clientId != 0) break;                // a second Hello: ignore
            Reader r(e.data);
            r.U8();
            const uint16_t protocol = r.U16();
            const std::string name = r.Str();
            if (!r.ok() || protocol != kProtocolVersion) {
                Log("rejected connection " + std::to_string(e.connection) + ": protocol " +
                    std::to_string(protocol) + ", server speaks " + std::to_string(kProtocolVersion),
                    wi::backlog::LogLevel::Warning);
                transport_->Close(e.connection, "protocol version mismatch - update the game");
                break;
            }
            uint32_t players = 0;
            for (const Peer& x : peers_) players += x.clientId != 0 ? 1 : 0;
            if (players >= config_.maxPlayers) {
                Log("rejected " + name + ": server full", wi::backlog::LogLevel::Warning);
                transport_->Close(e.connection, "server full");
                break;
            }
            p->name = name.empty() ? "Player" : name;
            p->clientId = nextClientId_++;
            Writer w(MsgWelcome);
            w.U32(p->clientId);
            w.U16(kTickRate);
            w.Str(config_.name);
            transport_->Send(e.connection, w.Bytes().data(), w.Bytes().size(), Delivery::Reliable);
            Log(p->name + " joined as client " + std::to_string(p->clientId));
            if (vehicles_) vehicles_->OnPlayerJoined(p->clientId, e.connection);
            Replication::Get().OnPlayerJoined(p->clientId, e.connection);
            NotifyPlayer(p->clientId, true);
            break;
        }
    }
}

void NetSession::OnClientEvent (const Event& e) {
    switch (e.type) {
        case Event::Type::Connected: {
            connecting_ = false;
            Peer p;
            p.connection = e.connection;
            p.name = "server";
            peers_.clear();
            peers_.push_back(p);
            Writer w(MsgHello);
            w.U16(kProtocolVersion);
            w.Str(playerName_);
            transport_->Send(e.connection, w.Bytes().data(), w.Bytes().size(), Delivery::Reliable);
            Log("connected, sent Hello");
            break;
        }
        case Event::Type::Disconnected: {
            error_ = e.reason;
            // The server said no on purpose: trying again would only be refused again (or,
            // for a kick, walk straight back in).
            const bool final = e.reason.find("kicked") != std::string::npos ||
                               e.reason.find("server full") != std::string::npos ||
                               e.reason.find("protocol") != std::string::npos;
            Log("disconnected: " + e.reason + (final ? "" : " - reconnecting in 3 s"), wi::backlog::LogLevel::Warning);
            peers_.clear();
            ready_ = false;
            connecting_ = false;
            localClientId_ = 0;
            // The server will hand out a new vehicle on the next Welcome.
            if (vehicles_) vehicles_->DespawnAll();
            Replication::Get().OnSessionEnded();   // the server re-sends the world after the next Welcome
            reconnectTimer_ = final ? -1.0f : kReconnectDelay;
            break;
        }
        case Event::Type::Message: {
            if (e.data.empty()) break;
            if (e.data[0] == MsgWelcome) {
                Reader r(e.data);
                r.U8();
                localClientId_ = r.U32();
                const uint16_t tickRate = r.U16();
                const std::string serverName = r.Str();
                if (!r.ok()) break;
                ready_ = true;
                Replication::Get().OnSessionStarted(*this);
                Log("welcomed by \"" + serverName + "\" as client " + std::to_string(localClientId_) +
                    ", " + std::to_string(tickRate) + " Hz");
                break;
            }
            Dispatch(e, nullptr);
            break;
        }
    }
}

void NetSession::Shutdown () {
    if (transport_) {
        for (const Peer& p : peers_) transport_->Close(p.connection, IsServer() ? "server shutting down" : "client left");
        transport_->Shutdown();
        transport_.reset();
    }
    vehicles_.reset();
    Replication::Get().OnSessionEnded();
    peers_.clear();
    localClientId_ = 0;
    mode_ = Mode::Off;
    ready_ = false;
}

void NetSession::RegisterBridge () {
    if (!st::dev::IsDevBuild()) return;
    st::dev::EditorBridge::Get().Register("net.status", "{} -> network mode, peers, ping, vehicles",
        [this](const st::json::Value&, std::string&) {
            st::json::Value o = st::json::Value::Object();
            o.Set("mode", mode_ == Mode::Server ? "server" : mode_ == Mode::Host ? "host" : mode_ == Mode::Client ? "client" : "off");
            o.Set("listening", transport_ != nullptr && IsServer());
            o.Set("transport", transport_ ? transport_->Name() : "");
            o.Set("ready", ready_);
            o.Set("port", unsigned(config_.port));
            o.Set("localClientId", unsigned(localClientId_));
            o.Set("error", error_);
            st::json::Value list = st::json::Value::Array();
            for (const Peer& p : peers_) {
                st::json::Value po = st::json::Value::Object();
                po.Set("connection", unsigned(p.connection));
                po.Set("clientId", unsigned(p.clientId));
                po.Set("name", p.name);
                if (transport_) {
                    const ConnectionStats s = transport_->Stats(p.connection);
                    po.Set("pingMs", s.pingMs);
                    po.Set("quality", s.quality);
                }
                list.push_back(po);
            }
            o.Set("peers", list);
            st::json::Value vl = st::json::Value::Array();
            if (vehicles_) {
                wi::scene::Scene& scene = wi::scene::GetScene();
                for (const VehicleSync::Vehicle& v : vehicles_->Vehicles()) {
                    st::json::Value vo = st::json::Value::Object();
                    vo.Set("netId", unsigned(v.netId));
                    vo.Set("owner", unsigned(v.owner));
                    vo.Set("entity", unsigned(v.entity));
                    if (const wi::scene::TransformComponent* t = scene.transforms.GetComponent(v.entity)) {
                        const XMFLOAT3 p = t->GetPosition();
                        st::json::Value pa = st::json::Value::Array();
                        pa.push_back(p.x); pa.push_back(p.y); pa.push_back(p.z);
                        vo.Set("position", pa);
                    }
                    vl.push_back(vo);
                }
            }
            o.Set("vehicles", vl);
            return o;
        });
    st::dev::EditorBridge::Get().Register("net.objects", "{} -> replicated objects: netId, entity, owner, key / template, bound, authority, position",
        [](const st::json::Value&, std::string&) {
            st::json::Value list = st::json::Value::Array();
            wi::scene::Scene& scene = wi::scene::GetScene();
            for (const Replication::Info& i : Replication::Get().List()) {
                st::json::Value o = st::json::Value::Object();
                o.Set("netId", unsigned(i.netId));
                o.Set("entity", unsigned(i.entity));
                o.Set("owner", unsigned(i.owner));
                o.Set("key", i.key);
                o.Set("template", i.templatePath);
                o.Set("bound", i.bound);
                o.Set("authority", Replication::Get().HasAuthority(i.entity));
                if (const wi::scene::TransformComponent* t = scene.transforms.GetComponent(i.entity)) {
                    const XMFLOAT3 pos = t->GetPosition();
                    st::json::Value pa = st::json::Value::Array();
                    pa.push_back(pos.x); pa.push_back(pos.y); pa.push_back(pos.z);
                    o.Set("position", pa);
                }
                list.push_back(o);
            }
            return list;
        });
    st::dev::EditorBridge::Get().Register("net.spawn", "{template, position=[0,0,0], owner=0} -> server: spawn a template on every machine, returns entity id",
        [](const st::json::Value& a, std::string& err) {
            const st::json::Value& pv = a["position"];
            const XMFLOAT3 pos((float)pv.At(0).AsNumber(), (float)pv.At(1).AsNumber(), (float)pv.At(2).AsNumber());
            const wi::ecs::Entity e = Replication::Get().Spawn(a["template"].AsString(), pos, XMFLOAT4(0, 0, 0, 1),
                                                                uint32_t(a["owner"].AsNumber()));
            if (e == wi::ecs::INVALID_ENTITY) { err = "spawn failed (not the server, or the template did not load)"; return st::json::Value(); }
            return st::json::Value(uint64_t(e));
        });
    st::dev::EditorBridge::Get().Register("net.despawn", "{id} -> server: remove a replicated object everywhere",
        [](const st::json::Value& a, std::string& err) {
            if (!Replication::Get().Despawn(wi::ecs::Entity(a["id"].AsNumber()))) { err = "not a replicated object here, or not the server"; return st::json::Value(); }
            return st::json::Value(true);
        });
    st::dev::EditorBridge::Get().Register("net.var", "{id, name, value?} -> server: set a NetVar (number / bool / string); any: read it",
        [](const st::json::Value& a, std::string& err) {
            const wi::ecs::Entity e = wi::ecs::Entity(a["id"].AsNumber());
            const std::string name = a["name"].AsString();
            const st::json::Value& v = a["value"];
            if (!v.IsNull()) {
                NetValue nv;
                if (v.IsBool()) { nv.type = NetValue::Type::Bool; nv.b = v.AsBool(); }
                else if (v.IsNumber()) { nv.type = NetValue::Type::Float; nv.f = v.AsNumber(); }
                else { nv.type = NetValue::Type::String; nv.s = v.AsString(); }
                if (!Replication::Get().SetVar(e, name, nv)) { err = "not a replicated object here, or not the server"; return st::json::Value(); }
            }
            const NetValue* got = Replication::Get().GetVar(e, name);
            if (got == nullptr) return st::json::Value();
            if (got->type == NetValue::Type::Bool) return st::json::Value(got->b);
            if (got->type == NetValue::Type::Float) return st::json::Value(got->f);
            if (got->type == NetValue::Type::Int) return st::json::Value(double(got->i));
            return st::json::Value(got->s);
        });
    st::dev::EditorBridge::Get().Register("net.owner", "{id, owner} -> server: give a replicated object to a client (0 = server)",
        [](const st::json::Value& a, std::string& err) {
            if (!Replication::Get().SetOwner(wi::ecs::Entity(a["id"].AsNumber()), uint32_t(a["owner"].AsNumber()))) { err = "not a replicated object here, or not the server"; return st::json::Value(); }
            return st::json::Value(true);
        });
    st::dev::EditorBridge::Get().Register("net.host", "{open?: port, close?: true} -> start the integrated host, open / close it to the network",
        [this](const st::json::Value& a, std::string&) {
            if (mode_ == Mode::Off) StartHost();
            if (a["close"].AsBool()) CloseLan();
            const int port = int(a["open"].AsNumber());
            bool opened = false;
            if (port > 0 && port < 65536) opened = OpenToLan(uint16_t(port));
            st::json::Value o = st::json::Value::Object();
            o.Set("host", mode_ == Mode::Host);
            o.Set("listening", transport_ != nullptr);
            o.Set("opened", opened);
            o.Set("error", error_);
            return o;
        });
    st::dev::EditorBridge::Get().Register("net.join", "{address, name?, transport?} | {leave: true} -> join a server at runtime / go back offline",
        [this](const st::json::Value& a, std::string&) {
            if (a["leave"].AsBool()) Leave();
            else if (!a["address"].AsString().empty()) {
                const std::string t = a["transport"].AsString();
                StartClient(a["address"].AsString(), a["name"].AsString(), t.empty() ? "gns" : t);
            }
            st::json::Value o = st::json::Value::Object();
            o.Set("mode", mode_ == Mode::Server ? "server" : mode_ == Mode::Host ? "host" : mode_ == Mode::Client ? "client" : "off");
            o.Set("error", error_);
            return o;
        });
    st::dev::EditorBridge::Get().Register("net.console", "{line} -> run a server console command",
        [this](const st::json::Value& a, std::string&) {
            std::scoped_lock lck(consoleLock_);
            consoleLines_.push_back(a["line"].AsString());
            st::json::Value o = st::json::Value::Object();
            o.Set("queued", true);
            return o;
        });
}

} // namespace st::net
