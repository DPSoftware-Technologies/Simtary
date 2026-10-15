#include "net/NetSession.h"

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
    if (mode_ == Mode::Off || !transport_) return;

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
        if (mode_ == Mode::Server) OnServerEvent(e);
        else OnClientEvent(e);
    }

    // handshake timeouts
    for (Peer& p : peers_) p.age += dt;
    if (mode_ == Mode::Server) {
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
    for (const Peer& p : peers_) {
        if (p.clientId == clientId && clientId != 0) {
            Log("kicking " + p.name + " (#" + std::to_string(clientId) + "): " + reason);
            transport_->Close(p.connection, reason);
            if (vehicles_) vehicles_->OnPlayerLeft(clientId);
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                [&](const Peer& x) { return x.clientId == clientId; }), peers_.end());
            return;
        }
    }
    Log("no client #" + std::to_string(clientId));
}

void NetSession::SetHandler (uint8_t type, Handler fn) { handlers_[type] = std::move(fn); }

void NetSession::SendToServer (const Writer& w, Delivery d) {
    if (mode_ != Mode::Client || !ready_ || peers_.empty()) return;
    transport_->Send(peers_[0].connection, w.Bytes().data(), w.Bytes().size(), d);
}

void NetSession::Broadcast (const Writer& w, Delivery d) {
    if (mode_ != Mode::Server) return;
    for (const Peer& p : peers_) {
        if (p.clientId != 0) transport_->Send(p.connection, w.Bytes().data(), w.Bytes().size(), d);
    }
}

void NetSession::Dispatch (const Event& e, Peer* from) {
    Reader r(e.data);
    const uint8_t type = r.U8();
    if (type >= 10 && type < 20) {
        if (vehicles_) vehicles_->OnMessage(e.connection, type, r);
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
        for (const Peer& p : peers_) transport_->Close(p.connection, mode_ == Mode::Server ? "server shutting down" : "client left");
        transport_->Shutdown();
        transport_.reset();
    }
    vehicles_.reset();
    peers_.clear();
    mode_ = Mode::Off;
    ready_ = false;
}

void NetSession::RegisterBridge () {
    if (!st::dev::IsDevBuild()) return;
    st::dev::EditorBridge::Get().Register("net.status", "{} -> network mode, peers, ping, vehicles",
        [this](const st::json::Value&, std::string&) {
            st::json::Value o = st::json::Value::Object();
            o.Set("mode", mode_ == Mode::Server ? "server" : mode_ == Mode::Client ? "client" : "off");
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
