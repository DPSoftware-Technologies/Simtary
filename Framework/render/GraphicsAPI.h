#pragma once
// Which graphics backend the process runs on.
//
// The engine can create either a DirectX 12 or a Vulkan device on Windows (Linux is
// Vulkan only), but the backend is fixed for the lifetime of the process: a device
// owns every texture, buffer, pipeline and shader the engine has, so switching it
// means starting over. This module is therefore a STARTUP decision plus a persisted
// preference, not a live toggle - the DevUI Display tab writes the preference and
// offers a restart.
//
// Resolution order, highest first:
//   1. command line          "vulkan" / "dx12"
//   2. options.stad          "display" -> "graphicsAPI" (what the options menu writes)
//   3. AppConfig::graphicsAPI  what the project shipped as its default
//   4. the platform default  DX12 on Windows, Vulkan everywhere else
//
// A choice that cannot be honoured (no Vulkan loader installed, DX12 asked for on
// Linux) falls back to the other backend rather than failing to start; Active() is
// what actually came up, Requested() is what was asked for.
//
// st::Run creates the device from this BEFORE Application::SetWindow, which is the
// seam the engine documents for exactly this ("User can also create a graphics
// device if custom logic is desired, but they must do before this function!").

#include "wiPlatform.h"

#include <memory>
#include <string>

namespace wi::graphics { class GraphicsDevice; }

namespace st {

enum class GraphicsAPI {
    Auto      = 0,   // the platform default
    DirectX12 = 1,
    Vulkan    = 2,
};

// "DirectX 12" / "Vulkan" / "Auto" - for combo boxes and log lines.
const char* GraphicsAPIName(GraphicsAPI api);

// The platform default: DX12 on Windows, Vulkan elsewhere. Never returns Auto.
GraphicsAPI DefaultGraphicsAPI();

// Compiled into this build at all (Simtary_BUILD_DX12 / Simtary_BUILD_VULKAN).
bool GraphicsAPICompiledIn(GraphicsAPI api);

// Compiled in AND the runtime is present on this machine - vulkan-1.dll for Vulkan,
// d3d12.dll for DX12. Checked before the device is created because both engine
// constructors abort the process on failure rather than reporting one.
bool GraphicsAPIAvailable(GraphicsAPI api);

// Resolve the four sources above into one backend. `configured` is
// AppConfig::graphicsAPI. Reads the command line, so wi::arguments::Parse must have
// run. Never returns Auto.
GraphicsAPI ResolveGraphicsAPI(GraphicsAPI configured);

// What was asked for (before the availability fallback), and what came up. Both are
// valid from st::Run onwards; Active() is Auto until the device exists.
GraphicsAPI RequestedGraphicsAPI();
GraphicsAPI ActiveGraphicsAPI();

// Create the device for `api` and point wi::renderer's shader path at the matching
// cache folder ("shaders/hlsl6/" or "shaders/spirv/"). Returns nullptr only when the
// backend is not compiled in - a runtime that is installed but broken still aborts
// inside the engine constructor, which is why GraphicsAPIAvailable() is checked first.
std::unique_ptr<wi::graphics::GraphicsDevice> CreateGraphicsDevice(
    GraphicsAPI api, wi::platform::window_type window);

} // namespace st
