#include "SceneDescriptor.h"
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

fs::path U8Path (const std::string& s) { return fs::u8path(s); }

uint64_t NowSeconds () {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t AlignUp (uint64_t v) {
    return (v + kPartAlignment - 1) & ~(kPartAlignment - 1);
}

// Every read out of an archive is bounds-checked. A .wiscene from a player's disk is
// untrusted input by the time it reaches a converter, and a length field read out of
// a truncated file is exactly the value that turns into a wild memcpy.
bool ReadU64 (const uint8_t* data, uint64_t size, uint64_t at, uint64_t& out) {
    if (at + sizeof(uint64_t) > size) return false;
    std::memcpy(&out, data + at, sizeof(uint64_t));
    return true;
}

void AppendU64 (std::vector<uint8_t>& out, uint64_t v) {
    const size_t at = out.size();
    out.resize(at + sizeof(uint64_t));
    std::memcpy(out.data() + at, &v, sizeof(uint64_t));
}

void PatchU64 (std::vector<uint8_t>& out, uint64_t at, uint64_t v) {
    std::memcpy(out.data() + at, &v, sizeof(uint64_t));
}

bool ReadWholeFile (const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    std::error_code ec;
    const auto size = fs::file_size(U8Path(path), ec);
    if (ec) { SetError(error, "cannot stat " + path); return false; }
    std::ifstream in(U8Path(path), std::ios::binary);
    if (!in.is_open()) { SetError(error, "cannot open " + path); return false; }
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) { SetError(error, "read failed for " + path); return false; }
    return true;
}

bool WriteWholeFile (const std::string& path, const void* data, uint64_t size, std::string* error) {
    std::ofstream out(U8Path(path), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { SetError(error, "cannot write " + path); return false; }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) { SetError(error, "write failed for " + path); return false; }
    return true;
}

// The archive prologue: how many bytes sit before wi::scene::Scene::Serialize's own
// first field, and whether the payload is zstd-compressed. See AssetFormat.h for why
// these three cases exist.
struct ArchivePrologue {
    uint64_t version      = 0;
    uint64_t headerSize   = 0;   // version [+ properties] only
    uint64_t thumbnailLen = 0;
    bool     compressed   = false;
    uint64_t dataStart    = 0;   // headerSize + thumbnailLen
};

bool ReadArchivePrologue (const uint8_t* data, uint64_t size, ArchivePrologue& out, std::string* error) {
    if (!ReadU64(data, size, 0, out.version)) {
        SetError(error, "file is too small to be a wi::Archive");
        return false;
    }
    if (out.version < kMinSupportedArchiveVersion) {
        SetError(error, "archive version " + std::to_string(out.version) +
                        " has no resource jump table; re-save the scene with a current"
                        " editor build (version " + std::to_string(kMinSupportedArchiveVersion) +
                        " or newer is required)");
        return false;
    }

    if (out.version >= 92) {
        uint64_t props = 0;
        if (!ReadU64(data, size, 8, props)) { SetError(error, "truncated archive header"); return false; }
        out.headerSize   = 16;
        out.thumbnailLen = props & 0xFFFFFFFFull;
        out.compressed   = ((props >> 32) & 1ull) != 0;
    } else if (out.version == 91) {
        // 91 wrote the thumbnail size as a bare size_t in the properties slot, with no
        // compression bit; the field layout only became a bitfield at 92.
        if (!ReadU64(data, size, 8, out.thumbnailLen)) { SetError(error, "truncated archive header"); return false; }
        out.headerSize = 16;
        out.compressed = false;
    } else {
        out.headerSize   = 8;    // version 90: version only, no properties word
        out.thumbnailLen = 0;
        out.compressed   = false;
    }

    out.dataStart = out.headerSize + out.thumbnailLen;
    if (out.dataStart > size) {
        SetError(error, "archive thumbnail runs past the end of the file");
        return false;
    }
    return true;
}

} // namespace

// ── .wiscene split ─────────────────────────────────────────────────────────────

