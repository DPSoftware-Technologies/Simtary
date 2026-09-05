#pragma once
// st::AssetSystem - the runtime side: mount packs, serve the engine out of them, and
// load a .stsd map into a wi::scene::Scene.
//
// This is the only file in Framework/io/asset that knows the engine exists. Everything
// under it (AssetPack, AssetPackWriter, SceneDescriptor) is plain C++ so the build-time
// packer can link it without pulling in a graphics device.
//
// How the engine ends up reading from a pack
//
// It does not have to be told. `wi::helper::SetAssetSourceOverride` is a seam the
// engine already routes every FileRead and FileExists through, so installing one
// override redirects the whole engine - resource manager, streaming, video, scripts
// at once. A path the packs do not have falls through to the real filesystem, which is
// what keeps shader caches, save data and loose development assets working next to a
// packed game.
//
//   st::AssetSystem::Get().Mount("assets/content.strd");   // before wi::initializer
//   st::AssetSystem::Get().Install();
//
// Mip and audio streaming survive the move because the override honours the engine's
// offset/length arguments, and because the packer stores already-compressed formats
// verbatim: a ranged read of a stored asset is a ranged read of a mapped page, no
// decompression and no copy of the parts nobody asked for.
//
// Mount points
//
// A pack's logical paths are relative to the content root ("scenes/s1map.stsd",
// "textures/wall.dds"), while a running game asks for "assets/scenes/s1map.stsd",
// because the build copies assets/contents/ to <exe>/assets/. `mountPoint` is the
// prefix the override strips before looking up, so both spellings resolve and neither
// side has to change. Resources lifted out of a .wiscene keep the relative names the
// engine stored, so they land in the same namespace with no prefix at all.
//
// Order
//
// Later mounts win. That is what makes a patch pack work: ship content.strd once,
// then mount patch1.strd over it and every asset it redefines shadows the original,
// with no rebuild of the base.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AssetPack.h"
#include "SceneDescriptor.h"

#include "CommonInclude.h"
#include "wiECS.h"
#include "wiScene.h"

namespace st {

// What the packs are serving right now.
//
// This exists because of a specific dead spot in a scene load. The engine reports
// progress per COMPONENT MANAGER while it deserialises ("Loading materials", "Loading
// meshes"), and then, between the last of those and "Processing assets", it waits for
// every texture the scene referenced to finish loading. That wait is most of the load
// on a big map, and nothing reports during it - the bar sits still and the player has
// no idea whether the game is working or hung.
//
// Every one of those texture loads is a read through this class, so this is the one
// place that knows which asset is in flight. `assetsExpected` comes from the .stsd's
// own reference list, which is why the count is a real "7 of 14" and not a guess.
struct AssetLoadProgress {
    uint32_t    assetsRead     = 0;   // pack reads served since the load started
    uint32_t    assetsExpected = 0;   // from the map's asset list; 0 when not loading a map
    uint64_t    bytesRead      = 0;
    std::string currentAsset;         // logical path of the most recent read
    uint64_t    currentSize    = 0;   // its uncompressed size
};

class AssetSystem {
public:
    static AssetSystem& Get ();

    // mounting

    // Open a .strd (and its parts) and add it to the search order. Mount before
    // wi::initializer runs if the engine will read from it during start-up.
    bool Mount (const std::string& strdPath,
                const std::string& mountPoint = "assets/",
                std::string* error = nullptr,
                bool verify = false);

    void Unmount    (const std::string& strdPath);
    void UnmountAll ();

    uint32_t MountCount () const { return static_cast<uint32_t>(mounts_.size()); }

    // engine hook

    // Route wi::helper::FileRead / FileExists through the mounted packs. Idempotent.
    // Uninstall() restores plain filesystem behaviour, which the DevUI explorer uses
    // to compare a packed asset against the loose file it came from.
    void Install ();
    void Uninstall ();
    bool IsInstalled () const { return installed_; }

    // direct access

    // Search every mount, last first. `outPack` receives the pack that answered.
    const asset::StrdAsset* Find (const std::string& logicalPath,
                                   const asset::AssetPack** outPack = nullptr) const;

    bool Exists (const std::string& logicalPath) const;
    bool Read   (const std::string& logicalPath, std::vector<uint8_t>& out,
                 std::string* error = nullptr) const;

    // scenes

