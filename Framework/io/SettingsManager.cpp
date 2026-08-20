#include "SettingsManager.h"
#include "UserData.h"

namespace st {

SettingsManager::SettingsManager() : store_(userdata::OptionsFile()) {
    store_.Load();
}

SettingsManager& SettingsManager::Get() {
    static SettingsManager instance;
    return instance;
}

nbt::Tag& SettingsManager::SubCompound(const std::string& name) {
    nbt::Tag& root = store_.Root();
    // Only reuse the existing tag if it really is a compound. A corrupt/hand-edited file
    // can leave a scalar under this name, and handing that back would have callers write
    // children into a tag that serializes none of them.
    if (nbt::Tag* g = root.get(name); g && g->isCompound())
        return *g;
    return root.put(name, nbt::Tag::Compound());
}

} // namespace st
