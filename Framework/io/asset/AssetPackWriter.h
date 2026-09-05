#pragma once
// AssetPackWriter - build side of a .strd index and its .stafp<N> parts.
//
// Used by tools/stpack during the build. Nothing at runtime writes a pack: a shipped
// game reads packs and writes save data, and keeping the writer out of the runtime is
// what lets the reader be const and lock-free.
//
//   st::asset::AssetPackWriter w;
//   w.Begin("build/assets", "content", options, &err);
//   w.AddFile("textures/wall.dds", "assets/contents/textures/wall.dds", &err);
//   w.Add("scenes/s1map.stsd", stsdBytes.data(), stsdBytes.size(), AssetType::Scene, ...);
//   w.Finish(&err);
//
// A part is assembled in memory and written in one go. That caps peak memory at one
// part (100 MB worst case) and buys two things worth more than the memory: the part's
// file hash is computed from the same buffer that is written, so it can never
// disagree with what landed on disk, and a failed build never leaves a half-written
// part behind for the next run to trip over.
//
// No exceptions, no RTTI. Errors come back as false plus an optional message.

#include <cstdint>
#include <string>
#include <vector>

#include "AssetFormat.h"
#include "AssetPack.h"

namespace st::asset {

struct PackOptions {
    // Target bytes per part. The packer starts a new part once adding an asset would
    // push the current one past this. Clamped into (0, kMaxPartSize].
    uint64_t partSizeTarget = kDefaultPartSize;

    // Frame size for Codec::ZstdChunked.
    uint32_t chunkSize = kDefaultChunkSize;

    // zstd level. 9 matches what wi::Archive uses for a compressed .wiscene, so a
    // converted pack is not suddenly bigger or slower to build than what it replaced.
    int compressionLevel = 9;

    // Store everything verbatim, whatever the type says. The escape hatch for
    // profiling a load, and for a platform where the decompress is the bottleneck.
    bool forceStored = false;

    // Compress everything that is not already a compressed container, even the small
    // stuff. Off by default because a 200-byte zstd frame saves nothing and costs a
    // call.
    bool aggressive = false;

    // Pack identity. Leave both at 0 and Begin() derives them from the base name and
    // the build timestamp, which is what makes a rebuilt pack reject an old part.
    uint64_t uuidLo = 0;
    uint64_t uuidHi = 0;

    // Stamped into the index for `stpack info`. 0 = ask the clock.
    uint64_t buildTimestamp = 0;
};

struct PackStats {
    uint32_t assetCount   = 0;
    uint32_t partCount    = 0;
    uint64_t originalBytes = 0;   // sum of asset sizes before compression
    uint64_t storedBytes   = 0;   // sum of asset sizes on disk
    uint64_t indexBytes    = 0;   // size of the .strd
};

class AssetPackWriter {
public:
    AssetPackWriter() = default;
    ~AssetPackWriter();

    AssetPackWriter(const AssetPackWriter&)            = delete;
    AssetPackWriter& operator=(const AssetPackWriter&) = delete;

    // `outDir` is created if it does not exist. Output lands as
    // <outDir>/<baseName>.strd plus <outDir>/<baseName>.stafp1, .stafp2, ...
    bool Begin (const std::string& outDir, const std::string& baseName,
                const PackOptions& options, std::string* error = nullptr);

    // Add one asset from memory. `logicalPath` is what the game will ask for at
    // runtime - the same string it passes to wi::resourcemanager::Load today, e.g.
    // "textures/wall_basecolor.dds". It is canonicalised (lower case, forward
    // slashes) before hashing, so authoring on Windows and shipping from Linux agree.
    //
    // Pass AssetType::Unknown to classify by extension, and Codec count (any value
    // outside the enum) is not accepted - use AddAuto() to let the packer choose.
    bool Add (const std::string& logicalPath, const uint8_t* data, uint64_t size,
              AssetType type, Codec codec, uint32_t flags, std::string* error = nullptr);

    // Add with the packer's own type and codec choice. This is the normal call.
    bool AddAuto (const std::string& logicalPath, const uint8_t* data, uint64_t size,
                  uint32_t flags = AssetFlag_None, std::string* error = nullptr);

    // Read `sourceFile` off disk and add it under `logicalPath`.
    bool AddFile (const std::string& logicalPath, const std::string& sourceFile,
                  uint32_t flags = AssetFlag_None, std::string* error = nullptr);

    // Already added under this logical path?
    bool Contains (const std::string& logicalPath) const;

    // Close the last part and write the index. After this the writer is empty again.
    bool Finish (std::string* error = nullptr);

    // Delete anything written so far and reset. Called automatically when Begin() or
    // Finish() fails, so a failed build leaves no half-pack behind.
    void Abort ();

    const PackStats& Stats () const { return stats_; }
    uint64_t UuidLo () const { return options_.uuidLo; }
    uint64_t UuidHi () const { return options_.uuidHi; }

private:
    struct PendingAsset {
        uint64_t    id           = 0;
        std::string name;         // canonical logical path
        uint32_t    partIndex    = 0;
        uint64_t    offset       = 0;
        uint64_t    storedSize   = 0;
        uint64_t    originalSize = 0;
        uint64_t    contentHash  = 0;
        AssetType   type         = AssetType::Unknown;
        Codec       codec        = Codec::None;
        uint32_t    flags        = 0;
        uint32_t    chunkSize    = 0;
    };

    struct PendingPart {
        uint32_t    number     = 0;
        std::string fileName;
        uint64_t    fileSize   = 0;
        uint64_t    fileHash   = 0;
        uint32_t    assetCount = 0;
        uint32_t    flags      = 0;
    };

    // Compress `src` into `out` according to `codec`, or fall back to Codec::None and
    // report that back through `codec` when compression does not pay for itself.
    bool EncodePayload (const uint8_t* src, uint64_t size, Codec& codec,
                        uint32_t chunkSize, std::vector<uint8_t>& out, std::string* error);

    bool StartPart (std::string* error);
    bool FlushPart (std::string* error);   // write the buffered part to disk
    bool WriteIndex (std::string* error);

    std::string  outDir_;
    std::string  baseName_;
    PackOptions  options_;
    PackStats    stats_;
    bool         open_ = false;

    std::vector<PendingAsset> assets_;
    std::vector<PendingPart>  parts_;
    std::vector<std::string>  writtenFiles_;   // for Abort()

    // The part currently being assembled.
    std::vector<uint8_t> partBuffer_;
    uint32_t             partNumber_    = 0;
    uint32_t             partAssetCount_ = 0;
    bool                 partOversized_ = false;
};

// one-call convenience used by the CLI

// Recursively pack every file under `contentDir`, keeping the directory structure as
// the logical path. `skipExtensions` (lower case, no dot) are left out - the packer
// uses it to skip the .wiscene sources it has already converted.
bool PackDirectory (const std::string& contentDir,
                    const std::string& outDir,
                    const std::string& baseName,
                    const PackOptions& options,
                    const std::vector<std::string>& skipExtensions,
                    PackStats* stats,
                    std::string* error);

} // namespace st::asset
