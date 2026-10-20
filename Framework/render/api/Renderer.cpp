#include "render/api/Renderer.h"

#include <cassert>

namespace st::render {

namespace {
Renderer* g_renderer = nullptr;
}

Renderer& Get () {
    assert(g_renderer != nullptr && "st::render::Get() before st::App installed a renderer");
    return *g_renderer;
}

bool Installed () { return g_renderer != nullptr; }

void Install (Renderer* renderer) { g_renderer = renderer; }

} // namespace st::render
