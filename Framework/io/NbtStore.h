#pragma once
// PlayerPrefs-like typed key/value store backed by one NBT file.
//
// This is the instance-based core used by both the game-options system
// (st::SettingsManager) and the save-game system (st::savegame). A global,
// Unity-style facade over a default options instance lives in PlayerPrefs.h.
//
// Flat keys behave like Unity's PlayerPrefs (SetInt/GetInt/...). For structured
// data (save games with nested lists/compounds) use Root() and the st::nbt::Tag
// API directly, then Save().
//
//   st::NbtStore prefs(st::userdata::OptionsFile());
//   prefs.Load();
//   int vol = prefs.GetInt("volume", 8);
//   prefs.SetInt("volume", 10);
//   prefs.Save();

#include <string>
#include "Nbt.h"

namespace st {

class NbtStore {
public:
    explicit NbtStore(std::string path);

    // Read the file into memory. A missing file is NOT an error — the store just
    // starts empty. Returns false only on a present-but-corrupt file; the store is empty
    // in that case too, so callers that ignore the result must not then Save() over it.
    bool Load();
    // Write the in-memory compound to disk (atomically). Clears the dirty flag on success.
    bool Save();
    // Discard in-memory state and re-read from disk.
    void Reload();

    const std::string& Path() const { return path_; }
    bool Dirty() const { return dirty_; }

    // ── PlayerPrefs-style flat accessors ────────────────────────────────────────
    void SetInt   (const std::string& key, int         v);
    void SetFloat (const std::string& key, float       v);
    void SetBool  (const std::string& key, bool        v);
    void SetString(const std::string& key, const std::string& v);

    int         GetInt   (const std::string& key, int         def = 0) const;
    float       GetFloat (const std::string& key, float       def = 0.0f) const;
    bool        GetBool  (const std::string& key, bool        def = false) const;
    std::string GetString(const std::string& key, const std::string& def = std::string()) const;

    bool HasKey   (const std::string& key) const;
    void DeleteKey(const std::string& key);
    void DeleteAll();

    // Raw compound root — for nested/structured save-game data.
    nbt::Tag&       Root()       { return root_; }
    const nbt::Tag& Root() const { return root_; }

private:
    std::string path_;
    nbt::Tag    root_ = nbt::Tag::Compound();
    bool        dirty_ = false;
};

} // namespace st
