// TCP backend for st::net::Transport - for networks that block UDP. See Transport.h and
// MULTIPLAYER.md. Plain sockets (Winsock on Windows, BSD sockets elsewhere), non-blocking,
// polled once a frame; no library.
//
// Wire: every frame is  u8 kind | u32 length | payload  (little-endian).
//   kind 0  data      a message for the game
//   kind 1  ping      u64 sender timestamp (microseconds)  - every second, both ways
//   kind 2  pong      the ping's timestamp echoed           -> round trip time
//   kind 3  close     reason text, then the socket closes
//
// TCP is reliable and ordered by construction, so the one thing to manage is staleness:
// an UNRELIABLE message (a snapshot, an input) is dropped instead of queued while more than
// kUnreliableBacklog bytes are still waiting to go out - a late snapshot is worse than none,
// and queueing them behind a congested link is how TCP games fall seconds behind.
// TCP_NODELAY is set: the game batches once a frame itself.

#include "net/Transport.h"

#include "wiBacklog.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define ST_CLOSESOCKET closesocket
#define ST_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
constexpr int kSendFlags = 0;
static int LastSocketError () { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#define ST_CLOSESOCKET close
#define ST_WOULDBLOCK(e) ((e) == EWOULDBLOCK || (e) == EAGAIN || (e) == EINPROGRESS)
constexpr int kSendFlags = MSG_NOSIGNAL;   // a write to a dead peer must not SIGPIPE the server
static int LastSocketError () { return errno; }
#endif

namespace st::net {
namespace {

constexpr uint8_t kData = 0, kPing = 1, kPong = 2, kClose = 3;
constexpr size_t kHeader = 5;
constexpr size_t kMaxFrame = 4u << 20;            // 4 MB: anything bigger is a broken peer
constexpr size_t kUnreliableBacklog = 64u << 10;  // see the header comment
constexpr double kPingInterval = 1.0;
constexpr double kTimeout = 10.0;                 // nothing received this long = gone

double Now () {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint64_t NowMicros () {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

bool SetNonBlocking (socket_t s) {
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void SetNoDelay (socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

class TcpTransport final : public Transport {
public:
    ~TcpTransport () override { Shutdown(); }

    bool Init (std::string& error) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            error = "WSAStartup failed";
            return false;
        }
        wsaStarted_ = true;
#endif
        return true;
    }

    const char* Name () const override { return "TCP"; }

    bool Listen (uint16_t port, std::string& error) override {
        // Dual-stack IPv6 socket: takes IPv4 clients too (as ::ffff:a.b.c.d).
        listen_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        bool v6 = listen_ != kInvalidSocket;
        if (v6) {
            int off = 0;
            setsockopt(listen_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
        } else {
            listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
        if (listen_ == kInvalidSocket) {
            error = "cannot create a TCP socket";
            return false;
        }
        int one = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

        int rc;
        if (v6) {
            sockaddr_in6 a = {};
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_any;
            a.sin6_port = htons(port);
            rc = bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        } else {
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(port);
            rc = bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        }
        if (rc != 0 || listen(listen_, 16) != 0 || !SetNonBlocking(listen_)) {
            error = "cannot listen on TCP port " + std::to_string(port) + " (in use?)";
            ST_CLOSESOCKET(listen_);
            listen_ = kInvalidSocket;
            return false;
        }
        return true;
    }

    bool Connect (const std::string& address, uint16_t defaultPort, std::string& error) override {
        // "host", "host:port", "[v6]:port" - host may be a NAME here (getaddrinfo resolves it).
        std::string host = address, port = std::to_string(defaultPort);
        if (!host.empty() && host[0] == '[') {
            const size_t close = host.find(']');
            if (close != std::string::npos) {
                if (close + 2 < host.size() && host[close + 1] == ':') port = host.substr(close + 2);
                host = host.substr(1, close - 1);
            }
        } else if (host.find(':') != std::string::npos && host.find(':') == host.rfind(':')) {
            port = host.substr(host.find(':') + 1);
            host = host.substr(0, host.find(':'));
        }
        addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
            error = "cannot resolve " + host;
            return false;
        }
        socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == kInvalidSocket || !SetNonBlocking(s)) {
            freeaddrinfo(res);
            error = "cannot create a TCP socket";
            return false;
        }
        const int rc = connect(s, res->ai_addr, int(res->ai_addrlen));
        freeaddrinfo(res);
        if (rc != 0 && !ST_WOULDBLOCK(LastSocketError())) {
            ST_CLOSESOCKET(s);
            error = "connect to " + host + ":" + port + " failed";
            return false;
        }
        SetNoDelay(s);
        Conn& c = Add(s);
        c.connecting = true;
        c.isClient = true;
        return true;
    }

    void Send (ConnectionId id, const void* data, size_t size, Delivery delivery) override {
        Conn* c = Find(id);
        if (c == nullptr || c->closing) return;
        if (delivery == Delivery::Unreliable && c->out.size() > kUnreliableBacklog) {
            ++c->dropped;   // stale before it could leave: skip it, the next one is newer
            return;
        }
        Frame(*c, kData, data, size);
    }

    void Close (ConnectionId id, const char* reason) override {
        Conn* c = Find(id);
        if (c == nullptr || c->closing) return;
        const std::string r = reason != nullptr ? reason : "";
        Frame(*c, kClose, r.data(), r.size());
        Flush(*c);
        c->closing = true;   // removed in the next Poll, quietly: the caller asked for it
    }

    void Poll (std::vector<Event>& out) override {
        // new connections
        if (listen_ != kInvalidSocket) {
            for (;;) {
                socket_t s = accept(listen_, nullptr, nullptr);
                if (s == kInvalidSocket) break;
                SetNonBlocking(s);
                SetNoDelay(s);
                Conn& c = Add(s);
                Push(out, Event::Type::Connected, c.id, "");
            }
        }

        const double now = Now();
        for (size_t i = 0; i < conns_.size();) {
            Conn& c = conns_[i];
            std::string reason;
            bool dead = c.closing;
            if (!dead && c.connecting) dead = !FinishConnect(c, out, reason);
            if (!dead && !c.connecting) dead = !Receive(c, out, reason, now);
            if (!dead && !c.connecting) {
                if (now - c.lastPingSent >= kPingInterval) {
                    const uint64_t t = NowMicros();
                    Frame(c, kPing, &t, sizeof(t));
                    c.lastPingSent = now;
                }
                if (now - c.lastReceive > kTimeout) {
                    dead = true;
                    reason = "timed out";
                }
            }
            if (!dead) dead = !Flush(c);
            if (dead) {
                // A connection the game closed itself is not news; anything else is.
                if (!c.closing && (c.connected || c.isClient)) {
                    Push(out, Event::Type::Disconnected, c.id, reason.empty() ? "connection lost" : reason.c_str());
                }
                ST_CLOSESOCKET(c.socket);
                conns_.erase(conns_.begin() + i);
                continue;
            }
            ++i;
        }
    }

    ConnectionStats Stats (ConnectionId id) const override {
        ConnectionStats s;
        for (const Conn& c : conns_) {
            if (c.id != id) continue;
            s.pingMs = c.pingMs;
            s.quality = c.dropped == 0 ? 1.0f : 1.0f / (1.0f + float(c.dropped) * 0.01f);
        }
        return s;
    }

    void Shutdown () override {
        for (Conn& c : conns_) ST_CLOSESOCKET(c.socket);
        conns_.clear();
        if (listen_ != kInvalidSocket) ST_CLOSESOCKET(listen_);
        listen_ = kInvalidSocket;
#ifdef _WIN32
        if (wsaStarted_) WSACleanup();
        wsaStarted_ = false;
#endif
    }

private:
    struct Conn {
        ConnectionId id = kNoConnection;
        socket_t socket = kInvalidSocket;
        std::vector<uint8_t> in;     // received, not yet framed
        std::vector<uint8_t> out;    // framed, not yet sent
        bool connecting = false;     // client: non-blocking connect in flight
        bool connected = false;
        bool isClient = false;
        bool closing = false;
        double lastReceive = 0.0;
        double lastPingSent = 0.0;
        int pingMs = -1;
        uint64_t dropped = 0;        // unreliable messages skipped under backlog
    };

    Conn& Add (socket_t s) {
        Conn c;
        c.id = nextId_++;
        c.socket = s;
        c.connected = true;
        c.lastReceive = Now();
        conns_.push_back(std::move(c));
        return conns_.back();
    }

    Conn* Find (ConnectionId id) {
        for (Conn& c : conns_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    static void Push (std::vector<Event>& out, Event::Type t, ConnectionId id, const char* reason) {
        Event e;
        e.type = t;
        e.connection = id;
        e.reason = reason;
        out.push_back(std::move(e));
    }

    static void Frame (Conn& c, uint8_t kind, const void* data, size_t size) {
        const uint32_t n = uint32_t(size);
        c.out.push_back(kind);
        const uint8_t* len = reinterpret_cast<const uint8_t*>(&n);
        c.out.insert(c.out.end(), len, len + 4);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        c.out.insert(c.out.end(), p, p + size);
    }

    // Client: has the non-blocking connect finished? True while still pending or done.
    bool FinishConnect (Conn& c, std::vector<Event>& out, std::string& reason) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(c.socket, &w);
        FD_SET(c.socket, &e);
        timeval tv = { 0, 0 };
        const int r = select(int(c.socket) + 1, nullptr, &w, &e, &tv);
        if (r == 0) {
            if (Now() - c.lastReceive > kTimeout) {
                reason = "connect timed out";
                return false;
            }
            return true;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(c.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (r < 0 || FD_ISSET(c.socket, &e) || err != 0) {
            reason = "connection refused";
            return false;
        }
        c.connecting = false;
        c.lastReceive = Now();
        Push(out, Event::Type::Connected, c.id, "");
        return true;
    }

    bool Receive (Conn& c, std::vector<Event>& out, std::string& reason, double now) {
        uint8_t buf[16384];
        for (;;) {
            const int n = int(recv(c.socket, reinterpret_cast<char*>(buf), int(sizeof(buf)), 0));
            if (n > 0) {
                c.in.insert(c.in.end(), buf, buf + n);
                c.lastReceive = now;
                continue;
            }
            if (n == 0) {
                reason = "closed by peer";
                return false;
            }
            if (ST_WOULDBLOCK(LastSocketError())) break;
            reason = "connection reset";
            return false;
        }

        size_t at = 0;
        while (c.in.size() - at >= kHeader) {
            const uint8_t kind = c.in[at];
            uint32_t len;
            memcpy(&len, &c.in[at + 1], 4);
            if (len > kMaxFrame) {
                reason = "malformed stream";
                return false;
            }
            if (c.in.size() - at - kHeader < len) break;
            const uint8_t* payload = c.in.data() + at + kHeader;
            switch (kind) {
                case kData: {
                    Event e;
                    e.type = Event::Type::Message;
                    e.connection = c.id;
                    e.data.assign(payload, payload + len);
                    out.push_back(std::move(e));
                    break;
                }
                case kPing:
                    Frame(c, kPong, payload, len);
                    break;
                case kPong:
                    if (len == 8) {
                        uint64_t t;
                        memcpy(&t, payload, 8);
                        c.pingMs = int((NowMicros() - t) / 1000);
                    }
                    break;
                case kClose:
                    reason.assign(reinterpret_cast<const char*>(payload), len);
                    if (reason.empty()) reason = "closed by peer";
                    return false;
                default:
                    reason = "malformed stream";
                    return false;
            }
            at += kHeader + len;
        }
        c.in.erase(c.in.begin(), c.in.begin() + at);
        return true;
    }

    bool Flush (Conn& c) {
        size_t sent = 0;
        while (sent < c.out.size()) {
            const int n = int(send(c.socket, reinterpret_cast<const char*>(c.out.data() + sent),
                                   int(c.out.size() - sent), kSendFlags));
            if (n > 0) {
                sent += size_t(n);
                continue;
            }
            if (n < 0 && ST_WOULDBLOCK(LastSocketError())) break;
            c.out.erase(c.out.begin(), c.out.begin() + sent);
            return false;
        }
        c.out.erase(c.out.begin(), c.out.begin() + sent);
        return true;
    }

    std::vector<Conn> conns_;
    socket_t listen_ = kInvalidSocket;
    ConnectionId nextId_ = 1;
#ifdef _WIN32
    bool wsaStarted_ = false;
#endif
};

} // namespace

std::unique_ptr<Transport> CreateTcpTransport () {
    auto t = std::make_unique<TcpTransport>();
    std::string error;
    if (!t->Init(error)) {
        wi::backlog::post("Net: " + error, wi::backlog::LogLevel::Error);
        return nullptr;
    }
    return t;
}

} // namespace st::net