    // Load a .stsd into `scene`, mirroring wi::scene::LoadModel: returns the root
    // entity when `attached` is true, INVALID_ENTITY otherwise. `stsdPath` is looked up
    // in the mounted packs first and on disk second, so a map can be iterated as a
    // loose file during development and shipped inside a pack with no code change.
    //
    // The map's own resources are NOT embedded in the .stsd - they are in the pack, and
    // the engine pulls them through the installed override while deserialising. Loading
    // a .stsd with no pack mounted therefore gives geometry and no textures, which is
    // why this reports that case rather than half-loading in silence.
    wi::ecs::Entity LoadScene (wi::scene::Scene& scene,
                               const std::string& stsdPath,
                               const XMMATRIX& transform = XMMatrixIdentity(),
                               bool attached = false,
                               wi::scene::LoadModelProgressCallback progress = nullptr,
                               std::string* error = nullptr);

    // Read a map's metadata without touching its entity payload - name, source, the
    // asset list. A few KB of a file whose blob may be 30 MB.
    bool ReadSceneInfo (const std::string& stsdPath, asset::SceneDescriptor& out,
                        std::string* error = nullptr) const;

    // Which of a map's assets are missing from what is mounted. Empty means the map can
    // load complete. This is the check worth running at the top of a loading screen.
    std::vector<std::string> MissingAssetsFor (const asset::SceneDescriptor& scene) const;

    // Is there a .stsd at this path at all - in a mounted pack, or as a loose file?
    // The question a scene asks before deciding between the packed map and the .wiscene
    // it was converted from.
    bool CanLoadScene (const std::string& stsdPath) const;

    // load progress

    // Snapshot of what the packs are serving. Safe from any thread.
    AssetLoadProgress LoadProgress () const;

    // Fired once per asset served out of a pack.
    //
    // CALLED FROM MANY THREADS AT ONCE. The engine loads a scene's resources on
    // job-system workers while the main thread is blocked inside Scene::Serialize, so
    // this callback must not touch the scene, ImGui, or anything else that assumes the
    // main thread. SubWinStatus - the native loading window - is explicitly thread safe
    // and is what st::App routes this to.
    //
    // A plain function pointer rather than a std::function, matching
    // wi::helper::AssetSourceOverride: the project builds with exceptions and RTTI off,
    // and this is on a path that runs thousands of times per load.
    using LoadProgressCallback = void (*)(const AssetLoadProgress& progress, void* userdata);
    void SetLoadProgressCallback (LoadProgressCallback callback, void* userdata);

    // Bracket a load so the counters mean something. LoadScene() calls these itself;
    // call them by hand only around a load this class does not drive.
    //   expectedAssets: how many reads are coming, or 0 if unknown.
    void BeginLoadTracking (uint32_t expectedAssets);
    void EndLoadTracking ();

    // enumeration (DevUI)

    struct MountInfo {
        std::string path;
        std::string mountPoint;
        uint32_t    assetCount = 0;
        uint32_t    partCount  = 0;
        uint64_t    payloadSize = 0;
        uint64_t    buildTimestamp = 0;
    };
    MountInfo               MountAt   (uint32_t i) const;
    const asset::AssetPack* PackAt    (uint32_t i) const;
    std::string             MountPointAt (uint32_t i) const;

private:
    AssetSystem() = default;
    ~AssetSystem();
    AssetSystem(const AssetSystem&)            = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    struct MountEntry {
        std::unique_ptr<asset::AssetPack> pack;
        std::string mountPoint;   // canonical, with a trailing '/'
    };

    // The three callbacks handed to wi::helper::SetAssetSourceOverride. Static because
    // that struct takes plain C function pointers, and plain function pointers because
    // the engine is built with exceptions and RTTI off.
    static bool SourceRead   (const std::string& fileName, const uint8_t** data, size_t* size, void* ud);
    static void SourceFree   (const uint8_t* data, void* ud);
    static bool SourceExists (const std::string& fileName, void* ud);
    static bool SourceStat   (const std::string& fileName, uint64_t* size, uint64_t* timestamp, void* ud);

    // Resolve the name the engine asked for against one mount's prefix.
    const asset::StrdAsset* Resolve (const MountEntry& m, const std::string& fileName) const;

    // Reports one served asset to the callback, if any. Called on loading threads.
    void NoteAssetServed (const asset::AssetPack& pack, const asset::StrdAsset& asset);

    std::vector<MountEntry> mounts_;
    bool                    installed_ = false;

    // progress, written from loading threads
    // The counters are atomic so the hot path never takes a lock; only the name does,
    // and a short string assign under a mutex is nothing next to the read that
    // produced it.
    std::atomic<uint32_t> assetsRead_{0};
    std::atomic<uint32_t> assetsExpected_{0};
    std::atomic<uint64_t> bytesRead_{0};
    mutable std::mutex    progressMutex_;
    std::string           currentAsset_;
    uint64_t              currentSize_ = 0;
    LoadProgressCallback  progressCallback_ = nullptr;
    void*                 progressUserdata_ = nullptr;
};

} // namespace st
