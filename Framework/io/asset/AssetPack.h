#pragma once
// AssetPack - read side of a .strd index plus its .stafp<N> parts.
//
// Open() memory-maps the index and every part, then answers lookups straight out of
// the mapped bytes. Nothing is parsed and nothing is allocated per lookup: the file
// IS the hash table (see AssetFormat.h). A miss costs one probe, a hit costs one
// probe and one 80-byte struct read.
//
//   st::asset::AssetPack pack;
//   if (pack.Open("assets/content.strd", &err)) {
//       if (const StrdAsset* a = pack.Find("textures/wall_basecolor.dds")) {
//           std::vector<uint8_t> bytes;
//           pack.Read(*a, bytes);
//       }
//   }
//
// Thread safety: Open() and Close() are not thread safe. Everything else is const and
// safe to call from any number of threads at once, with no internal locking, which
// matters because wi::helper's asset-source override is called from every loading
// job at once. That is the reason every part is mapped up front rather than on first
// touch - a lazy map needs a lock on the read path, and mapping is address space, not
// memory: pages arrive from the page cache when they are touched and leave under
// pressure, so mapping a 40 GB pack costs 40 GB of a 128 TB address space and no RAM.
//
// No exceptions, no RTTI (the project builds with /EHsc- /GR-). Everything reports
// through a bool and an optional error string.

#include <cstdint>
#include <string>
#include <vector>

#include "AssetFormat.h"

namespace st::asset {

// A decoded, owning snapshot of one entry - for UI lists and tools that want a plain
// value. The read path uses the raw StrdAsset instead and copies nothing.
struct AssetInfo {
    uint64_t    id           = 0;
    std::string name;
    AssetType   type         = AssetType::Unknown;
    Codec       codec        = Codec::None;
    uint64_t    size         = 0;   // uncompressed
    uint64_t    storedSize   = 0;   // on disk
    uint64_t    contentHash  = 0;
    uint32_t    partIndex    = 0;   // index into the part table
    uint32_t    partNumber   = 0;   // the N in .stafp<N>
    uint32_t    flags        = 0;
};

struct PartInfo {
    uint32_t    number      = 0;
    std::string fileName;
    uint64_t    fileSize    = 0;
    uint64_t    fileHash    = 0;
    uint32_t    assetCount  = 0;
    uint32_t    flags       = 0;
    bool        mapped      = false;
};

class AssetPack {
public:
    AssetPack() = default;
    ~AssetPack();

    AssetPack(const AssetPack&)            = delete;
    AssetPack& operator=(const AssetPack&) = delete;
    AssetPack(AssetPack&&)                 = delete;
    AssetPack& operator=(AssetPack&&)      = delete;

    // Map <strdPath> and every part named in it. Parts are resolved next to the
    // index file. Fails if the index is malformed, if a part is missing, or if a
    // part's UUID or size disagrees with what the index recorded - a stale part is
    // caught here rather than as corrupt geometry three loads later.
    //
    // `verifyParts` additionally hashes every part end to end, which is a full
    // sequential read of the whole pack. Right for an installer or a "verify files"
    // menu item; wrong for startup.
    bool Open (const std::string& strdPath, std::string* error = nullptr, bool verifyParts = false);
    void Close ();
    bool IsOpen () const { return header_ != nullptr; }

    const std::string& Path () const { return path_; }
    uint64_t UuidLo () const { return header_ ? header_->packUuidLo : 0; }
    uint64_t UuidHi () const { return header_ ? header_->packUuidHi : 0; }

    // lookup
    // Both return nullptr when absent. The pointer is into the mapped index and stays
    // valid until Close().
    const StrdAsset* Find (uint64_t id) const;
    const StrdAsset* Find (const std::string& logicalPath) const;

    bool Contains (const std::string& logicalPath) const { return Find(logicalPath) != nullptr; }

    // The asset's logical path, straight out of the mapped name heap. The string_view
    // form copies nothing; it is valid until Close().
    const char* NameData   (const StrdAsset& a) const;
    std::string NameString (const StrdAsset& a) const;

    // enumeration (for the DevUI explorer and tools)
    uint32_t          AssetCount () const { return header_ ? header_->assetCount : 0; }
    const StrdAsset* AssetAt    (uint32_t i) const;   // sorted ascending by id
    AssetInfo         InfoAt     (uint32_t i) const;
    AssetInfo         InfoOf     (const StrdAsset& a) const;

    uint32_t          PartCount () const { return header_ ? header_->partCount : 0; }
    PartInfo          PartAt    (uint32_t i) const;

