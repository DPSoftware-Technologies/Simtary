#include "render/GraphicsAPI.h"

#include "io/SettingsManager.h"

#include "wiArguments.h"
#include "wiBacklog.h"
#include "wiGraphicsDevice.h"
#include "wiGraphicsDevice_DX12.h"
#include "wiGraphicsDevice_Vulkan.h"
#include "wiRenderer.h"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace st {

namespace {

GraphicsAPI g_requested = GraphicsAPI::Auto;
GraphicsAPI g_active    = GraphicsAPI::Auto;

// Is the backend's runtime installed? Both engine device constructors call
// wi::platform::Exit() when they cannot come up - a message box and a dead process -
// so "try it and catch the failure" is not available. Loading the loader DLL is the
// same thing the constructor does first, and it is cheap and undoable.
bool RuntimePresent(GraphicsAPI api) {
#ifdef _WIN32
    const char* dll = (api == GraphicsAPI::Vulkan) ? "vulkan-1.dll" : "d3d12.dll";
    if (HMODULE m = LoadLibraryA(dll)) {
        FreeLibrary(m);
        return true;
    }
    return false;
#else
    // Linux only builds Vulkan, and volkInitialize does the loader search itself.
    (void)api;
    return true;
#endif
}

// The other one, for the fallback.
GraphicsAPI Other(GraphicsAPI api) {
    return api == GraphicsAPI::Vulkan ? GraphicsAPI::DirectX12 : GraphicsAPI::Vulkan;
}

wi::graphics::ValidationMode ValidationFromArguments() {
    using namespace wi::graphics;
    if (wi::arguments::HasArgument("gpu_verbose"))   return ValidationMode::Verbose;
    if (wi::arguments::HasArgument("gpuvalidation")) return ValidationMode::GPU;
    if (wi::arguments::HasArgument("debugdevice"))   return ValidationMode::Enabled;
    return ValidationMode::Disabled;
}

wi::graphics::GPUPreference PreferenceFromArguments() {
    using namespace wi::graphics;
    if (wi::arguments::HasArgument("igpu"))      return GPUPreference::Integrated;
    if (wi::arguments::HasArgument("nvidiagpu")) return GPUPreference::Nvidia;
    if (wi::arguments::HasArgument("amdgpu"))    return GPUPreference::AMD;
    if (wi::arguments::HasArgument("intelgpu"))  return GPUPreference::Intel;
    return GPUPreference::Discrete;
}

} // namespace

const char* GraphicsAPIName(GraphicsAPI api) {
    switch (api) {
    case GraphicsAPI::DirectX12: return "DirectX 12";
    case GraphicsAPI::Vulkan:    return "Vulkan";
    default:                     return "Auto";
    }
}

GraphicsAPI DefaultGraphicsAPI() {
#ifdef _WIN32
    return GraphicsAPI::DirectX12;
#else
    return GraphicsAPI::Vulkan;
#endif
}

bool GraphicsAPICompiledIn(GraphicsAPI api) {
    switch (api) {
    case GraphicsAPI::DirectX12:
#ifdef Simtary_BUILD_DX12
        return true;
#else
        return false;
#endif
    case GraphicsAPI::Vulkan:
#ifdef Simtary_BUILD_VULKAN
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

bool GraphicsAPIAvailable(GraphicsAPI api) {
    if (api == GraphicsAPI::Auto)
        api = DefaultGraphicsAPI();
    return GraphicsAPICompiledIn(api) && RuntimePresent(api);
}

GraphicsAPI ResolveGraphicsAPI(GraphicsAPI configured) {
    GraphicsAPI wanted = GraphicsAPI::Auto;

    // 1. command line - the developer's override, and it beats a saved preference so
    //    a machine whose options.stad picked the wrong backend is still startable.
    if (wi::arguments::HasArgument("vulkan"))    wanted = GraphicsAPI::Vulkan;
    else if (wi::arguments::HasArgument("dx12")) wanted = GraphicsAPI::DirectX12;

    // 2. the player's saved choice. Read straight out of options.stad rather than off
    //    DisplaySettings: the device has to exist before st::App does, so nothing has
    //    loaded the settings into an object yet.
    if (wanted == GraphicsAPI::Auto) {
        const int stored = SettingsManager::Get()
            .SubCompound("display").getInt("graphicsAPI", (int)GraphicsAPI::Auto);
        if (stored == (int)GraphicsAPI::Vulkan || stored == (int)GraphicsAPI::DirectX12)
            wanted = (GraphicsAPI)stored;
    }

    // 3. what the project shipped, 4. the platform default.
    if (wanted == GraphicsAPI::Auto) wanted = configured;
    if (wanted == GraphicsAPI::Auto) wanted = DefaultGraphicsAPI();

    g_requested = wanted;

    if (GraphicsAPIAvailable(wanted))
        return wanted;

    const GraphicsAPI fallback = Other(wanted);
    if (GraphicsAPIAvailable(fallback)) {
        wilog_warning("Graphics: %s is not available on this machine, falling back to %s.",
                      GraphicsAPIName(wanted), GraphicsAPIName(fallback));
        return fallback;
    }

    // Neither runtime answered. Return the request anyway and let the engine's own
    // device constructor produce the message box - it knows why it failed, we do not.
    return wanted;
}

GraphicsAPI RequestedGraphicsAPI() { return g_requested; }
GraphicsAPI ActiveGraphicsAPI()    { return g_active; }

std::unique_ptr<wi::graphics::GraphicsDevice> CreateGraphicsDevice(
    GraphicsAPI api, wi::platform::window_type window) {

    if (api == GraphicsAPI::Auto)
        api = DefaultGraphicsAPI();

    const wi::graphics::ValidationMode validation = ValidationFromArguments();
    const wi::graphics::GPUPreference  preference = PreferenceFromArguments();

    std::unique_ptr<wi::graphics::GraphicsDevice> device;

    // The shader path gains the backend's subfolder here because we are standing in
    // for Application::SetWindow, which is where the engine would have appended it.
    // Everything that loads a .cso - the engine's ~360 shaders, the ImGui backend,
    // the lens flare, the projector and laser passes - resolves through it.
    if (api == GraphicsAPI::Vulkan) {
#ifdef Simtary_BUILD_VULKAN
        wi::renderer::SetShaderPath(wi::renderer::GetShaderPath() + "spirv/");
        device = std::make_unique<wi::graphics::GraphicsDevice_Vulkan>(window, validation, preference);
#endif
    } else {
#ifdef Simtary_BUILD_DX12
#ifdef PLATFORM_XBOX
        wi::renderer::SetShaderPath(wi::renderer::GetShaderPath() + "hlsl6_xs/");
#else
        wi::renderer::SetShaderPath(wi::renderer::GetShaderPath() + "hlsl6/");
#endif // PLATFORM_XBOX
        device = std::make_unique<wi::graphics::GraphicsDevice_DX12>(validation, preference);
#endif
    }

    if (device != nullptr) {
        g_active = api;
        wilog("Graphics: %s on %s", GraphicsAPIName(api), device->GetAdapterName().c_str());
    }
    return device;
}

} // namespace st
