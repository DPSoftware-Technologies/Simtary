#include "scene/GuiHooks.h"

#include <vector>

namespace st {

GuiHooks& GuiHooks::Get () {
    static GuiHooks instance;
    return instance;
}

int GuiHooks::Add (std::function<void()> draw) {
    const int id = next_++;
    hooks_[id] = std::move(draw);
    return id;
}

void GuiHooks::Remove (int id) { hooks_.erase(id); }

void GuiHooks::Draw () {
    // Copied first: a hook may remove itself (or another) while drawing.
    std::vector<std::function<void()>> draws;
    draws.reserve(hooks_.size());
    for (auto& kv : hooks_) draws.push_back(kv.second);
    for (auto& d : draws) d();
}

} // namespace st
