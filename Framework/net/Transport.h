#pragma once
// st::net::Transport - the one seam between the game's networking and a network library.
// Connections, reliable / unreliable messages, polling; nothing about the game. Backends:
//   GNS  (GnsTransport.cpp)  GameNetworkingSockets over UDP - now
//   TCP                      later, for networks that block UDP (MULTIPLAYER.md)
//
// Main thread only. Poll() once a frame drains everything that arrived since the last call.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace st::net {

using ConnectionId = uint32_t;
constexpr ConnectionId kNoConnection = 0;

enum class Delivery {
    Reliable,     // arrives, in order (joins, spawns, events)
    Unreliable,   // newest wins, may drop (inputs, snapshots)
};

struct Event {
    enum class Type { Connected, Disconnected, Message };
    Type type = Type::Message;
    ConnectionId connection = kNoConnection;
    std::vector<uint8_t> data;   // Message
    std::string reason;          // Disconnected
};

struct ConnectionStats {
    int pingMs = -1;
    float quality = 0.0f;        // 0..1, fraction of packets delivered (local view)
    float outBytesPerSec = 0.0f;
    float inBytesPerSec = 0.0f;
};

class Transport {
public:
    virtual ~Transport () = default;

    virtual const char* Name () const = 0;
    // Server: accept connections on every interface at `port`.
    virtual bool Listen (uint16_t port, std::string& error) = 0;
    // Client: one connection to `address` ("host:port" or "host", default port added).
    virtual bool Connect (const std::string& address, uint16_t defaultPort, std::string& error) = 0;

    virtual void Send (ConnectionId c, const void* data, size_t size, Delivery delivery) = 0;
    virtual void Close (ConnectionId c, const char* reason) = 0;
    virtual void Poll (std::vector<Event>& out) = 0;
    virtual ConnectionStats Stats (ConnectionId c) const = 0;
    virtual void Shutdown () = 0;
};

// nullptr when the build has no GNS (SIMTARY_HAS_GNS).
std::unique_ptr<Transport> CreateGnsTransport ();

} // namespace st::net