    uint64_t TotalPayloadSize () const { return header_ ? header_->totalPayloadSize : 0; }
    uint64_t BuildTimestamp   () const { return header_ ? header_->buildTimestamp : 0; }

    // reading

    // Whole asset, decompressed. `out` is resized to the original size.
    bool Read (const StrdAsset& a, std::vector<uint8_t>& out, std::string* error = nullptr) const;

    // A byte range of the DECOMPRESSED asset. This is the call that makes streaming
    // work: the engine asks for "the tail of this .dds starting at mip 4" and, for a
    // stored or chunk-compressed asset, only those bytes are touched. Reading past
    // the end clamps; reading entirely past it yields an empty buffer and true.
    bool ReadRange (const StrdAsset& a, uint64_t offset, uint64_t size,
                    std::vector<uint8_t>& out, std::string* error = nullptr) const;

    // Zero-copy pointer to the asset's bytes inside the mapped part. Returns nullptr
    // unless the asset is Codec::None, because anything else has to be decompressed
    // somewhere and this call promises not to allocate. The whole reason the packer
    // leaves already-compressed formats (dds/png/ogg/mp4) at Codec::None by default
    // is to keep this path available for them.
    const uint8_t* MappedData (const StrdAsset& a) const;

    // True when `p` points inside one of this pack's mapped part files. The runtime
    // asset source uses it to tell a zero-copy pointer it handed the engine from a
    // buffer it decompressed and now has to free - the release callback is given only
    // the pointer, with no way to say which of the two it is.
    bool OwnsMappedPointer (const void* p) const;

    // integrity

    // Re-hash one asset's bytes and compare against the index. Cheap per asset.
    bool VerifyAsset (const StrdAsset& a, std::string* error = nullptr) const;
    // Re-hash a whole part file, header included. One sequential read of that part.
    bool VerifyPart  (uint32_t partIndex, std::string* error = nullptr) const;
    // Every part, then the index itself. `progress` may be null; it is called with
    // (done, total) part counts.
    bool VerifyAll   (std::string* error = nullptr,
                      void (*progress)(uint32_t done, uint32_t total, void* ud) = nullptr,
                      void* userdata = nullptr) const;

private:
    struct Mapping {
        void*       handle = nullptr;   // OS-specific; file handle / mapping object
        void*       view   = nullptr;   // OS-specific; second handle on Windows
        const void* data   = nullptr;
        uint64_t    size   = 0;
    };

    static bool MapFile   (const std::string& path, Mapping& out, std::string* error);
    static void UnmapFile (Mapping& m);

    const StrdPart*  PartRecord (uint32_t partIndex) const;
    const uint8_t*    PartBytes  (uint32_t partIndex, uint64_t& sizeOut) const;

    // Decompress `storedSize` bytes at `src` into `out`, honouring the asset's codec.
    // `wantOffset`/`wantSize` are in ORIGINAL bytes; for ZstdChunked only the frames
    // that overlap the window are decoded.
    bool Decode (const StrdAsset& a, const uint8_t* src,
                 uint64_t wantOffset, uint64_t wantSize,
                 std::vector<uint8_t>& out, std::string* error) const;

    std::string          path_;
    Mapping              indexMap_;
    std::vector<Mapping> partMaps_;

    // All pointers into indexMap_.data - nothing here owns anything.
    const StrdHeader* header_  = nullptr;
    const uint32_t*    buckets_ = nullptr;
    const StrdAsset*  assets_  = nullptr;
    const StrdPart*   parts_   = nullptr;
    const char*        names_   = nullptr;
};

// helpers shared with the packer and the DevUI

// Classify by file extension. The packer uses this for its default type and codec,
// and the explorer uses it for grouping. Unknown extensions land on Binary, which is
// always safe: the type never changes how bytes are read.
AssetType     ClassifyByExtension (const std::string& path);
// The codec the packer picks when the caller does not override it. The path matters as
// well as the type: "Image" covers both .png, which is already deflate-compressed and
// gains nothing, and .bmp/.tga/.hdr, which are raw samples and halve or better.
Codec         DefaultCodecFor     (AssetType type, uint64_t size);
Codec         DefaultCodecFor     (const std::string& path, AssetType type, uint64_t size);
// True for a container that already carries its own compression, where a second pass
// costs a decompress on every read and buys a couple of percent.
bool          IsCompressedContainer (const std::string& path);
// Human-readable names, for UI and CLI output.
const char*   ToString (AssetType t);
const char*   ToString (Codec c);
// "12.4 MB" - used by the explorer and `stpack info`.
std::string   FormatBytes (uint64_t bytes);
// "content.stafp3" from ("content", 3).
std::string   PartFileName (const std::string& baseName, uint32_t partNumber);

} // namespace st::asset
