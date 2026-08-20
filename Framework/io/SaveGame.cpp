#include "SaveGame.h"
#include "UserData.h"
#include "wiHelper.h"

#include <filesystem>
#include <system_error>

namespace st::savegame {

NbtStore Open(const std::string& slot) {
    NbtStore store(userdata::SavePath(slot));
    store.Load();
    return store;
}

bool Exists(const std::string& slot) {
    return wi::helper::FileExists(userdata::SavePath(slot));
}

bool Delete(const std::string& slot) {
    std::error_code ec; // no-throw overload (exceptions are disabled project-wide)
    return std::filesystem::remove(userdata::SavePath(slot), ec) && !ec;
}

std::vector<std::string> ListSlots() {
    std::vector<std::string> slots;
    std::error_code ec;
    const std::string dir = userdata::SavesDir();
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::filesystem::path& p = it->path();
        if (p.extension() == ".stcd")
            slots.push_back(p.stem().string());
    }
    return slots;
}

} // namespace st::savegame
