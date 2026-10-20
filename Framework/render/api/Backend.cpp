#include "render/api/Backend.h"

#include "wiArguments.h"
#include "wiBacklog.h"

#include <string>

namespace st::render {

bool Compiled (Backend backend) {
    switch (backend) {
    case Backend::Detroit:    return true;   // the engine's own renderer: every build has it
    case Backend::Hurricanes:
#if defined(SIMTARY_HAS_HURRICANES)
        return true;
#else
        return false;
#endif
    }
    return false;
}

const char* BackendName (Backend backend) {
    return backend == Backend::Hurricanes ? "hurricanes" : "detroit";
}

Backend ChosenBackend () {
    static bool decided = false;
    static Backend chosen = Backend::Detroit;
    if (decided) return chosen;
    decided = true;

    std::string asked;
    if (wi::arguments::GetArgumentValue("--renderer=", asked) || wi::arguments::GetArgumentValue("renderer=", asked)) {
        for (char& c : asked) c = (char)std::tolower((unsigned char)c);
        const Backend want = asked == "hurricanes" ? Backend::Hurricanes : Backend::Detroit;
        if (asked != "hurricanes" && asked != "detroit")
            wi::backlog::post("--renderer=" + asked + ": unknown (detroit, hurricanes) - using detroit",
                              wi::backlog::LogLevel::Warning);
        else if (!Compiled(want))
            wi::backlog::post("--renderer=" + asked + ": not in this build (SIMTARY_RENDERERS) - using detroit",
                              wi::backlog::LogLevel::Warning);
        else
            chosen = want;
    }
    return chosen;
}

} // namespace st::render
