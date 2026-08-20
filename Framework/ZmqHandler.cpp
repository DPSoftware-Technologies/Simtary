#include "ZmqHandler.h"

#include <zmq.h>
#include <utility>

bool ZmqHandler::Start(const std::string& endpoint, const std::string& topic) {
    if (running_.load()) return true; // already running
    if (ctx_) return false;           // a prior run left state behind; call Stop() before re-Start

    ctx_ = zmq_ctx_new();
    if (!ctx_) return false;

    running_.store(true);
    thread_ = std::thread(&ZmqHandler::Run, this, endpoint, topic);
    return true;
}

void ZmqHandler::Run(std::string endpoint, std::string topic) {
    void* sub = zmq_socket(ctx_, ZMQ_SUB);
    if (!sub) {
        running_.store(false);
        return;
    }

    // Recv times out periodically so the loop can observe running_ == false and
    // exit, instead of blocking forever on a quiet socket.
    int timeoutMs = 100;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeoutMs, sizeof(timeoutMs));
    // Don't block on close with messages still queued in the socket.
    int linger = 0;
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof(linger));
    // SUB sockets receive nothing until subscribed; "" = everything.
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, topic.data(), topic.size());

    if (zmq_connect(sub, endpoint.c_str()) != 0) {
        zmq_close(sub);
        running_.store(false);
        return;
    }

    while (running_.load()) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        int n = zmq_msg_recv(&msg, sub, 0);
        if (n < 0) {
            zmq_msg_close(&msg);
            int err = zmq_errno();
            if (err == ETERM) break;   // context terminated → shut down
            continue;                  // EAGAIN (timeout) / EINTR → re-check running_
        }

        std::string body(static_cast<const char*>(zmq_msg_data(&msg)),
                         static_cast<size_t>(n));
        zmq_msg_close(&msg);

        {
            std::lock_guard<std::mutex> lock(mtx_);
            inbox_.push(std::move(body));
        }
    }

    zmq_close(sub); // close BEFORE Stop() calls zmq_ctx_term, or term would block
}

std::vector<std::string> ZmqHandler::Drain() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mtx_);
    out.reserve(inbox_.size());
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop();
    }
    return out;
}

void ZmqHandler::Stop() {
    running_.store(false);          // ask the loop to exit (it will within RCVTIMEO)
    if (thread_.joinable()) {
        thread_.join();             // socket is closed at the end of Run()
    }
    if (ctx_) {
        zmq_ctx_term(ctx_);         // safe now: the only socket is already closed
        ctx_ = nullptr;
    }
}

ZmqHandler::~ZmqHandler() {
    Stop();
}
