#include "eventBus.h"

void EventBus::Subscribe(const std::string& eventName, Callback callback) {
    listeners[eventName].push_back(std::move(callback));
}

void EventBus::Emit(const std::string& eventName, const std::string& payload) {
    auto it = listeners.find(eventName);
    if (it == listeners.end()) return;
    // Copy the callback list first: a callback may Subscribe (reallocating the
    // vector and rehashing the map), which would invalidate `it` and any
    // iterator into it->second mid-loop.
    auto callbacks = it->second;
    for (auto& cb : callbacks) {
        if (cb) cb(payload);
    }
}
