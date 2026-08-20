#include "NbtStore.h"
#include "wiHelper.h"

namespace st {

NbtStore::NbtStore(std::string path) : path_(std::move(path)) {}

bool NbtStore::Load() {
    nbt::Tag loaded;
    std::string err;
    if (!nbt::readFile(path_, loaded, nullptr, &err)) {
        root_  = nbt::Tag::Compound();
        dirty_ = false;
        // readFile fails the same way for "no file yet" and "file is corrupt", but the
        // two must not be conflated: a missing file is the normal first run, while
        // reporting success on a corrupt one lets the next Save() overwrite real user
        // data with an empty store.
        return !wi::helper::FileExists(path_);
    }
    if (!loaded.isCompound()) {
        root_  = nbt::Tag::Compound();
        dirty_ = false;
        return false; // present but not a compound root == corrupt
    }
    root_  = std::move(loaded);
    dirty_ = false;
    return true;
}

bool NbtStore::Save() {
    // Unconditional: Root() hands out a mutable reference, so structured edits bypass
    // dirty_ entirely and gating on it here would silently drop them.
    if (!nbt::writeFile(path_, root_, "simtary"))
        return false;
    dirty_ = false;
    return true;
}

void NbtStore::Reload() {
    Load();
}

// ── setters ─────────────────────────────────────────────────────────────────────
void NbtStore::SetInt(const std::string& key, int v)               { root_.putInt(key, v);    dirty_ = true; }
void NbtStore::SetFloat(const std::string& key, float v)           { root_.putFloat(key, v);  dirty_ = true; }
void NbtStore::SetBool(const std::string& key, bool v)             { root_.putBool(key, v);   dirty_ = true; }
void NbtStore::SetString(const std::string& key, const std::string& v) { root_.putString(key, v); dirty_ = true; }

// ── getters ─────────────────────────────────────────────────────────────────────
int NbtStore::GetInt(const std::string& key, int def) const              { return root_.getInt(key, def); }
float NbtStore::GetFloat(const std::string& key, float def) const        { return root_.getFloat(key, def); }
bool NbtStore::GetBool(const std::string& key, bool def) const           { return root_.getBool(key, def); }
std::string NbtStore::GetString(const std::string& key, const std::string& def) const { return root_.getString(key, def); }

bool NbtStore::HasKey(const std::string& key) const { return root_.has(key); }

void NbtStore::DeleteKey(const std::string& key) {
    if (root_.erase(key)) dirty_ = true;
}

void NbtStore::DeleteAll() {
    root_  = nbt::Tag::Compound();
    dirty_ = true;
}

} // namespace st
