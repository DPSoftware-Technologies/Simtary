// GameNetworkingSockets backend for st::net::Transport. See Transport.h / MULTIPLAYER.md.
//
// GNS reports connection state through ONE global callback (no user pointer), so the active
// transport is reached through a static. One GNS transport per process is all a game needs:
// a server listens, a client connects.

#include "net/Transport.h"

#if defined(SIMTARY_HAS_GNS)

#include "wiBacklog.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <cstring>

namespace st::net {
namespace {

class GnsTransport;
GnsTransport* g_active = nullptr;
int g_libraryRefs = 0;

void DebugOutput (ESteamNetworkingSocketsDebugOutputType type, const char* msg) {
    wi::backlog::post(std::string("GNS: ") + msg,
        type <= k_ESteamNetworkingSocketsDebugOutputType_Error ? wi::backlog::LogLevel::Error
                                                               : wi::backlog::LogLevel::Default);
}

class GnsTransport final : public Transport {
public:
    ~GnsTransport () override { Shutdown(); }

    bool Init (std::string& error) {
        if (g_libraryRefs == 0) {
            SteamNetworkingErrMsg msg;
            if (!GameNetworkingSockets_Init(nullptr, msg)) {
                error = std::string("GameNetworkingSockets_Init: ") + msg;
                return false;
            }
            SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, DebugOutput);
        }
        ++g_libraryRefs;
        initialised_ = true;
        api_ = SteamNetworkingSockets();
        g_active = this;
        return true;
    }

    const char* Name () const override { return "GNS"; }

    bool Listen (uint16_t port, std::string& error) override {
        SteamNetworkingIPAddr addr;
        addr.Clear();
        addr.m_port = port;
        SteamNetworkingConfigValue_t opt = StatusCallback();
        listen_ = api_->CreateListenSocketIP(addr, 1, &opt);
        if (listen_ == k_HSteamListenSocket_Invalid) {
            error = "cannot listen on UDP port " + std::to_string(port) + " (in use?)";
            return false;
        }
        group_ = api_->CreatePollGroup();
        return true;
    }

    bool Connect (const std::string& address, uint16_t defaultPort, std::string& error) override {
        SteamNetworkingIPAddr addr;
        addr.Clear();
        std::string text = address;
        if (text.find(':') == std::string::npos) text += ":" + std::to_string(defaultPort);
        if (!addr.ParseString(text.c_str())) {
            // ParseString takes numeric addresses only; resolving names is the caller's
            // job for now (an IP is what a server list hands out anyway).
            error = "not an IP address: " + address + " (use a numeric IPv4 / IPv6 address)";
            return false;
        }
        SteamNetworkingConfigValue_t opt = StatusCallback();
        client_ = api_->ConnectByIPAddress(addr, 1, &opt);
        if (client_ == k_HSteamNetConnection_Invalid) {
            error = "ConnectByIPAddress failed for " + text;
            return false;
        }
        return true;
    }

    void Send (ConnectionId c, const void* data, size_t size, Delivery delivery) override {
        if (api_ == nullptr || c == kNoConnection) return;
        // No Nagle either way: a game sends its own batches once a frame, and the 5 ms Nagle
        // hold buys nothing but latency.
        const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
                                                         : k_nSteamNetworkingSend_UnreliableNoNagle;
        api_->SendMessageToConnection(c, data, uint32_t(size), flags, nullptr);
    }

    void Close (ConnectionId c, const char* reason) override {
        if (api_ == nullptr || c == kNoConnection) return;
        // No linger: the process may be about to call GameNetworkingSockets_Kill, and a lingering
        // close never reaches the peer then (it times out ~10 s later instead). The reason still
        // travels in the close packet.
        api_->CloseConnection(c, 0, reason, false);
        if (c == client_) client_ = k_HSteamNetConnection_Invalid;
    }

