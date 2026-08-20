#include "UserData.h"
#include "wiHelper.h"

#ifdef _WIN32
#include <ShlObj.h>      // SHGetKnownFolderPath, FOLDERID_LocalAppDataLow
#include <objbase.h>     // CoTaskMemFree
#else
#include <SDL.h>         // SDL_GetPrefPath
#endif

namespace st::userdata {

namespace {

std::string g_organization = "DPSoftware";
std::string g_application  = "Simtary";

// Resolve the platform base dir and ensure it exists. Called once (function-local static).
std::string ResolveBaseDir() {
    std::string base;

#ifdef _WIN32
    PWSTR wpath = nullptr;
    // LocalLow (not Roaming): the machine-local, non-syncing app-data bucket.
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &wpath)) && wpath) {
        std::wstring w(wpath);
        wi::helper::StringConvert(w, base); // wide -> utf8
    }
    if (wpath) CoTaskMemFree(wpath);
    if (base.empty()) base = "."; // extreme fallback: keep files next to the exe
    base = wi::helper::BackslashToForwardSlash(base);
    if (!base.empty() && base.back() != '/') base += '/';
    base += g_organization + "/" + g_application + "/";
#else
    // SDL creates and returns the org/app pref dir (with trailing slash) for us.
    char* pref = SDL_GetPrefPath(g_organization.c_str(), g_application.c_str());
    if (pref) { base = pref; SDL_free(pref); }
    if (base.empty()) base = "./" + g_organization + "/" + g_application + "/";
    base = wi::helper::BackslashToForwardSlash(base);
    if (!base.empty() && base.back() != '/') base += '/';
#endif

    wi::helper::DirectoryCreate(base); // recursive mkdir; no-op if it already exists
    return base;
}

} // namespace

void Configure(std::string organization, std::string application) {
    if (!organization.empty()) g_organization = std::move(organization);
    if (!application.empty())  g_application  = std::move(application);
}

const std::string& BaseDir() {
    static const std::string base = ResolveBaseDir();
    return base;
}

std::string OptionsFile() {
    return BaseDir() + "options.stad";
}

const std::string& SavesDir() {
    // Resolved and created once; SavePath() is called per slot and must not hit the
    // filesystem every time.
    static const std::string dir = [] {
        std::string d = BaseDir() + "saves/";
        wi::helper::DirectoryCreate(d);
        return d;
    }();
    return dir;
}

std::string SavePath(const std::string& slot) {
    return SavesDir() + slot + ".stcd";
}

} // namespace st::userdata
