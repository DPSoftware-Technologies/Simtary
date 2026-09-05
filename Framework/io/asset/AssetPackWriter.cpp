#include "AssetPackWriter.h"
#include "StHash.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Utility/zstd/zstd.h"

namespace fs = std::filesystem;

namespace st::asset {

namespace {

void SetError (std::string* error, const std::string& text) {
    if (error) *error = text;
}

// std::filesystem::path built from a UTF-8 std::string. u8path is the C++17 spelling
// that says "these bytes are UTF-8"; the plain path constructor would take them as the
// active ANSI code page on Windows and mangle any non-ASCII asset name.
fs::path U8Path (const std::string& s) { return fs::u8path(s); }

uint64_t NowSeconds () {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Round up to the next kPartAlignment boundary.
uint64_t AlignUp (uint64_t v) {
    return (v + kPartAlignment - 1) & ~(kPartAlignment - 1);
}

bool WriteWholeFile (const std::string& path, const void* data, uint64_t size, std::string* error) {
    std::ofstream out(U8Path(path),
                      std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        SetError(error, "cannot write " + path);
        return false;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        SetError(error, "write failed for " + path);
        return false;
    }
    return true;
}

bool ReadWholeFile (const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    std::error_code ec;
    const auto size = fs::file_size(U8Path(path), ec);
    if (ec) {
        SetError(error, "cannot stat " + path);
        return false;
    }
    std::ifstream in(U8Path(path), std::ios::binary);
    if (!in.is_open()) {
        SetError(error, "cannot open " + path);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) {
        SetError(error, "read failed for " + path);
        return false;
    }
    return true;
}

std::string LowerExtNoDot (const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

} // namespace

// lifetime

AssetPackWriter::~AssetPackWriter () {
    if (open_) Abort();
}

bool AssetPackWriter::Begin (const std::string& outDir, const std::string& baseName,
                             const PackOptions& options, std::string* error) {
    Abort();

    outDir_   = outDir;
    baseName_ = baseName;
    options_  = options;

    if (baseName_.empty()) {
        SetError(error, "pack base name is empty");
        return false;
    }
    if (options_.partSizeTarget == 0 || options_.partSizeTarget > kMaxPartSize) {
        // Clamping rather than failing, because a caller asking for 0 or for 4 GB
        // meant "as big as you allow" far more often than it meant "stop the build".
        options_.partSizeTarget = std::min<uint64_t>(
            options_.partSizeTarget == 0 ? kDefaultPartSize : options_.partSizeTarget,
            kMaxPartSize);
    }
    if (options_.chunkSize == 0) options_.chunkSize = kDefaultChunkSize;
    if (options_.buildTimestamp == 0) options_.buildTimestamp = NowSeconds();

    if (options_.uuidLo == 0 && options_.uuidHi == 0) {
        // Derived, not random: reproducible from (name, timestamp), and different on
        // every rebuild, which is exactly what makes AssetPack::Open() able to reject
        // a part left over from the previous build.
        options_.uuidLo = Hash64(baseName_.data(), baseName_.size(), 0x5354414F44ull);
        options_.uuidHi = Hash64(&options_.buildTimestamp, sizeof(uint64_t), options_.uuidLo);
    }

    std::error_code ec;
    fs::create_directories(U8Path(outDir_), ec);
    if (ec) {
        SetError(error, "cannot create output directory " + outDir_);
        return false;
    }

    open_  = true;
    stats_ = PackStats{};
    return StartPart(error);
}

void AssetPackWriter::Abort () {
    if (!writtenFiles_.empty()) {
        std::error_code ec;
        for (const std::string& f : writtenFiles_)
            fs::remove(U8Path(f), ec);
    }
    assets_.clear();
    parts_.clear();
    writtenFiles_.clear();
    partBuffer_.clear();
    partNumber_     = 0;
    partAssetCount_ = 0;
    partOversized_  = false;
    open_           = false;
}

// parts

bool AssetPackWriter::StartPart (std::string* error) {
    (void)error;
    ++partNumber_;
    partAssetCount_ = 0;
    partOversized_  = false;

    partBuffer_.clear();
    // Reserve the header plus its alignment padding up front; both are filled in for
    // real by FlushPart(), once payloadSize and assetCount are known.
    partBuffer_.resize(static_cast<size_t>(kPartAlignment), 0);
    return true;
}

bool AssetPackWriter::FlushPart (std::string* error) {
    // Nothing but the reserved header means no asset ever landed here - do not emit an
    // empty part file, and do not burn a part number on it either.
    if (partAssetCount_ == 0) {
        --partNumber_;
        partBuffer_.clear();
        return true;
    }

    PendingPart part;
    part.number     = partNumber_;
    part.fileName   = PartFileName(baseName_, partNumber_);
    part.assetCount = partAssetCount_;
    part.flags      = partOversized_ ? PartFlag_Oversized : PartFlag_None;
    part.fileSize   = partBuffer_.size();

    StafpHeader head{};
    std::memcpy(head.magic, kStafpMagic, sizeof(head.magic));
    head.version       = kStafpVersion;
    head.partIndex     = partNumber_;
    head.packUuidLo    = options_.uuidLo;
    head.packUuidHi    = options_.uuidHi;
    head.payloadOffset = kPartAlignment;
    head.payloadSize   = partBuffer_.size() - kPartAlignment;
    head.assetCount    = partAssetCount_;
    head.headerHash    = 0;
    head.headerHash    = Hash64(&head, sizeof(head));   // over the header with the field zeroed

    std::memcpy(partBuffer_.data(), &head, sizeof(head));

    // Hash exactly what is about to be written, from the same buffer, so the recorded
    // file hash cannot drift from the file.
    {
        StafpHeader zeroed = head;
        zeroed.headerHash = 0;
        Hasher64 hasher;
        hasher.Update(&zeroed, sizeof(zeroed));
        hasher.Update(partBuffer_.data() + sizeof(StafpHeader),
                      partBuffer_.size() - sizeof(StafpHeader));
        part.fileHash = hasher.Digest();
    }

    const std::string path = outDir_ + "/" + part.fileName;
    if (!WriteWholeFile(path, partBuffer_.data(), partBuffer_.size(), error)) return false;
    writtenFiles_.push_back(path);

    parts_.push_back(part);
    partBuffer_.clear();
    return true;
}

// payload encoding

bool AssetPackWriter::EncodePayload (const uint8_t* src, uint64_t size, Codec& codec,
                                     uint32_t chunkSize, std::vector<uint8_t>& out,
                                     std::string* error) {
    out.clear();

    if (options_.forceStored) codec = Codec::None;

    // A frame's own overhead is a couple of dozen bytes; below this there is nothing
    // to win and a decompress call to lose.
    if (size < 512 && !options_.aggressive) codec = Codec::None;

    switch (codec) {

    case Codec::None:
        out.assign(src, src + size);
        return true;

    case Codec::Zstd: {
        const size_t bound = ZSTD_compressBound(static_cast<size_t>(size));
        out.resize(bound);
        const size_t got = ZSTD_compress(out.data(), out.size(), src,
                                         static_cast<size_t>(size), options_.compressionLevel);
        if (ZSTD_isError(got)) {
            SetError(error, std::string("zstd compress failed: ") + ZSTD_getErrorName(got));
            return false;
        }
        out.resize(got);
        break;
    }

    case Codec::ZstdChunked: {
        if (chunkSize == 0) chunkSize = options_.chunkSize;
        const uint32_t frameCount =
            static_cast<uint32_t>((size + chunkSize - 1) / chunkSize);

        ChunkTableHeader table{};
        table.chunkSize        = chunkSize;
        table.frameCount       = frameCount;
        table.uncompressedSize = size;

        const size_t tableBytes = sizeof(ChunkTableHeader) +
                                  size_t(frameCount + 1) * sizeof(uint64_t);
        out.resize(tableBytes);
        std::memcpy(out.data(), &table, sizeof(table));

        std::vector<uint64_t> frameOffsets(frameCount + 1, 0);
        std::vector<uint8_t>  frame;
        frame.resize(ZSTD_compressBound(chunkSize));

        for (uint32_t f = 0; f < frameCount; ++f) {
            frameOffsets[f] = out.size();
            const uint64_t begin = uint64_t(f) * chunkSize;
            const uint64_t len   = std::min<uint64_t>(chunkSize, size - begin);
            const size_t got = ZSTD_compress(frame.data(), frame.size(), src + begin,
                                             static_cast<size_t>(len), options_.compressionLevel);
            if (ZSTD_isError(got)) {
                SetError(error, std::string("zstd frame compress failed: ") + ZSTD_getErrorName(got));
                return false;
            }
            out.insert(out.end(), frame.begin(), frame.begin() + got);
        }
        frameOffsets[frameCount] = out.size();

        std::memcpy(out.data() + sizeof(ChunkTableHeader), frameOffsets.data(),
                    frameOffsets.size() * sizeof(uint64_t));
        break;
    }

    default:
        SetError(error, "unknown codec requested");
        return false;
    }

    // If the "compressed" form is within 3% of the original there is nothing to gain,
    // and storing it keeps AssetPack::MappedData() - the zero-copy path - available.
    if (out.size() >= static_cast<size_t>(double(size) * 0.97)) {
        codec = Codec::None;
        out.assign(src, src + size);
    }
    return true;
}

// adding

bool AssetPackWriter::Contains (const std::string& logicalPath) const {
    const uint64_t id = AssetIdFromPath(logicalPath);
    for (const PendingAsset& a : assets_)
        if (a.id == id) return true;
    return false;
}

bool AssetPackWriter::Add (const std::string& logicalPath, const uint8_t* data, uint64_t size,
                           AssetType type, Codec codec, uint32_t flags, std::string* error) {
    if (!open_) {
        SetError(error, "AssetPackWriter::Add called before Begin()");
        return false;
    }

    // Stored with its case intact, hashed lower cased. See StHash.h: the engine asks
    // for the exact string a material saved, so lower casing what goes in the name heap
    // would break both the reverse conversion and any case sensitive filesystem.
    const std::string stored = NormalizePath(logicalPath);
    if (stored.empty()) {
        SetError(error, "asset logical path is empty");
        return false;
    }
    const uint64_t id = AssetIdFromPath(stored);

    for (const PendingAsset& a : assets_) {
        if (a.id != id) continue;
        if (CanonicalPath(a.name) == CanonicalPath(stored)) {
            SetError(error, "duplicate asset " + stored);
        } else {
            // 64-bit collision between two different paths. Astronomically unlikely,
            // and silently shipping it would mean one asset shadowing another at
            // runtime with no way to tell - so it stops the build and names both.
            SetError(error, "asset ID collision between \"" + a.name + "\" and \"" +
                            stored + "\" — rename one of them");
        }
        return false;
    }

    if (type == AssetType::Unknown) type = ClassifyByExtension(stored);

    std::vector<uint8_t> payload;
    Codec chosen = codec;
    if (!EncodePayload(data, size, chosen, options_.chunkSize, payload, error)) return false;

    // Part placement. Start a new part when this asset would push the current one past
    // the target, unless the current part is still empty - in which case the asset is
    // simply bigger than the cap and gets a part to itself. Splitting an asset across
    // two files would cost a second seek on every read of it, which is the one thing
    // this format exists to avoid.
    const uint64_t aligned = AlignUp(payload.size());
    if (partAssetCount_ > 0 && partBuffer_.size() + aligned > options_.partSizeTarget) {
        if (!FlushPart(error)) return false;
        if (!StartPart(error)) return false;
    }
    if (payload.size() > options_.partSizeTarget) partOversized_ = true;

    PendingAsset entry;
    entry.id           = id;
    entry.name         = stored;
    entry.partIndex    = static_cast<uint32_t>(parts_.size());   // this part, once flushed
    entry.offset       = partBuffer_.size();
    entry.storedSize   = payload.size();
    entry.originalSize = size;
    entry.contentHash  = Hash64(data, static_cast<size_t>(size));
    entry.type         = type;
    entry.codec        = chosen;
    entry.flags        = flags;
    entry.chunkSize    = (chosen == Codec::ZstdChunked) ? options_.chunkSize : 0;

    // A stored asset can be handed to the engine as a mapped pointer and read in
    // ranges; a whole-frame zstd asset cannot. Marking it here is what lets the
    // runtime decide whether mip streaming is available without re-deriving it.
    if (chosen == Codec::None || chosen == Codec::ZstdChunked) entry.flags |= AssetFlag_Streamable;
    else                                                       entry.flags &= ~uint32_t(AssetFlag_Streamable);

    partBuffer_.insert(partBuffer_.end(), payload.begin(), payload.end());
    partBuffer_.resize(static_cast<size_t>(AlignUp(partBuffer_.size())), 0);
    ++partAssetCount_;

    assets_.push_back(std::move(entry));
    stats_.originalBytes += size;
    stats_.storedBytes   += payload.size();
    ++stats_.assetCount;
    return true;
}

bool AssetPackWriter::AddAuto (const std::string& logicalPath, const uint8_t* data, uint64_t size,
                               uint32_t flags, std::string* error) {
    const AssetType type  = ClassifyByExtension(logicalPath);
    const Codec     codec = DefaultCodecFor(logicalPath, type, size);
    return Add(logicalPath, data, size, type, codec, flags, error);
}

bool AssetPackWriter::AddFile (const std::string& logicalPath, const std::string& sourceFile,
                               uint32_t flags, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(sourceFile, bytes, error)) return false;
    return AddAuto(logicalPath, bytes.data(), bytes.size(), flags, error);
}

// index

bool AssetPackWriter::WriteIndex (std::string* error) {
    // Sorted by ID so the table is deterministic (two builds of the same content
    // produce the same bytes) and so a tool can binary-search it without the buckets.
    std::sort(assets_.begin(), assets_.end(),
              [](const PendingAsset& a, const PendingAsset& b) { return a.id < b.id; });

    // Name heap. Part names go in first so a reader that only wants the part list
    // touches one contiguous run of it.
    std::string nameHeap;
    std::vector<uint64_t> partNameOffsets(parts_.size());
    for (size_t i = 0; i < parts_.size(); ++i) {
        partNameOffsets[i] = nameHeap.size();
        nameHeap += parts_[i].fileName;
        nameHeap.push_back('\0');
    }
    std::vector<uint64_t> assetNameOffsets(assets_.size());
    for (size_t i = 0; i < assets_.size(); ++i) {
        assetNameOffsets[i] = nameHeap.size();
        nameHeap += assets_[i].name;
        nameHeap.push_back('\0');
    }

    // Bucket count: the next power of two at or above 2x the asset count, so the load
    // factor never exceeds 0.5 and linear probing stays short.
    uint32_t bucketCount = 8;
    while (bucketCount < assets_.size() * 2) bucketCount <<= 1;

    std::vector<uint32_t> buckets(bucketCount, kEmptyBucket);
    const uint32_t mask = bucketCount - 1;
    for (uint32_t i = 0; i < assets_.size(); ++i) {
        uint32_t slot = static_cast<uint32_t>(assets_[i].id) & mask;
        while (buckets[slot] != kEmptyBucket) slot = (slot + 1) & mask;
        buckets[slot] = i;
    }

    std::vector<StrdAsset> assetTable(assets_.size());
    for (size_t i = 0; i < assets_.size(); ++i) {
        const PendingAsset& s = assets_[i];
        StrdAsset& d = assetTable[i];
        d.id           = s.id;
        d.offset       = s.offset;
        d.storedSize   = s.storedSize;
        d.originalSize = s.originalSize;
        d.contentHash  = s.contentHash;
        d.nameOffset   = assetNameOffsets[i];
        d.nameLength   = static_cast<uint32_t>(s.name.size());
        d.partIndex    = s.partIndex;
        d.type         = static_cast<uint16_t>(s.type);
        d.codec        = static_cast<uint16_t>(s.codec);
        d.flags        = s.flags;
        d.chunkSize    = s.chunkSize;
        d.reserved[0] = d.reserved[1] = d.reserved[2] = 0;
    }

    std::vector<StrdPart> partTable(parts_.size());
    uint64_t totalPayload = 0;
    for (size_t i = 0; i < parts_.size(); ++i) {
        const PendingPart& s = parts_[i];
        StrdPart& d = partTable[i];
        d.index      = s.number;
        d.flags      = s.flags;
        d.fileSize   = s.fileSize;
        d.fileHash   = s.fileHash;
        d.nameOffset = partNameOffsets[i];
        d.nameLength = static_cast<uint32_t>(s.fileName.size());
        d.assetCount = s.assetCount;
        d.reserved[0] = d.reserved[1] = d.reserved[2] = 0;
        totalPayload += s.fileSize > kPartAlignment ? s.fileSize - kPartAlignment : 0;
    }

    StrdHeader head{};
    std::memcpy(head.magic, kStrdMagic, sizeof(head.magic));
    head.version     = kStrdVersion;
    head.flags       = PackFlag_Verified;
    head.packUuidLo  = options_.uuidLo;
    head.packUuidHi  = options_.uuidHi;
    head.partCount   = static_cast<uint32_t>(partTable.size());
    head.assetCount  = static_cast<uint32_t>(assetTable.size());
    head.bucketCount = bucketCount;
    head.nameHeapSize = static_cast<uint32_t>(nameHeap.size());

    uint64_t cursor = sizeof(StrdHeader);
    head.bucketTableOffset = cursor; cursor += uint64_t(bucketCount) * sizeof(uint32_t);
    head.assetTableOffset  = cursor; cursor += assetTable.size() * sizeof(StrdAsset);
    head.partTableOffset   = cursor; cursor += partTable.size()  * sizeof(StrdPart);
    head.nameHeapOffset    = cursor; cursor += nameHeap.size();

    head.totalPayloadSize = totalPayload;
    head.buildTimestamp   = options_.buildTimestamp;
    head.indexHash        = 0;
    head.reserved[0] = head.reserved[1] = head.reserved[2] = 0;

    std::vector<uint8_t> file(static_cast<size_t>(cursor), 0);
    auto blit = [&file](uint64_t at, const void* src, size_t n) {
        if (n) std::memcpy(file.data() + at, src, n);
    };
    blit(0,                      &head,             sizeof(head));
    blit(head.bucketTableOffset, buckets.data(),    buckets.size()    * sizeof(uint32_t));
    blit(head.assetTableOffset,  assetTable.data(), assetTable.size() * sizeof(StrdAsset));
    blit(head.partTableOffset,   partTable.data(),  partTable.size()  * sizeof(StrdPart));
    blit(head.nameHeapOffset,    nameHeap.data(),   nameHeap.size());

    // Hash everything past the header, then stamp it in. Reading it back is the first
    // thing AssetPack::Open() does, so a truncated or edited index is refused before
    // any offset inside it is trusted.
    const uint64_t tailHash = Hash64(file.data() + sizeof(StrdHeader),
                                     file.size() - sizeof(StrdHeader));
    std::memcpy(file.data() + offsetof(StrdHeader, indexHash), &tailHash, sizeof(tailHash));

    const std::string path = outDir_ + "/" + baseName_ + ".strd";
    if (!WriteWholeFile(path, file.data(), file.size(), error)) return false;
    writtenFiles_.push_back(path);

    stats_.partCount  = static_cast<uint32_t>(partTable.size());
    stats_.indexBytes = file.size();
    return true;
}

bool AssetPackWriter::Finish (std::string* error) {
    if (!open_) {
        SetError(error, "AssetPackWriter::Finish called before Begin()");
        return false;
    }
    if (!FlushPart(error)) { Abort(); return false; }
    if (!WriteIndex(error)) { Abort(); return false; }

    const PackStats stats = stats_;
    writtenFiles_.clear();   // keep them: the build succeeded
    Abort();
    stats_ = stats;
    return true;
}

// directory sweep

bool PackDirectory (const std::string& contentDir,
                    const std::string& outDir,
                    const std::string& baseName,
                    const PackOptions& options,
                    const std::vector<std::string>& skipExtensions,
                    PackStats* stats,
                    std::string* error) {
    const fs::path root = U8Path(contentDir);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        SetError(error, contentDir + " is not a directory");
        return false;
    }

    AssetPackWriter writer;
    if (!writer.Begin(outDir, baseName, options, error)) return false;

    // Collected and sorted before adding, so the pack is byte-identical between two
    // builds of the same tree - directory iteration order is not guaranteed, and an
    // index that shuffles every build defeats incremental patching.
    std::vector<std::string> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());

    for (const std::string& full : files) {
        const fs::path p = U8Path(full);
        const std::string ext = LowerExtNoDot(p);
        if (std::find(skipExtensions.begin(), skipExtensions.end(), ext) != skipExtensions.end())
            continue;

        const std::string logical = NormalizePath(fs::relative(p, root, ec).string());
        if (ec || logical.empty()) continue;

        if (!writer.AddFile(logical, full, AssetFlag_None, error)) return false;
    }

    if (!writer.Finish(error)) return false;
    if (stats) *stats = writer.Stats();
    return true;
}

} // namespace st::asset
