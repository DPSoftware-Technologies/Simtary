#pragma once
// st module ABI - the seam between the host executable and the project module.
//
// In module mode a project ships as two files instead of one:
//
//   <project>.exe      the HOST: engine + framework + the loader main() below
//   application.dll    the MODULE: the game's own code (App subclass, scenes,
//                      components), loaded at startup
//
// The host owns the process, the window, the engine and every global the engine
// keeps. The module owns only game logic. That direction matters: there is exactly
// ONE copy of the engine in the process, so wi::shared_ptr's allocator table,
// wi::jobsystem's worker pool, the ImGui context and every other header-inline
// static have exactly one instance. A module that linked the engine itself would
// get a second copy of all of them and corrupt the heap the first time an object
// crossed the boundary.
//
// This is a MATCHED-VERSION boundary, not a stable ABI. Host and module share C++
// types (st::App, st::AppConfig) and must be built from the same source, the same
// compiler and the same layout-affecting options - SIMTARY_LARGE_WORLD above all,
// which changes sizeof(TransformComponent). ST_ENGINE_BUILD_ID encodes exactly
// that set and is checked at load time, so a mismatched pair refuses to start
// instead of reading a scene at the wrong stride.
//
// A project writes one entry point that works in BOTH modes; see ST_APP_ENTRY at
// the bottom of this file.

#include "stRun.h"

#include <cstdint>

// Bumped whenever stModuleDesc changes shape. The host refuses a module whose ABI
// it does not recognise, which is the check that survives even when the build id
// comparison is disabled.
#define ST_MODULE_ABI 1u

// The one exported symbol the host looks up. A macro so host and module cannot
// disagree about the spelling.
#define ST_MODULE_ENTRY_NAME "stModuleEntry"

// Baked in by simtary_add_app(). The fallbacks keep this header usable on its own
// (an IDE indexer, a unit test) without pretending the values are meaningful.
#ifndef ST_MODULE_NAME
#define ST_MODULE_NAME "application"
#endif
#ifndef ST_ENGINE_BUILD_ID
#define ST_ENGINE_BUILD_ID "unversioned"
#endif

#if defined(_WIN32)
#define ST_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define ST_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern "C" {

// What stModuleEntry() hands back. Read-only, owned by the module, valid until the
// module is unloaded.
struct stModuleDesc {
    // Checked before ANY other field is touched: a module built against a different
    // ST_MODULE_ABI may not even have the rest of this struct laid out the same way.
    uint32_t abi;
    // sizeof(stModuleDesc) as the module saw it. Lets a newer host tell "field is
    // absent" from "field is null" if this struct ever grows; new fields go at the
    // END and existing ones never move.
    uint32_t structSize;

    // Display name, for error messages and the log line at startup.
    const char* name;
    // ST_ENGINE_BUILD_ID as the module was compiled with. Must equal the host's.
    const char* buildId;

    // Fills `config` with the project's properties and returns the App instance.
    // Allocated MODULE-side, so it must be released through destroy() below and
    // never with a plain delete on the host side.
    st::App* (*create)(st::AppConfig& config);
    // Releases what create() returned. Called before the module is unloaded.
    void (*destroy)(st::App* app);
};

typedef const stModuleDesc* (*stModuleEntryFn)(void);

} // extern "C"

// ST_APP_ENTRY
// The project's entry point, written once and valid in both build modes:
//
//     static void ConfigureApp (st::AppConfig& config) {
//         config.name         = ST_PROJECT_NAME;
//         config.startupScene = "Main";
//     }
//     ST_APP_ENTRY(MyGame, ConfigureApp)
//
// Without MODULE this expands to the ordinary int main() that constructs the App
// on the stack and calls st::Run. With MODULE it expands to the exported
// descriptor instead, and the host's loader main() does the same three steps.
// Switching modes is a CMake flag, never a source edit.
#ifdef ST_MODULE_BUILD

#define ST_APP_ENTRY(AppClass, ConfigureFn)                                     \
    static ::st::App* stModuleCreateApp (::st::AppConfig& config) {             \
        ConfigureFn(config);                                                    \
        return new AppClass();                                                  \
    }                                                                           \
    static void stModuleDestroyApp (::st::App* app) {                           \
        delete app;                                                             \
    }                                                                           \
    static const stModuleDesc stModuleDescriptor = {                            \
        ST_MODULE_ABI,                                                          \
        (uint32_t)sizeof(stModuleDesc),                                         \
        ST_MODULE_NAME,                                                         \
        ST_ENGINE_BUILD_ID,                                                     \
        stModuleCreateApp,                                                      \
        stModuleDestroyApp,                                                     \
    };                                                                          \
    ST_MODULE_EXPORT const stModuleDesc* stModuleEntry (void) {                 \
        return &stModuleDescriptor;                                             \
    }

#else

#define ST_APP_ENTRY(AppClass, ConfigureFn)                                     \
    int main (int argc, char* argv[]) {                                         \
        ::st::AppConfig config;                                                 \
        ConfigureFn(config);                                                    \
        AppClass app;                                                           \
        return ::st::Run(argc, argv, config, app);                              \
    }

#endif // ST_MODULE_BUILD
