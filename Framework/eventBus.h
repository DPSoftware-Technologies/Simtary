#pragma once

#include <functional>
#include <unordered_map>
#include <vector>
#include <string>

// Simple synchronous event bus.
//
// NOT thread-safe: Subscribe and Emit must run on the same thread (the main
// thread here). Background producers (e.g. ZmqHandler) must hand their data to
// the main thread, which then Emits — so subscriber callbacks (scenes) never
// run off-thread and can safely touch the Wicked scene.
//
// No Unsubscribe: a registered callback lives for the lifetime of the bus
// (process lifetime). Subscribers that capture `this` MUST outlive the bus —
// never register a callback from an object that can be destroyed while the bus
// is alive, or its next Emit dereferences a dangling pointer. Scenes satisfy
// this because SceneManager keeps every registered Scene alive for the whole
// app run (it never erases the registry).
class EventBus {
    public:
        // payload carries the event data (e.g. a received ZMQ message body).
        using Callback = std::function<void(const std::string&)>;

        static EventBus& Get() {
            static EventBus instance;
            return instance;
        }

        void Subscribe(const std::string& eventName, Callback callback);
        void Emit(const std::string& eventName, const std::string& payload = "");

    private:
        std::unordered_map<std::string, std::vector<Callback>> listeners;
};