bool SplitWiscene (const uint8_t* data, uint64_t size, WisceneSplit& out, std::string* error) {
    out = WisceneSplit{};

    ArchivePrologue pro;
    if (!ReadArchivePrologue(data, size, pro, error)) return false;

    out.archiveVersion = pro.version;
    out.wasCompressed  = pro.compressed;

    // A compressed archive is one zstd frame over everything past the header, so the
    // jump offsets inside it are offsets into the DECOMPRESSED stream and mean nothing
    // until it is inflated. Inflate once, clear the bit, and everything below works on
    // one representation.
    if (pro.compressed) {
        const uint64_t compressedSize = size - pro.dataStart;
        const unsigned long long plainSize =
            ZSTD_getFrameContentSize(data + pro.dataStart, static_cast<size_t>(compressedSize));
        if (plainSize == ZSTD_CONTENTSIZE_ERROR || plainSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            SetError(error, "compressed archive has no frame content size — cannot inflate it");
            return false;
        }

        out.owned.resize(static_cast<size_t>(pro.dataStart + plainSize));
        std::memcpy(out.owned.data(), data, static_cast<size_t>(pro.dataStart));

        const size_t got = ZSTD_decompress(out.owned.data() + pro.dataStart,
                                           static_cast<size_t>(plainSize),
                                           data + pro.dataStart,
                                           static_cast<size_t>(compressedSize));
        if (ZSTD_isError(got) || got != plainSize) {
            SetError(error, std::string("cannot inflate compressed archive: ") +
                            (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output"));
            return false;
        }

        // Clear the compressed bit; the stream this function hands on is plain.
        uint64_t props = 0;
        std::memcpy(&props, out.owned.data() + 8, sizeof(props));
        props &= ~(1ull << 32);
        std::memcpy(out.owned.data() + 8, &props, sizeof(props));
    } else {
        out.source     = data;
        out.sourceSize = size;
    }

    const uint8_t* bytes = out.Bytes();
    const uint64_t total = out.Size();

    // Scene::Serialize's own prologue: uint32_t reserved (widened to 8 bytes on the
    // wire, like every integer wi::Archive writes), then the two jump offsets.
    const uint64_t reservedAt   = pro.dataStart;
    const uint64_t jumpBeforeAt = reservedAt + 8;
    const uint64_t jumpAfterAt  = reservedAt + 16;
    const uint64_t ecsStart     = reservedAt + 24;

    uint64_t jumpBefore = 0, jumpAfter = 0;
    if (!ReadU64(bytes, total, jumpBeforeAt, jumpBefore) ||
        !ReadU64(bytes, total, jumpAfterAt,  jumpAfter)) {
        SetError(error, "truncated scene header (no jump table)");
        return false;
    }
    if (jumpBefore < ecsStart || jumpAfter < jumpBefore || jumpAfter > total) {
        SetError(error, "scene jump table points outside the file — the archive is corrupt"
                        " or is not a scene archive");
        return false;
    }

    // ── read the embedded resource block ──────────────────────────────────────
    // Layout, straight out of wi::resourcemanager::Serialize_WRITE:
    //   u64 count, then per entry u64 nameLen, name, u64 flags, u64 dataLen, data.
    uint64_t cursor = jumpBefore;
    uint64_t count  = 0;
    if (!ReadU64(bytes, total, cursor, count)) {
        SetError(error, "truncated resource block");
        return false;
    }
    cursor += 8;

    out.resources.reserve(static_cast<size_t>(count < 100000 ? count : 0));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t nameLen = 0, flags = 0, dataLen = 0;
        if (!ReadU64(bytes, total, cursor, nameLen)) { SetError(error, "truncated resource name length"); return false; }
        cursor += 8;
        if (nameLen > total - cursor) { SetError(error, "resource name runs past the end of the file"); return false; }

        EmbeddedResource res;
        res.name.assign(reinterpret_cast<const char*>(bytes + cursor), static_cast<size_t>(nameLen));
        cursor += nameLen;

        if (!ReadU64(bytes, total, cursor, flags)) { SetError(error, "truncated resource flags"); return false; }
        cursor += 8;
        if (!ReadU64(bytes, total, cursor, dataLen)) { SetError(error, "truncated resource data length"); return false; }
        cursor += 8;
        if (dataLen > total - cursor) { SetError(error, "resource data runs past the end of the file"); return false; }

        res.engineFlags = static_cast<uint32_t>(flags);
        res.offset      = cursor;
        res.size        = dataLen;
        cursor += dataLen;

        out.resources.push_back(std::move(res));
    }

    if (cursor != jumpAfter) {
        // Not fatal — a future engine version could append to the block — but it means
        // this build did not understand all of it, and silently dropping the tail would
        // lose data on the way back out.
        SetError(error, "resource block ends at " + std::to_string(cursor) +
                        " but the jump table says " + std::to_string(jumpAfter) +
                        " — this archive has content this build does not understand");
        return false;
    }

    // ── build the resource-free archive ───────────────────────────────────────
    // Everything up to the resource block is copied verbatim, so the entity bytes are
    // never re-encoded and never reinterpreted. Only the second jump moves, because
    // the block it points past is now 8 bytes long instead of megabytes.
    out.ecsArchive.resize(static_cast<size_t>(jumpBefore));
    std::memcpy(out.ecsArchive.data(), bytes, static_cast<size_t>(jumpBefore));
    AppendU64(out.ecsArchive, 0);                              // empty resource block
    PatchU64(out.ecsArchive, jumpAfterAt, jumpBefore + 8);     // new end of that block

    return true;
}

bool MergeWiscene (const uint8_t* ecsArchive, uint64_t ecsSize,
                   const std::vector<ResourceToEmbed>& resources,
                   std::vector<uint8_t>& out, std::string* error) {
    out.clear();

    ArchivePrologue pro;
    if (!ReadArchivePrologue(ecsArchive, ecsSize, pro, error)) return false;
    if (pro.compressed) {
        SetError(error, "the stored scene archive is compressed, which it never should be"
                        " — .stsd applies its own codec to the whole blob");
        return false;
    }

    const uint64_t jumpAfterAt = pro.dataStart + 16;
    uint64_t jumpBefore = 0;
    if (!ReadU64(ecsArchive, ecsSize, pro.dataStart + 8, jumpBefore)) {
        SetError(error, "stored scene archive has no jump table");
        return false;
    }
    if (jumpBefore > ecsSize) {
        SetError(error, "stored scene archive jump table is out of range");
        return false;
    }

    out.resize(static_cast<size_t>(jumpBefore));
    std::memcpy(out.data(), ecsArchive, static_cast<size_t>(jumpBefore));

    AppendU64(out, resources.size());
    for (const ResourceToEmbed& r : resources) {
        AppendU64(out, r.name.size());
        out.insert(out.end(), r.name.begin(), r.name.end());
        AppendU64(out, r.engineFlags);
        AppendU64(out, r.size);
        if (r.size) {
            if (r.data == nullptr) {
                SetError(error, "resource \"" + r.name + "\" has a size but no data");
                return false;
            }
            out.insert(out.end(), r.data, r.data + r.size);
        }
    }

    PatchU64(out, jumpAfterAt, out.size());
    return true;
}

// ── .stsd ──────────────────────────────────────────────────────────────────────

const std::vector<uint8_t>* SceneDescriptor::EcsArchive () const {
    for (const SceneBlob& b : blobs)
        if (b.kind == StsdBlobKind::EcsArchive) return &b.data;
    return nullptr;
}

namespace {

// Shared with AssetPackWriter's encoder in intent but not in code: this one always
// produces a self-contained buffer for one blob and reports the codec it settled on.
bool EncodeBlob (const std::vector<uint8_t>& src, Codec& codec, uint32_t chunkSize,
                 int level, std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    if (src.empty()) { codec = Codec::None; return true; }

    switch (codec) {
    case Codec::None:
        out = src;
        return true;

    case Codec::Zstd: {
        out.resize(ZSTD_compressBound(src.size()));
        const size_t got = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), level);
        if (ZSTD_isError(got)) {
            SetError(error, std::string("zstd compress failed: ") + ZSTD_getErrorName(got));
            return false;
        }
        out.resize(got);
        break;
    }

    case Codec::ZstdChunked: {
        if (chunkSize == 0) chunkSize = kDefaultChunkSize;
        const uint32_t frameCount = static_cast<uint32_t>((src.size() + chunkSize - 1) / chunkSize);

        ChunkTableHeader table{};
        table.chunkSize        = chunkSize;
        table.frameCount       = frameCount;
        table.uncompressedSize = src.size();

        out.resize(sizeof(ChunkTableHeader) + size_t(frameCount + 1) * sizeof(uint64_t));
        std::memcpy(out.data(), &table, sizeof(table));

        std::vector<uint64_t> frameOffsets(frameCount + 1, 0);
        std::vector<uint8_t>  frame(ZSTD_compressBound(chunkSize));
        for (uint32_t f = 0; f < frameCount; ++f) {
            frameOffsets[f] = out.size();
            const uint64_t begin = uint64_t(f) * chunkSize;
            const uint64_t len   = std::min<uint64_t>(chunkSize, src.size() - begin);
            const size_t got = ZSTD_compress(frame.data(), frame.size(), src.data() + begin,
                                             static_cast<size_t>(len), level);
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
        SetError(error, "unknown blob codec");
        return false;
    }

    if (out.size() >= static_cast<size_t>(double(src.size()) * 0.97)) {
        codec = Codec::None;
        out = src;
    }
    return true;
}

bool DecodeBlob (const uint8_t* src, const StsdBlob& meta, std::vector<uint8_t>& out,
                 std::string* error, const ReadProgress& progress) {
    out.clear();
    switch (static_cast<Codec>(meta.codec)) {
    case Codec::None:
        out.assign(src, src + meta.storedSize);
        if (progress.report) progress.report(meta.originalSize, meta.originalSize, progress.userdata);
        return true;

    case Codec::Zstd: {
        out.resize(static_cast<size_t>(meta.originalSize));
        const size_t got = ZSTD_decompress(out.data(), out.size(), src,
                                           static_cast<size_t>(meta.storedSize));
        if (ZSTD_isError(got) || got != meta.originalSize) {
            SetError(error, std::string("blob decompress failed: ") +
                            (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output"));
            return false;
        }
        // One frame, so there is nothing to report part-way through — only the end.
        if (progress.report) progress.report(meta.originalSize, meta.originalSize, progress.userdata);
        return true;
    }

    case Codec::ZstdChunked: {
        if (meta.storedSize < sizeof(ChunkTableHeader)) {
            SetError(error, "chunked blob is missing its frame table");
            return false;
        }
        ChunkTableHeader table{};
        std::memcpy(&table, src, sizeof(table));
        const uint64_t tableBytes = sizeof(ChunkTableHeader) +
                                    uint64_t(table.frameCount + 1) * sizeof(uint64_t);
        if (table.chunkSize == 0 || tableBytes > meta.storedSize ||
            table.uncompressedSize != meta.originalSize) {
            SetError(error, "chunked blob has an inconsistent frame table");
            return false;
        }
        const uint64_t* frameOffsets =
            reinterpret_cast<const uint64_t*>(src + sizeof(ChunkTableHeader));

        out.resize(static_cast<size_t>(meta.originalSize));
        for (uint32_t f = 0; f < table.frameCount; ++f) {
            const uint64_t begin = frameOffsets[f];
            const uint64_t end   = frameOffsets[f + 1];
            if (end < begin || end > meta.storedSize) {
                SetError(error, "chunked blob frame is out of range");
                return false;
            }
            const uint64_t origin = uint64_t(f) * table.chunkSize;
            const uint64_t len    = std::min<uint64_t>(table.chunkSize, meta.originalSize - origin);
            const size_t got = ZSTD_decompress(out.data() + origin, static_cast<size_t>(len),
                                               src + begin, static_cast<size_t>(end - begin));
            if (ZSTD_isError(got) || got != len) {
                SetError(error, std::string("chunked blob frame decompress failed: ") +
                                (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output"));
                return false;
            }
            // Per frame, so a 36 MB map reports ~140 times instead of once — enough for a
            // bar that visibly moves, cheap enough to be free next to the inflate itself.
            if (progress.report) progress.report(origin + len, meta.originalSize, progress.userdata);
        }
        return true;
    }

    default:
        SetError(error, "blob uses a codec this build does not know");
        return false;
    }
}

nbt::Tag BuildMetadataTag (const SceneDescriptor& scene) {
    nbt::Tag root = nbt::Tag::Compound();
    root.putInt   ("format",         static_cast<int32_t>(kStsdVersion));
    root.putString("name",           scene.name);
    root.putString("source",         scene.sourceFile);
    root.putLong  ("archiveVersion", static_cast<int64_t>(scene.archiveVersion));
    root.putLong  ("built",          static_cast<int64_t>(scene.buildTimestamp));
    root.putLong  ("packUuidLo",     static_cast<int64_t>(scene.packUuidLo));
    root.putLong  ("packUuidHi",     static_cast<int64_t>(scene.packUuidHi));

    nbt::Tag assets = nbt::Tag::List(nbt::Type::Compound);
    for (const SceneAssetRef& a : scene.assets) {
        nbt::Tag entry = nbt::Tag::Compound();
        entry.putLong  ("id",    static_cast<int64_t>(a.id));
        entry.putString("path",  a.path);
        entry.putInt   ("flags",       static_cast<int32_t>(a.flags));
        entry.putInt   ("engineFlags", static_cast<int32_t>(a.engineFlags));
        assets.add(std::move(entry));
    }
    root.put("assets", std::move(assets));
    root.put("project", scene.project);
    return root;
}

void ReadMetadataTag (const nbt::Tag& root, SceneDescriptor& scene) {
    scene.name           = root.getString("name");
    scene.sourceFile     = root.getString("source");
    scene.archiveVersion = static_cast<uint64_t>(root.getLong("archiveVersion"));
    scene.buildTimestamp = static_cast<uint64_t>(root.getLong("built"));
    scene.packUuidLo     = static_cast<uint64_t>(root.getLong("packUuidLo"));
    scene.packUuidHi     = static_cast<uint64_t>(root.getLong("packUuidHi"));

    scene.assets.clear();
    if (const nbt::Tag* list = root.get("assets")) {
        for (const nbt::Tag& e : list->items) {
            SceneAssetRef ref;
            ref.id    = static_cast<uint64_t>(e.getLong("id"));
            ref.path  = e.getString("path");
            ref.flags       = static_cast<uint32_t>(e.getInt("flags"));
            ref.engineFlags = static_cast<uint32_t>(e.getInt("engineFlags"));
            scene.assets.push_back(std::move(ref));
        }
    }
    if (const nbt::Tag* p = root.get("project")) scene.project = *p;
}

} // namespace

bool SerializeSceneDescriptor (const SceneDescriptor& scene, std::vector<uint8_t>& fileOut,
                               const SceneWriteOptions& options, std::string* error) {
    // Metadata first: it has to be sized before the blob offsets can be computed, and
    // it is small enough that building it twice would still be free.
    std::vector<uint8_t> nbtBytes;
    if (!nbt::write(BuildMetadataTag(scene), "stsd", nbtBytes)) {
        SetError(error, "cannot serialise scene metadata");
        return false;
    }

    std::vector<std::vector<uint8_t>> encoded(scene.blobs.size());
    std::vector<StsdBlob>             table(scene.blobs.size());
    for (size_t i = 0; i < scene.blobs.size(); ++i) {
        Codec codec = scene.blobs[i].codec != Codec::None ? scene.blobs[i].codec : options.blobCodec;
        if (!EncodeBlob(scene.blobs[i].data, codec, options.chunkSize,
                        options.compressionLevel, encoded[i], error)) return false;
        table[i].storedSize   = encoded[i].size();
        table[i].originalSize = scene.blobs[i].data.size();
        table[i].contentHash  = Hash64(scene.blobs[i].data.data(), scene.blobs[i].data.size());
        table[i].kind         = static_cast<uint32_t>(scene.blobs[i].kind);
        table[i].codec        = static_cast<uint16_t>(codec);
        table[i].reserved0    = 0;
        table[i].chunkSize    = (codec == Codec::ZstdChunked) ? options.chunkSize : 0;
        table[i].reserved1    = 0;
    }

    StsdHeader head{};
    std::memcpy(head.magic, kStsdMagic, sizeof(head.magic));
    head.version       = kStsdVersion;
    head.flags         = 0;
    head.packUuidLo    = scene.packUuidLo;
    head.packUuidHi    = scene.packUuidHi;
    head.blobCount     = static_cast<uint32_t>(table.size());
    head.reserved0     = 0;
    head.assetRefCount = scene.assets.size();
    head.reserved1[0]  = head.reserved1[1] = 0;

    uint64_t cursor = sizeof(StsdHeader);
    head.nbtOffset       = cursor;
    head.nbtSize         = nbtBytes.size();
    cursor += nbtBytes.size();
    head.blobTableOffset = cursor; cursor += table.size() * sizeof(StsdBlob);

    // Blobs are page-aligned so the ECS archive can be handed to the engine straight
    // out of a mapped view without a bounce buffer, the same way a pack part is.
    for (size_t i = 0; i < table.size(); ++i) {
        cursor = AlignUp(cursor);
        table[i].offset = cursor;
        cursor += table[i].storedSize;
    }

    std::vector<uint8_t> file(static_cast<size_t>(cursor), 0);
    std::memcpy(file.data(), &head, sizeof(head));
    if (!nbtBytes.empty())
        std::memcpy(file.data() + head.nbtOffset, nbtBytes.data(), nbtBytes.size());
    if (!table.empty())
        std::memcpy(file.data() + head.blobTableOffset, table.data(), table.size() * sizeof(StsdBlob));
    for (size_t i = 0; i < table.size(); ++i) {
        if (!encoded[i].empty())
            std::memcpy(file.data() + table[i].offset, encoded[i].data(), encoded[i].size());
    }

    // The hash covers everything past the header, so it can be stamped into the header
    // afterwards without invalidating itself.
    reinterpret_cast<StsdHeader*>(file.data())->fileHash =
        Hash64(file.data() + sizeof(StsdHeader), file.size() - sizeof(StsdHeader));

    fileOut = std::move(file);
    return true;
}

bool WriteSceneDescriptor (const std::string& path, const SceneDescriptor& scene,
                           const SceneWriteOptions& options, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!SerializeSceneDescriptor(scene, bytes, options, error)) return false;
    return WriteWholeFile(path, bytes.data(), bytes.size(), error);
}

bool ParseSceneDescriptor (const uint8_t* data, uint64_t size, SceneDescriptor& out,
                           bool loadBlobs, std::string* error, const ReadProgress& progress) {
    out = SceneDescriptor{};

    if (size < sizeof(StsdHeader)) {
        SetError(error, "too small to be a .stsd");
        return false;
    }
    StsdHeader head{};
    std::memcpy(&head, data, sizeof(head));

    if (std::memcmp(head.magic, kStsdMagic, sizeof(head.magic)) != 0) {
        SetError(error, "not a .stsd (bad magic)");
        return false;
    }
    if (head.version != kStsdVersion) {
        SetError(error, ".stsd version " + std::to_string(head.version) +
                        ", this build reads version " + std::to_string(kStsdVersion));
        return false;
    }

    auto fits = [size](uint64_t off, uint64_t bytes) { return off <= size && bytes <= size - off; };
    if (!fits(head.nbtOffset, head.nbtSize) ||
        !fits(head.blobTableOffset, uint64_t(head.blobCount) * sizeof(StsdBlob))) {
        SetError(error, ".stsd tables run past the end of the file");
        return false;
    }

    const uint64_t tailHash = Hash64(data + sizeof(StsdHeader),
                                     static_cast<size_t>(size - sizeof(StsdHeader)));
    if (tailHash != head.fileHash) {
        SetError(error, ".stsd content hash mismatch (the file is corrupt)");
        return false;
    }

    out.packUuidLo = head.packUuidLo;
    out.packUuidHi = head.packUuidHi;

    nbt::Tag root;
    std::string nbtError;
    if (head.nbtSize > 0 &&
        !nbt::read(data + head.nbtOffset, static_cast<size_t>(head.nbtSize), root, nullptr, &nbtError)) {
        SetError(error, ".stsd metadata is unreadable: " + nbtError);
        return false;
    }
    ReadMetadataTag(root, out);

    const StsdBlob* table = reinterpret_cast<const StsdBlob*>(data + head.blobTableOffset);
    out.blobs.resize(head.blobCount);
    for (uint32_t i = 0; i < head.blobCount; ++i) {
        out.blobs[i].kind  = static_cast<StsdBlobKind>(table[i].kind);
        out.blobs[i].codec = static_cast<Codec>(table[i].codec);
        if (!loadBlobs) continue;

        if (!fits(table[i].offset, table[i].storedSize)) {
            SetError(error, ".stsd blob " + std::to_string(i) + " runs past the end of the file");
            return false;
        }
        if (!DecodeBlob(data + table[i].offset, table[i], out.blobs[i].data, error, progress)) return false;

        const uint64_t got = Hash64(out.blobs[i].data.data(), out.blobs[i].data.size());
        if (got != table[i].contentHash) {
            SetError(error, ".stsd blob " + std::to_string(i) + " failed its content hash");
            return false;
        }
    }
    return true;
}

bool ReadSceneDescriptor (const std::string& path, SceneDescriptor& out,
                          bool loadBlobs, std::string* error, const ReadProgress& progress) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(path, bytes, error)) return false;
    if (!ParseSceneDescriptor(bytes.data(), bytes.size(), out, loadBlobs, error, progress)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

// ── conversion ─────────────────────────────────────────────────────────────────

bool BuildSceneDescriptor (const std::string& wiscenePath,
                           const std::string& resourcePrefix,
                           AssetPackWriter*   writer,
                           SceneDescriptor&   out,
                           std::string* error) {
    out = SceneDescriptor{};

    std::vector<uint8_t> source;
    if (!ReadWholeFile(wiscenePath, source, error)) return false;

    WisceneSplit split;
    if (!SplitWiscene(source.data(), source.size(), split, error)) {
        if (error) *error = wiscenePath + ": " + *error;
        return false;
    }

    out.name           = U8Path(wiscenePath).stem().string();
    out.sourceFile     = NormalizePath(U8Path(wiscenePath).filename().string());
    out.archiveVersion = split.archiveVersion;
    out.buildTimestamp = NowSeconds();
    if (writer) {
        out.packUuidLo = writer->UuidLo();
        out.packUuidHi = writer->UuidHi();
    }

    SceneBlob ecs;
    ecs.kind  = StsdBlobKind::EcsArchive;
    ecs.codec = Codec::ZstdChunked;
    ecs.data  = std::move(split.ecsArchive);
    out.blobs.push_back(std::move(ecs));

    // Every resource the map embedded becomes a pack asset under `resourcePrefix`. The
    // engine asks for these by the exact relative path it stored, which is why the
    // runtime mounts the pack so that path resolves — see AssetSystem::Resolve.
    for (const EmbeddedResource& r : split.resources) {
        const std::string logical = NormalizePath(resourcePrefix + r.name);

        SceneAssetRef ref;
        ref.id          = AssetIdFromPath(logical);
        ref.path        = logical;
        ref.flags       = AssetFlag_FromScene;
        ref.engineFlags = r.engineFlags;
        out.assets.push_back(ref);

        if (writer == nullptr) continue;

        // A texture two maps share is added once. This is the whole size win of the
        // conversion: today each .wiscene carries its own private copy.
        if (writer->Contains(logical)) continue;

        if (!writer->AddAuto(logical, split.Bytes() + r.offset, r.size,
                             AssetFlag_FromScene, error)) {
            if (error) *error = wiscenePath + ": " + *error;
            return false;
        }
    }
    return true;
}

bool ConvertWiscene (const std::string& wiscenePath,
                     const std::string& outDir,
                     const std::string& resourcePrefix,
                     AssetPackWriter*   writer,
                     const SceneWriteOptions& options,
                     std::string* error) {
    SceneDescriptor scene;
    if (!BuildSceneDescriptor(wiscenePath, resourcePrefix, writer, scene, error)) return false;
    const std::string outPath = outDir + "/" + scene.name + ".stsd";
    return WriteSceneDescriptor(outPath, scene, options, error);
}

bool RebuildWiscene (const SceneDescriptor& scene, const AssetPack* pack,
                     std::vector<uint8_t>& out, std::string* error) {
    const std::vector<uint8_t>* ecs = scene.EcsArchive();
    if (ecs == nullptr || ecs->empty()) {
        SetError(error, "scene descriptor has no entity payload");
        return false;
    }

    // The resource bytes have to outlive MergeWiscene, which takes pointers, so they
    // are all materialised first. Peak cost is the same as the .wiscene being rebuilt,
    // which is what the caller is about to write anyway.
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ResourceToEmbed>      embed;
    storage.reserve(scene.assets.size());
    embed.reserve(scene.assets.size());

    for (const SceneAssetRef& ref : scene.assets) {
        if (pack == nullptr) continue;
        const StrdAsset* a = pack->Find(ref.id);
        if (a == nullptr) {
            SetError(error, "asset \"" + ref.path + "\" is not in the pack — rebuilding this"
                            " scene needs the pack it was converted with");
            return false;
        }
        storage.emplace_back();
        if (!pack->Read(*a, storage.back(), error)) return false;

        ResourceToEmbed e;
        e.name        = ref.path;
        e.engineFlags = ref.engineFlags;
        e.data        = storage.back().data();
        e.size        = storage.back().size();
        embed.push_back(e);
    }

    return MergeWiscene(ecs->data(), ecs->size(), embed, out, error);
}

} // namespace st::asset
