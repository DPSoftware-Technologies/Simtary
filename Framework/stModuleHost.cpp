#ifdef ST_MODULE_HOST

#include "stModuleHost.h"
#include "Simtary.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace st {
namespace {

// Platform file name for a module, so a project's CMakeLists and its AppConfig can
// both talk about "application" and never about "application.dll".
std::string ModuleFileName (const std::string& name) {
#if defined(_WIN32)
    return name + ".dll";
#elif defined(__APPLE__)
    return name + ".dylib";
#else
    return name + ".so";
#endif
}

// The module sits next to the executable, not in the working directory: a shortcut
// or a debugger can start the game from anywhere, and the module is part of the
// install, not part of the content.
std::string ExecutableDirectory () {
    const std::string exePath = wi::helper::GetExecutablePath();
    const size_t cut = exePath.find_last_of("/\\");
    if (cut == std::string::npos)
        return std::string();
    return exePath.substr(0, cut + 1);
}

std::string LastLoadError () {
#ifdef _WIN32
    const DWORD code = GetLastError();
    char buffer[512] = {};
    const DWORD written = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer, (DWORD)sizeof(buffer) - 1, nullptr);
    std::string message = written ? std::string(buffer, written) : std::string("unknown error");
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message + " (error " + std::to_string(code) + ")";
#else
    const char* message = dlerror();
    return message ? std::string(message) : std::string("unknown error");
#endif
}

} // namespace

bool LoadModule (const std::string& name, Module& out, std::string& error) {
    out = Module();

    if (name.empty()) {
        error = "No module name was given.";
        return false;
    }

    out.path = ExecutableDirectory() + ModuleFileName(name);

#ifdef _WIN32
    // The module imports from the host executable by its file name, so the host has
    // to already be in memory under that name — which it is, being the process
    // image. Renaming the exe after the build therefore breaks the load with a
    // "specified module could not be found" that is really about the EXE, not the
    // DLL; that is what the hint below is for.
    HMODULE handle = LoadLibraryA(out.path.c_str());
#else
    // RTLD_GLOBAL: the module resolves engine symbols out of the already-loaded
    // executable, which was linked with --export-dynamic for exactly this.
    void* handle = dlopen(out.path.c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif

    if (handle == nullptr) {
        error = "The application module '" + out.path + "' could not be loaded.\n\n"
              + LastLoadError()
              + "\n\nIf the executable has been renamed, rename it back: the module is "
                "bound to the host by file name.";
        return false;
    }
    out.handle = (void*)handle;

#ifdef _WIN32
    stModuleEntryFn entry = (stModuleEntryFn)GetProcAddress(handle, ST_MODULE_ENTRY_NAME);
#else
    stModuleEntryFn entry = (stModuleEntryFn)dlsym(handle, ST_MODULE_ENTRY_NAME);
#endif
    if (entry == nullptr) {
        error = "'" + out.path + "' is not a Simtary application module: it does not "
                "export " ST_MODULE_ENTRY_NAME "().";
        UnloadModule(out);
        return false;
    }

    const stModuleDesc* desc = entry();
    if (desc == nullptr) {
        error = "The application module '" + out.path + "' returned no descriptor.";
        UnloadModule(out);
        return false;
    }

    // ABI first, and on its own: every field after this one is only known to be
    // where we expect it once the ABI has matched.
    if (desc->abi != ST_MODULE_ABI) {
        error = "The application module '" + out.path + "' was built for module ABI "
              + std::to_string(desc->abi) + ", but this build expects "
              + std::to_string(ST_MODULE_ABI) + ".";
        UnloadModule(out);
        return false;
    }
    if (desc->structSize < sizeof(stModuleDesc)) {
        error = "The application module '" + out.path + "' has a truncated descriptor.";
        UnloadModule(out);
        return false;
    }

    // The matched-version check. Host and module share C++ types, so a difference in
    // the compiler or in any layout-affecting engine option (SIMTARY_LARGE_WORLD
    // changes sizeof(TransformComponent)) means they disagree about struct layout.
    // Refusing here costs a clear message; letting it through costs a corrupt scene
    // graph at some unrelated point later.
    const char* moduleBuildId = desc->buildId ? desc->buildId : "";
    if (strcmp(moduleBuildId, ST_ENGINE_BUILD_ID) != 0) {
        error = std::string("The application module '") + out.path + "' does not match this build.\n\n"
              + "  host:   " ST_ENGINE_BUILD_ID "\n"
              + "  module: " + moduleBuildId + "\n\n"
                "Both files come from the same build and have to be updated together.";
        UnloadModule(out);
        return false;
    }

    if (desc->create == nullptr || desc->destroy == nullptr) {
        error = "The application module '" + out.path + "' has no entry points.";
        UnloadModule(out);
        return false;
    }

    out.desc = desc;
    return true;
}

void UnloadModule (Module& module) {
    if (module.handle != nullptr) {
#ifdef _WIN32
        FreeLibrary((HMODULE)module.handle);
#else
        dlclose(module.handle);
#endif
    }
    module.handle = nullptr;
    module.desc = nullptr;
}

std::string ModuleNameFromArgs (int argc, char* argv[]) {
    static const char* kFlag = "--module";
    const size_t flagLength = strlen(kFlag);

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], kFlag, flagLength) != 0)
            continue;

        // --module=<name>
        if (argv[i][flagLength] == '=')
            return std::string(argv[i] + flagLength + 1);

        // --module <name>
        if (argv[i][flagLength] == '\0' && i + 1 < argc)
            return std::string(argv[i + 1]);
    }
    return std::string();
}

} // namespace st

#endif // ST_MODULE_HOST