    void Poll (std::vector<Event>& out) override {
        if (api_ == nullptr) return;
        pending_ = &out;
        api_->RunCallbacks();   // status changes -> OnStatusChanged -> out
        pending_ = nullptr;

        SteamNetworkingMessage_t* msgs[64];
        for (;;) {
            int n = 0;
            if (group_ != k_HSteamNetPollGroup_Invalid) {
                n = api_->ReceiveMessagesOnPollGroup(group_, msgs, 64);
            } else if (client_ != k_HSteamNetConnection_Invalid) {
                n = api_->ReceiveMessagesOnConnection(client_, msgs, 64);
            }
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                Event e;
                e.type = Event::Type::Message;
                e.connection = msgs[i]->m_conn;
                const uint8_t* p = static_cast<const uint8_t*>(msgs[i]->m_pData);
                e.data.assign(p, p + msgs[i]->m_cbSize);
                out.push_back(std::move(e));
                msgs[i]->Release();
            }
        }
    }

    ConnectionStats Stats (ConnectionId c) const override {
        ConnectionStats s;
        SteamNetConnectionRealTimeStatus_t rt;
        if (api_ != nullptr && api_->GetConnectionRealTimeStatus(c, &rt, 0, nullptr) == k_EResultOK) {
            s.pingMs = rt.m_nPing;
            s.quality = rt.m_flConnectionQualityLocal;
            s.outBytesPerSec = rt.m_flOutBytesPerSec;
            s.inBytesPerSec = rt.m_flInBytesPerSec;
        }
        return s;
    }

    void Shutdown () override {
        if (!initialised_) return;
        if (client_ != k_HSteamNetConnection_Invalid) api_->CloseConnection(client_, 0, "shutdown", false);
        if (listen_ != k_HSteamListenSocket_Invalid) api_->CloseListenSocket(listen_);
        if (group_ != k_HSteamNetPollGroup_Invalid) api_->DestroyPollGroup(group_);
        client_ = k_HSteamNetConnection_Invalid;
        listen_ = k_HSteamListenSocket_Invalid;
        group_ = k_HSteamNetPollGroup_Invalid;
        if (g_active == this) g_active = nullptr;
        initialised_ = false;
        api_ = nullptr;
        if (--g_libraryRefs == 0) GameNetworkingSockets_Kill();
    }

    void OnStatusChanged (SteamNetConnectionStatusChangedCallback_t* info) {
        const HSteamNetConnection c = info->m_hConn;
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                // Incoming on our listen socket: accept, and route its messages to the group.
                if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
                    if (api_->AcceptConnection(c) != k_EResultOK) {
                        api_->CloseConnection(c, 0, "accept failed", false);
                        break;
                    }
                    api_->SetConnectionPollGroup(c, group_);
                }
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                Push(Event::Type::Connected, c, "");
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // Only connections that were ever up are news to the game; a client whose
                // connect attempt failed hears it too (it has nothing else to wait for).
                const bool wasUp = info->m_eOldState == k_ESteamNetworkingConnectionState_Connected;
                if (wasUp || c == client_) Push(Event::Type::Disconnected, c, info->m_info.m_szEndDebug);
                api_->CloseConnection(c, 0, nullptr, false);
                if (c == client_) client_ = k_HSteamNetConnection_Invalid;
                break;
            }
            default:
                break;
        }
    }

    static void StatusChangedThunk (SteamNetConnectionStatusChangedCallback_t* info) {
        if (g_active != nullptr) g_active->OnStatusChanged(info);
    }

private:
    SteamNetworkingConfigValue_t StatusCallback () const {
        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   reinterpret_cast<void*>(&GnsTransport::StatusChangedThunk));
        return opt;
    }

    void Push (Event::Type type, ConnectionId c, const char* reason) {
        if (pending_ == nullptr) return;
        Event e;
        e.type = type;
        e.connection = c;
        e.reason = reason != nullptr ? reason : "";
        pending_->push_back(std::move(e));
    }

    ISteamNetworkingSockets* api_ = nullptr;
    bool initialised_ = false;
    HSteamListenSocket listen_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup group_ = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection client_ = k_HSteamNetConnection_Invalid;
    std::vector<Event>* pending_ = nullptr;
};

} // namespace

std::unique_ptr<Transport> CreateGnsTransport () {
    auto t = std::make_unique<GnsTransport>();
    std::string error;
    if (!t->Init(error)) {
        wi::backlog::post("Net: " + error, wi::backlog::LogLevel::Error);
        return nullptr;
    }
    return t;
}

} // namespace st::net

#else

namespace st::net {
std::unique_ptr<Transport> CreateGnsTransport () { return nullptr; }
} // namespace st::net

#endif
