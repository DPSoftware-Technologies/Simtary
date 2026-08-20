#pragma once
// Game-options manager: owns the single options.stad store and mediates access to it.
//
// Kept separate from save games (which live in their own per-slot files via
// st::savegame). SettingsManager is the home for graphics/audio/gameplay options.
//
// Graphics (de)serialization itself lives on GraphicsSettings (it owns the private
// EngineGfx snapshot); SettingsManager only hands it the "graphics" sub-compound of
// the store root and persists the file.

#include "NbtStore.h"

namespace st {

class SettingsManager {
public:
    static SettingsManager& Get();

    // Generic option KV (audio, gameplay, ...). Same file as graphics.
    NbtStore& Store() { return store_; }

    void Load() { store_.Load(); }
    bool Save() { return store_.Save(); }

    // A named child compound of the store root, created on demand. Used to give each
    // subsystem its own namespaced section in options.stad (graphics, lensflare, ...).
    nbt::Tag& SubCompound(const std::string& name);

    // Convenience: the "graphics" section. Callers (GraphicsSettings) read/write
    // EngineGfx fields into this tag, then Save().
    nbt::Tag& GraphicsTag() { return SubCompound("graphics"); }

private:
    SettingsManager();
    NbtStore store_;
};

} // namespace st
