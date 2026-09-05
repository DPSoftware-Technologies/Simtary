// The host executable's entry point in module mode.
//
// Everything the process needs - engine, framework, window, crash reporter - is
// linked into this executable. What is NOT here is the game: that lives in the
// project module beside it, and the three steps below are the whole of the seam.
//
// The project still writes its entry point once, with ST_APP_ENTRY in stModule.h.
// In this mode that macro produces the module's exported descriptor instead of a
// main(), and this file supplies the main() that drives it.

#ifdef ST_MODULE_HOST

#include "stModuleHost.h"
#include "Simtary.h"

#include <cstdio>

int main (int argc, char* argv[]) {
    // --module=<name> overrides the name baked in at build time. Only a development
    // convenience - a shipped install has exactly one module beside the exe.
    std::string moduleName = st::ModuleNameFromArgs(argc, argv);
    if (moduleName.empty())
        moduleName = ST_MODULE_NAME;

    st::Module module;
    std::string error;
    if (!st::LoadModule(moduleName, module, error)) {
        // Before anything else exists: no window, no log, no crash reporter. A
        // message box is the only thing a player will actually see - and the flush
        // matters, because the box blocks: a redirected stdout would otherwise still
        // be sitting in a buffer while someone reads an empty log.
        printf("%s\n", error.c_str());
        fflush(stdout);
        wi::helper::messageBox(error, "Application could not start");
        return 1;
    }
    printf("Loaded application module: %s\n", module.path.c_str());
    fflush(stdout);

    // The config lives HERE, on the host's stack, because st::Run keeps a pointer to
    // it for the lifetime of the process (see stRun.h). The module only fills it in.
    st::AppConfig config;
    st::App* app = module.desc->create(config);
    if (app == nullptr) {
        error = "The application module '" + module.path + "' failed to start.";
        printf("%s\n", error.c_str());
        fflush(stdout);
        wi::helper::messageBox(error, "Application could not start");
        st::UnloadModule(module);
        return 1;
    }

    const int exitCode = st::Run(argc, argv, config, *app);

    // Order is load-bearing: the App was allocated inside the module, so it has to be
    // released by the module's own destroy() while the module is still mapped.
    module.desc->destroy(app);
    st::UnloadModule(module);
    return exitCode;
}

#else

// Not a module build: this translation unit is empty, and MSVC warns (LNK4221) about
// an object file with no public symbols. One unused symbol is cheaper than teaching
// the source glob about build modes.
namespace st { extern const int stModuleMainUnused; const int stModuleMainUnused = 0; }

#endif // ST_MODULE_HOST
