#pragma once
// Which renderer a run starts on (EDITIONS.md). A build carries the renderers CMake's
// SIMTARY_RENDERERS lists; one of them is chosen at startup and kept until exit - a renderer
// owns every GPU resource, so changing it is a restart, like changing DirectX 12 / Vulkan.
//
//   --renderer=detroit | --renderer=hurricanes     this run (ignored when not compiled in)
//   otherwise                                       Detroit when compiled in, else Hurricanes
//
// Main thread. No exceptions, no RTTI.

#include "render/api/Renderer.h"

namespace st::render {

bool Compiled (Backend backend);
const char* BackendName (Backend backend);   // "detroit", "hurricanes"
// Decided once, the first time it is asked (st::Run asks before the device exists).
Backend ChosenBackend ();

} // namespace st::render
