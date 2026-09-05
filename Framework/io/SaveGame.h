#pragma once
// Save-game storage - multiple named slots, each its own NBT file under
// <userdata>/saves/<slot>.stcd. Separate from game options (options.stad).
//
//   st::NbtStore s = st::savegame::Open("slot0");
//   s.SetString("map", "level1");
//   s.SetInt("hp", 100);
//   // structured data via s.Root() (nbt::Tag lists/compounds) for inventory etc.
//   s.Save();
//
// Open() returns a store already Load()ed (empty if the slot doesn't exist yet), so
// the same object serves both "new save" and "load existing".

#include <string>
#include <vector>
#include "NbtStore.h"

namespace st::savegame {

// Open (or start) a save slot. Never fails: a missing file yields an empty store.
NbtStore Open(const std::string& slot);

// True if <saves>/<slot>.stcd exists on disk.
bool Exists(const std::string& slot);

// Delete a slot's file. Returns true if a file was removed.
bool Delete(const std::string& slot);

// Names of all existing slots (files ending in .stcd under the saves dir), sans extension.
std::vector<std::string> ListSlots();

} // namespace st::savegame
