#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Background ZeroMQ receiver.
//
// Owns a zmq context + SUB socket on a dedicated thread. Received message
// bodies are pushed into a thread-safe queue; the MAIN thread drains them via
// Drain() (see st::App::Update) and re-publishes through EventBus. This keeps
// subscriber callbacks — and any Wicked scene/ECS mutation they perform — on
// the main thread, because the Wicked scene is not thread-safe.
//
// C API (zmq.h) only: cppzmq is not vendored because it throws and exceptions
// are disabled project-wide.
class ZmqHandler {
public:
    ZmqHandler() = default;
    ~ZmqHandler();

    ZmqHandler(const ZmqHandler&)            = delete;
    ZmqHandler& operator=(const ZmqHandler&) = delete;

    // Start the receiver thread, connecting a SUB socket to `endpoint`
    // (e.g. "tcp://127.0.0.1:5556"). `topic` is the SUB prefix filter
    // ("" = receive everything). Returns false if the context fails to create;
    // socket/connect failures are reported asynchronously (thread exits).
    // Calling Start while already running is a no-op that returns true.
    bool Start(const std::string& endpoint, const std::string& topic = "");

    // Stop the thread and tear down the socket/context. Idempotent.
    void Stop();

    // MAIN THREAD: pop every queued message in arrival order.
    std::vector<std::string> Drain();

    bool Running() const { return running_.load(); }

private:
    void Run(std::string endpoint, std::string topic);

    void*             ctx_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_{false};

    std::mutex              mtx_;
    std::queue<std::string> inbox_;
};
