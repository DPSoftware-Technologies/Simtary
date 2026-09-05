#include "AssetSystem.h"
#include "StHash.h"

#include <algorithm>
#include <cstring>

#include "wiHelper.h"
#include "wiArchive.h"
#include "wiBacklog.h"

namespace st {

using namespace wi::ecs;
using namespace wi::scene;

namespace {

// Normalise a mount point to "" or "something/". Canonicalising here means the compare
// in Resolve() is a plain prefix test with no per-call string work.
std::string NormaliseMountPoint (const std::string& raw) {
    if (raw.empty()) return std::string();
    std::string m = asset::CanonicalPath(raw);
    if (!m.empty() && m.back() != '/') m.push_back('/');
    return m;
}

// Maps blob-inflate progress into a slice of the overall load bar. A function pointer
// with a context, not a lambda, because asset::ReadProgress has to stay usable from the
// build-time packer, which links none of the engine.
struct BlobDecodeContext {
    const wi::scene::LoadModelProgressCallback* progress = nullptr;
    float begin = 0.0f;
    float end   = 0.0f;
};

void ReportBlobDecode (uint64_t done, uint64_t total, void* userdata) {
    BlobDecodeContext* ctx = static_cast<BlobDecodeContext*>(userdata);
    if (ctx == nullptr || ctx->progress == nullptr || !*ctx->progress || total == 0) return;
    const float f = ctx->begin + (ctx->end - ctx->begin) *
                    (static_cast<float>(done) / static_cast<float>(total));
    (*ctx->progress)(f, "Decompressing map data (" + asset::FormatBytes(done) +
                        " of " + asset::FormatBytes(total) + ")");
}

} // namespace

AssetSystem& AssetSystem::Get () {
    static AssetSystem instance;
    return instance;
}

AssetSystem::~AssetSystem () {
    // Uninstall first: leaving the override pointing at a destroyed singleton during
    // static teardown turns any late engine file read into a use-after-free.
    Uninstall();
    mounts_.clear();
}

// mounting

bool AssetSystem::Mount (const std::string& strdPath, const std::string& mountPoint,
                         std::string* error, bool verify) {
    auto pack = std::make_unique<asset::AssetPack>();
    if (!pack->Open(strdPath, error, verify)) return false;

    MountEntry m;
    m.mountPoint = NormaliseMountPoint(mountPoint);
    m.pack       = std::move(pack);

    wi::backlog::post("AssetSystem: mounted " + strdPath + " (" +
                      std::to_string(m.pack->AssetCount()) + " assets, " +
                      std::to_string(m.pack->PartCount()) + " parts, " +
                      asset::FormatBytes(m.pack->TotalPayloadSize()) + ")");

    mounts_.push_back(std::move(m));
    return true;
}

void AssetSystem::Unmount (const std::string& strdPath) {
    for (size_t i = mounts_.size(); i-- > 0; ) {
        if (mounts_[i].pack && mounts_[i].pack->Path() == strdPath)
            mounts_.erase(mounts_.begin() + static_cast<ptrdiff_t>(i));
    }
}

void AssetSystem::UnmountAll () {
    mounts_.clear();
}

// engine hook

void AssetSystem::Install () {
    if (installed_) return;
    wi::helper::AssetSourceOverride source;
    source.file_read   = &AssetSystem::SourceRead;
    source.file_free   = &AssetSystem::SourceFree;
    source.file_exists = &AssetSystem::SourceExists;
    source.file_stat   = &AssetSystem::SourceStat;
    source.userdata    = this;
    wi::helper::SetAssetSourceOverride(source);
    installed_ = true;
}

void AssetSystem::Uninstall () {
    if (!installed_) return;
    wi::helper::SetAssetSourceOverride(wi::helper::AssetSourceOverride{});
    installed_ = false;
}

const asset::StrdAsset* AssetSystem::Resolve (const MountEntry& m, const std::string& fileName) const {
    if (m.pack == nullptr) return nullptr;

    const std::string canonical = asset::CanonicalPath(fileName);

    // Try the path as given first. Resources lifted out of a .wiscene keep the exact
    // relative names the engine stored ("textures/wall.dds"), so they hit here.
    if (const asset::StrdAsset* a = m.pack->Find(canonical)) return a;

    // Then with the mount point stripped: the running game asks for
    // "assets/scenes/s1map.stsd" because the build copies contents/ into <exe>/assets/,
    // while the pack stores it as "scenes/s1map.stsd".
    if (!m.mountPoint.empty() && canonical.size() > m.mountPoint.size() &&
        canonical.compare(0, m.mountPoint.size(), m.mountPoint) == 0) {
        return m.pack->Find(canonical.substr(m.mountPoint.size()));
    }
    return nullptr;
}

const asset::StrdAsset* AssetSystem::Find (const std::string& logicalPath,
                                            const asset::AssetPack** outPack) const {
    // Last mount first, so a patch pack shadows the base pack.
    for (size_t i = mounts_.size(); i-- > 0; ) {
        if (const asset::StrdAsset* a = Resolve(mounts_[i], logicalPath)) {
            if (outPack) *outPack = mounts_[i].pack.get();
            return a;
        }
    }
    if (outPack) *outPack = nullptr;
    return nullptr;
}

bool AssetSystem::Exists (const std::string& logicalPath) const {
    return Find(logicalPath) != nullptr;
}

bool AssetSystem::Read (const std::string& logicalPath, std::vector<uint8_t>& out,
                        std::string* error) const {
    const asset::AssetPack* pack = nullptr;
    const asset::StrdAsset* a = Find(logicalPath, &pack);
    if (a == nullptr) {
        if (error) *error = logicalPath + " is not in any mounted pack";
        return false;
    }
    return pack->Read(*a, out, error);
}

bool AssetSystem::SourceRead (const std::string& fileName, const uint8_t** data, size_t* size, void* ud) {
    AssetSystem* self = static_cast<AssetSystem*>(ud);
    if (self == nullptr) return false;

    const asset::AssetPack* pack = nullptr;
    const asset::StrdAsset* a = self->Find(fileName, &pack);
    if (a == nullptr) return false;   // decline; the engine falls back to the filesystem

    self->NoteAssetServed(*pack, *a);

    // Stored asset: hand back a pointer straight into the mapped part. No allocation,
    // no copy, and the engine's own offset/length slicing turns this into real
    // streaming - which is exactly why the packer leaves dds/png/ogg/mp4 uncompressed.
    if (const uint8_t* mapped = pack->MappedData(*a)) {
        *data = mapped;
        *size = static_cast<size_t>(a->originalSize);
        return true;
    }

    // Compressed asset: it has to be materialised somewhere. SourceFree gives the
    // buffer back.
    std::vector<uint8_t> bytes;
    std::string error;
    if (!pack->Read(*a, bytes, &error)) {
        wi::backlog::post("AssetSystem: " + fileName + ": " + error, wi::backlog::LogLevel::Error);
        return false;
    }

    uint8_t* owned = new (std::nothrow) uint8_t[bytes.size() ? bytes.size() : 1];
    if (owned == nullptr) return false;
    if (!bytes.empty()) std::memcpy(owned, bytes.data(), bytes.size());
    *data = owned;
    *size = bytes.size();
    return true;
}

void AssetSystem::SourceFree (const uint8_t* data, void* ud) {
    AssetSystem* self = static_cast<AssetSystem*>(ud);
    if (self == nullptr || data == nullptr) return;

    // The callback is handed a bare pointer with no way of saying which of the two
    // kinds it is, so ask the packs: anything inside a mapped part was never allocated.
    for (const MountEntry& m : self->mounts_) {
        if (m.pack && m.pack->OwnsMappedPointer(data)) return;
    }
    delete[] data;
}

bool AssetSystem::SourceExists (const std::string& fileName, void* ud) {
    AssetSystem* self = static_cast<AssetSystem*>(ud);
    return self != nullptr && self->Find(fileName) != nullptr;
}

bool AssetSystem::SourceStat (const std::string& fileName, uint64_t* size, uint64_t* timestamp, void* ud) {
    AssetSystem* self = static_cast<AssetSystem*>(ud);
    if (self == nullptr) return false;
    const asset::StrdAsset* a = self->Find(fileName);
    if (a == nullptr) return false;

    if (size) *size = a->originalSize;
    // Deliberately 0, not the pack's build time. A mounted package is immutable for the
    // life of the process, and wi::resourcemanager reloads a resource whenever the
    // timestamp it sees is NEWER than the one it cached - a value that never rises is
    // exactly the "this can never go stale" answer.
    if (timestamp) *timestamp = 0;
    return true;
}

// load progress

void AssetSystem::SetLoadProgressCallback (LoadProgressCallback callback, void* userdata) {
    std::lock_guard<std::mutex> lock(progressMutex_);
    progressCallback_ = callback;
    progressUserdata_ = userdata;
}

void AssetSystem::BeginLoadTracking (uint32_t expectedAssets) {
    assetsRead_.store(0, std::memory_order_relaxed);
    bytesRead_.store(0, std::memory_order_relaxed);
    assetsExpected_.store(expectedAssets, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(progressMutex_);
    currentAsset_.clear();
    currentSize_ = 0;
}

void AssetSystem::EndLoadTracking () {
    // The counters are left alone on purpose: after a load they are the record of what
    // that load actually touched, which is worth being able to read. Only the
    // "how many are still coming" expectation is cleared, so a later stray read cannot
    // be mistaken for part of a map load.
    assetsExpected_.store(0, std::memory_order_relaxed);
}

AssetLoadProgress AssetSystem::LoadProgress () const {
    AssetLoadProgress out;
    out.assetsRead     = assetsRead_.load(std::memory_order_relaxed);
    out.assetsExpected = assetsExpected_.load(std::memory_order_relaxed);
    out.bytesRead      = bytesRead_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(progressMutex_);
    out.currentAsset = currentAsset_;
    out.currentSize  = currentSize_;
    return out;
}

void AssetSystem::NoteAssetServed (const asset::AssetPack& pack, const asset::StrdAsset& asset) {
    // Runs on whichever loading worker served the read. Everything below is either
    // atomic or under progressMutex_.
    const uint32_t read = assetsRead_.fetch_add(1, std::memory_order_relaxed) + 1;
    bytesRead_.fetch_add(asset.originalSize, std::memory_order_relaxed);

    AssetLoadProgress snapshot;
    LoadProgressCallback callback = nullptr;
    void* userdata = nullptr;
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        currentAsset_ = pack.NameString(asset);
        currentSize_  = asset.originalSize;

        callback = progressCallback_;
        userdata = progressUserdata_;
        if (callback == nullptr) return;

        snapshot.assetsRead     = read;
        snapshot.assetsExpected = assetsExpected_.load(std::memory_order_relaxed);
        snapshot.bytesRead      = bytesRead_.load(std::memory_order_relaxed);
        snapshot.currentAsset   = currentAsset_;
        snapshot.currentSize    = currentSize_;
    }
    // Deliberately outside the lock: the callback ends up in SubWinStatus, which takes
    // a lock of its own, and holding two while several loading threads pile in here is
    // how a load screen turns into a deadlock.
    callback(snapshot, userdata);
}

// scenes

bool AssetSystem::ReadSceneInfo (const std::string& stsdPath, asset::SceneDescriptor& out,
                                 std::string* error) const {
    std::vector<uint8_t> bytes;
    if (Read(stsdPath, bytes, nullptr))
        return asset::ParseSceneDescriptor(bytes.data(), bytes.size(), out, false, error);
    return asset::ReadSceneDescriptor(stsdPath, out, false, error);
}

std::vector<std::string> AssetSystem::MissingAssetsFor (const asset::SceneDescriptor& scene) const {
    std::vector<std::string> missing;
    for (const asset::SceneAssetRef& ref : scene.assets) {
        bool found = false;
        for (size_t i = mounts_.size(); i-- > 0 && !found; ) {
            if (mounts_[i].pack && mounts_[i].pack->Find(ref.id) != nullptr) found = true;
        }
        if (!found) missing.push_back(ref.path);
    }
    return missing;
}

bool AssetSystem::CanLoadScene (const std::string& stsdPath) const {
    if (Exists(stsdPath)) return true;
    return wi::helper::FileExists(stsdPath);
}

Entity AssetSystem::LoadScene (Scene& scene, const std::string& stsdPath,
                               const XMMATRIX& transform, bool attached,
                               wi::scene::LoadModelProgressCallback progress,
                               std::string* error) {
    if (progress) progress(0.0f, "Reading scene descriptor");

    // Pack first, loose file second. A map can therefore be iterated as a file during
    // development and shipped inside a pack with no code change on either side.
    //
    // Inflating the entity blob is the first thing in this function that takes real
    // time - tens of megabytes of zstd - so it reports per compression frame into the
    // [0.02, 0.12] slice rather than leaving the bar parked on the line above.
    BlobDecodeContext decodeContext;
    decodeContext.progress = &progress;
    decodeContext.begin    = 0.02f;
    decodeContext.end      = 0.12f;

    asset::ReadProgress readProgress;
    readProgress.report   = &ReportBlobDecode;
    readProgress.userdata = &decodeContext;

    asset::SceneDescriptor descriptor;
    std::vector<uint8_t> bytes;
    bool ok = false;
    if (Read(stsdPath, bytes, nullptr)) {
        ok = asset::ParseSceneDescriptor(bytes.data(), bytes.size(), descriptor, true, error, readProgress);
    } else {
        ok = asset::ReadSceneDescriptor(stsdPath, descriptor, true, error, readProgress);
    }
    if (!ok) {
        wi::backlog::post("AssetSystem: cannot load " + stsdPath +
                          (error && !error->empty() ? ": " + *error : ""),
                          wi::backlog::LogLevel::Error);
        return INVALID_ENTITY;
    }

    const std::vector<uint8_t>* ecs = descriptor.EcsArchive();
    if (ecs == nullptr || ecs->empty()) {
        if (error) *error = stsdPath + " has no entity payload";
        wi::backlog::post("AssetSystem: " + *error, wi::backlog::LogLevel::Error);
        return INVALID_ENTITY;
    }

    // A map whose resources are not mounted still deserialises - it just comes up
    // untextured. Saying so here beats the alternative, which is a grey world and no
    // clue why.
    const std::vector<std::string> missing = MissingAssetsFor(descriptor);
    if (!missing.empty()) {
        wi::backlog::post("AssetSystem: " + stsdPath + " is missing " +
                          std::to_string(missing.size()) + " of " +
                          std::to_string(descriptor.assets.size()) +
                          " assets (first: " + missing.front() +
                          ") — is the content pack mounted?",
                          wi::backlog::LogLevel::Warning);
    }

    // The map's own reference list is what turns the asset counter into a real
    // "7 of 14" rather than a running total with no denominator.
    const uint32_t expectedAssets =
        static_cast<uint32_t>(descriptor.assets.size() - missing.size());
    BeginLoadTracking(expectedAssets);

    // From here this mirrors wi::scene::LoadModel2, with a memory archive instead of a
    // file one. The archive has no source directory, which is what keeps the resource
    // names inside it relative - and relative is exactly how the packer stored them.
    Entity rootEntity = attached ? CreateEntity() : INVALID_ENTITY;

    {
        wi::Archive archive(ecs->data(), ecs->size());
        if (!archive.IsOpen()) {
            if (error) *error = stsdPath + ": stored scene archive is not readable by this engine build";
            wi::backlog::post("AssetSystem: " + *error, wi::backlog::LogLevel::Error);
            EndLoadTracking();
            return INVALID_ENTITY;
        }

        // The engine reports which COMPONENT MANAGER it is on; this adds which ASSET is
        // in flight, which is the half that is actually slow. Both end up on one line:
        //   "Loading materials  -  7/14 assets: textures/wall_basecolor.png"
        //
        // This only fires when the engine calls back, so it does not cover the stretch
        // where the engine is waiting on resource jobs and reporting nothing. That
        // stretch is covered from the other side, by SetLoadProgressCallback firing on
        // the loading threads themselves - see st::App.
        wi::scene::LoadModelProgressCallback wrapped =
            [this, &progress, expectedAssets](float fraction, const std::string& status) {
                if (!progress) return;
                std::string text = status;
                const AssetLoadProgress p = LoadProgress();
                if (expectedAssets > 0 && p.assetsRead > 0) {
                    const uint32_t done = p.assetsRead < expectedAssets ? p.assetsRead : expectedAssets;
                    text += "  -  " + std::to_string(done) + "/" + std::to_string(expectedAssets) + " assets";
                    if (!p.currentAsset.empty()) text += ": " + p.currentAsset;
                }
                progress(0.12f + 0.83f * fraction, text);
            };
        scene.Serialize(archive, wrapped);
    }

    bool createdRoot = false;
    if (rootEntity == INVALID_ENTITY) {
        rootEntity  = CreateEntity();
        createdRoot = true;
    }
    scene.transforms.Create(rootEntity);
    scene.layers.Create(rootEntity).layerMask = ~0u;

    if (progress) {
        const AssetLoadProgress p = LoadProgress();
        progress(0.96f, "Applying transform (" + std::to_string(p.assetsRead) + " assets, " +
                        asset::FormatBytes(p.bytesRead) + " read)");
    }
    for (size_t i = 0; i < scene.transforms.GetCount(); ++i) {
        const Entity entity = scene.transforms.GetEntity(i);
        if (entity != rootEntity && !scene.hierarchy.Contains(entity))
            scene.Component_Attach(entity, rootEntity);
    }
    if (TransformComponent* rootTransform = scene.transforms.GetComponent(rootEntity))
        rootTransform->MatrixTransform(transform);
    scene.Update(0);

    if (createdRoot) {
        // Nothing asked for a handle to it, so flatten it back out - the same choice
        // LoadModel makes, and for the same reason: a stray root deepens every
        // hierarchy walk for the life of the scene.
        scene.Component_DetachChildren(rootEntity);
        scene.Entity_Remove(rootEntity);
        rootEntity = INVALID_ENTITY;
    }

    EndLoadTracking();
    if (progress) progress(1.0f, "Done");
    return rootEntity;
}

// enumeration

AssetSystem::MountInfo AssetSystem::MountAt (uint32_t i) const {
    MountInfo info;
    if (i >= mounts_.size() || mounts_[i].pack == nullptr) return info;
    const asset::AssetPack& p = *mounts_[i].pack;
    info.path           = p.Path();
    info.mountPoint     = mounts_[i].mountPoint;
    info.assetCount     = p.AssetCount();
    info.partCount      = p.PartCount();
    info.payloadSize    = p.TotalPayloadSize();
    info.buildTimestamp = p.BuildTimestamp();
    return info;
}

const asset::AssetPack* AssetSystem::PackAt (uint32_t i) const {
    if (i >= mounts_.size()) return nullptr;
    return mounts_[i].pack.get();
}

std::string AssetSystem::MountPointAt (uint32_t i) const {
    if (i >= mounts_.size()) return std::string();
    return mounts_[i].mountPoint;
}

} // namespace st
