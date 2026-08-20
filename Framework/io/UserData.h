#pragma once
// User-data path resolver for a Simtary app.
//
// All persisted files (game options + save games) live under the platform's per-user
// "local, non-roaming" application data directory:
//
//   Windows:  %USERPROFILE%\AppData\LocalLow\<Organization>\<Application>\
//   Other:    <SDL pref path>/<Organization>/<Application>/   (fallback)
//
// LocalLow is used deliberately (not Roaming): it is the low-integrity, machine-local
// bucket Unity-style games use, so options/saves never sync across machines and match
// the AppData/LocalLow/<Organization>/<Application> layout.
//
// Configure() sets the two names and must be called before the first BaseDir()
// call (st::Main does it from AppConfig); after that the path is fixed.
//
// Directories are created on first access. Options and save games are kept in SEPARATE
// files, with distinct extensions: OptionsFile() (.stad) for game options, SavePath(slot)
// (.stcd) for each save game.

#include <string>

namespace st::userdata {

// Set the organization/application folder names. Call once, before any other
// function here (the resolved base dir is cached on first use).
void Configure(std::string organization, std::string application);

// Base directory, with trailing slash. Resolved once and created (recursively) on first
// call. Returns an empty-ish local path only if the platform lookup fails (very rare).
const std::string& BaseDir();

// <BaseDir>/options.stad — the single game-options file.
std::string OptionsFile();

// <BaseDir>/saves/  (trailing slash). Resolved once and created on first call.
const std::string& SavesDir();

// <BaseDir>/saves/<slot>.stcd — one file per named save slot.
std::string SavePath(const std::string& slot);

} // namespace st::userdata
