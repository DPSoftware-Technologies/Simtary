#pragma once
// st::ModuleHost — host-side loading of the project module.
//
// Only compiled into a build made with simtary_add_app(... MODULE). See stModule.h
// for what the boundary is and why it runs in this direction.

#include "stModule.h"

#include <string>

namespace st {

// A loaded project module. Holds the OS handle so it can be released in the right
// order: the App has to be destroyed through desc->destroy() BEFORE the library it
// was allocated from goes away.
struct Module {
    void*               handle = nullptr;   // HMODULE on Windows, dlopen handle elsewhere
    const stModuleDesc* desc   = nullptr;   // owned by the module, valid until Unload
    std::string         path;               // what was actually opened, for logs
};

// Loads "<exe directory>/<name><platform suffix>" and validates its descriptor:
// the entry symbol exists, the ABI matches, and the build id agrees with the host's.
// Returns false with a player-readable reason in `error` on any of those failing.
bool LoadModule (const std::string& name, Module& out, std::string& error);

// Releases the library. Safe to call on a Module that never loaded.
void UnloadModule (Module& module);

// "--module=<name>" / "--module <name>" from the command line, or an empty string
// when the argument is absent. A development convenience: it lets one host run a
// different module without a rebuild.
std::string ModuleNameFromArgs (int argc, char* argv[]);

} // namespace st
