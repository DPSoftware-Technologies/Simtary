#pragma once
// Global, Unity-style facade over a single default game-options store.
//
//   st::PlayerPrefs::SetInt("volume", 8);
//   int v = st::PlayerPrefs::GetInt("volume", 5);
//   st::PlayerPrefs::Save();
//
// This is a facade over the very same NbtStore *instance* that st::SettingsManager
// owns — not a second store on the same path. Two independent stores over one file
// would silently clobber each other on Save(), so both routes must share state.
// Use st::NbtStore directly when you genuinely want an owned/separate instance.

#include <string>
#include "NbtStore.h"
#include "SettingsManager.h"

namespace st::PlayerPrefs {

// The default options store — SettingsManager's, loaded by its constructor on first use.
inline NbtStore& Store() { return SettingsManager::Get().Store(); }

inline void SetInt   (const std::string& k, int v)                { Store().SetInt(k, v); }
inline void SetFloat (const std::string& k, float v)              { Store().SetFloat(k, v); }
inline void SetBool  (const std::string& k, bool v)               { Store().SetBool(k, v); }
inline void SetString(const std::string& k, const std::string& v) { Store().SetString(k, v); }

inline int         GetInt   (const std::string& k, int v = 0)                        { return Store().GetInt(k, v); }
inline float       GetFloat (const std::string& k, float v = 0.0f)                   { return Store().GetFloat(k, v); }
inline bool        GetBool  (const std::string& k, bool v = false)                   { return Store().GetBool(k, v); }
inline std::string GetString(const std::string& k, const std::string& v = std::string()) { return Store().GetString(k, v); }

inline bool HasKey   (const std::string& k) { return Store().HasKey(k); }
inline void DeleteKey(const std::string& k) { Store().DeleteKey(k); }
inline void DeleteAll()                     { Store().DeleteAll(); }
inline bool Save()                          { return Store().Save(); }

} // namespace st::PlayerPrefs
