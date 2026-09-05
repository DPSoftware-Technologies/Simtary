#include "AssetPack.h"
#include "StHash.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

// zstd comes from the engine's vendored amalgamation. In a game build the symbols are
// already in the Utility static library that Simtary links; the standalone packer
// compiles zstd.c into itself and puts Engine/ on the include path, so this one line
// resolves in both.
#include "Utility/zstd/zstd.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    // Without NOMINMAX, windows.h defines min/max as macros and every std::min in this
    // file becomes a syntax error at the "::".
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace st::asset {

namespace {

void SetError (std::string* error, const std::string& text) {
    if (error) *error = text;
}

std::string DirectoryOf (const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash + 1);
}

std::string LowerExtension (const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && dot < slash) return std::string();
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return ext;
}

bool MagicMatches (const char* got, const char (&want)[8]) {
    return std::memcmp(got, want, 8) == 0;
}

} // namespace

// platform file mapping

bool AssetPack::MapFile (const std::string& path, Mapping& out, std::string* error) {
    out = Mapping{};

#if defined(_WIN32)
    // Widen for CreateFileW so a path with non-ASCII characters still opens; the
    // engine's own ToNativeString does the same thing for the same reason.
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide;
    if (wideLen > 0) {
        wide.resize(static_cast<size_t>(wideLen - 1));
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wideLen);
    }

    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "cannot open " + path);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        SetError(error, "empty or unreadable file " + path);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        SetError(error, "cannot map " + path);
        return false;
    }

    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        SetError(error, "cannot map view of " + path);
        return false;
    }

    out.handle = file;
    out.view   = mapping;
    out.data   = view;
    out.size   = static_cast<uint64_t>(size.QuadPart);
    return true;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        SetError(error, "cannot open " + path);
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        SetError(error, "empty or unreadable file " + path);
        return false;
    }
    void* view = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        SetError(error, "cannot map " + path);
        return false;
    }
    out.handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    out.view   = nullptr;
    out.data   = view;
    out.size   = static_cast<uint64_t>(st.st_size);
    return true;
#endif
}

void AssetPack::UnmapFile (Mapping& m) {
    if (m.data == nullptr) return;
#if defined(_WIN32)
    UnmapViewOfFile(m.data);
    if (m.view)   CloseHandle(static_cast<HANDLE>(m.view));
    if (m.handle) CloseHandle(static_cast<HANDLE>(m.handle));
#else
    ::munmap(const_cast<void*>(m.data), static_cast<size_t>(m.size));
    ::close(static_cast<int>(reinterpret_cast<intptr_t>(m.handle)));
#endif
    m = Mapping{};
}

// open / close

AssetPack::~AssetPack () { Close(); }

bool AssetPack::Open (const std::string& strdPath, std::string* error, bool verifyParts) {
    Close();

    if (!MapFile(strdPath, indexMap_, error)) return false;
    path_ = strdPath;

    const uint8_t* base = static_cast<const uint8_t*>(indexMap_.data);
    const uint64_t size = indexMap_.size;

    if (size < sizeof(StrdHeader)) {
        SetError(error, strdPath + ": too small to be a .strd");
        Close();
        return false;
    }

    const StrdHeader* h = reinterpret_cast<const StrdHeader*>(base);
    if (!MagicMatches(h->magic, kStrdMagic)) {
        SetError(error, strdPath + ": not a .strd (bad magic)");
        Close();
        return false;
    }
    if (h->version != kStrdVersion) {
        SetError(error, strdPath + ": .strd version " + std::to_string(h->version) +
                        ", this build reads version " + std::to_string(kStrdVersion));
        Close();
        return false;
    }

    // Every table has to sit inside the file before a single entry of it is believed.
    // A truncated index otherwise reads as a valid index full of garbage offsets, and
    // the first symptom is a crash somewhere else entirely.
    auto fits = [size](uint64_t off, uint64_t bytes) {
        return off <= size && bytes <= size - off;
    };
    if (!fits(h->bucketTableOffset, uint64_t(h->bucketCount) * sizeof(uint32_t)) ||
        !fits(h->assetTableOffset,  uint64_t(h->assetCount)  * sizeof(StrdAsset)) ||
        !fits(h->partTableOffset,   uint64_t(h->partCount)   * sizeof(StrdPart)) ||
        !fits(h->nameHeapOffset,    h->nameHeapSize)) {
        SetError(error, strdPath + ": .strd tables run past the end of the file");
        Close();
        return false;
    }
    if (h->bucketCount == 0 || (h->bucketCount & (h->bucketCount - 1)) != 0) {
        SetError(error, strdPath + ": .strd bucket count is not a power of two");
        Close();
        return false;
    }

    const uint64_t tailHash = Hash64(base + sizeof(StrdHeader),
                                     static_cast<size_t>(size - sizeof(StrdHeader)));
    if (tailHash != h->indexHash) {
        SetError(error, strdPath + ": .strd index hash mismatch (the index is corrupt)");
        Close();
        return false;
    }

    header_  = h;
    buckets_ = reinterpret_cast<const uint32_t*>  (base + h->bucketTableOffset);
    assets_  = reinterpret_cast<const StrdAsset*>(base + h->assetTableOffset);
    parts_   = reinterpret_cast<const StrdPart*> (base + h->partTableOffset);
    names_   = reinterpret_cast<const char*>      (base + h->nameHeapOffset);

    // Map every part now, so the read path never locks. See the header comment.
    const std::string dir = DirectoryOf(strdPath);
    partMaps_.resize(h->partCount);
    for (uint32_t i = 0; i < h->partCount; ++i) {
        const StrdPart& p = parts_[i];
        if (p.nameOffset + p.nameLength > header_->nameHeapSize) {
            SetError(error, strdPath + ": part " + std::to_string(i) + " has a bad name reference");
            Close();
            return false;
        }
        const std::string partPath = dir + std::string(names_ + p.nameOffset, p.nameLength);

        if (!MapFile(partPath, partMaps_[i], error)) {
            SetError(error, "missing pack part " + partPath +
                            " (the .strd expects it next to itself)");
            Close();
            return false;
        }
        if (partMaps_[i].size != p.fileSize) {
            SetError(error, partPath + ": size is " + std::to_string(partMaps_[i].size) +
                            " but the index recorded " + std::to_string(p.fileSize) +
                            " — this part does not belong to this index");
            Close();
            return false;
        }
        if (partMaps_[i].size < sizeof(StafpHeader)) {
            SetError(error, partPath + ": too small to be a .stafp");
            Close();
            return false;
        }

        const StafpHeader* ph = reinterpret_cast<const StafpHeader*>(partMaps_[i].data);
        if (!MagicMatches(ph->magic, kStafpMagic) || ph->version != kStafpVersion) {
            SetError(error, partPath + ": not a readable .stafp part");
            Close();
            return false;
        }
        if (ph->packUuidLo != h->packUuidLo || ph->packUuidHi != h->packUuidHi) {
            SetError(error, partPath + ": belongs to a different pack build than the .strd"
                            " — rebuild or reinstall so the set matches");
            Close();
            return false;
        }
        if (ph->partIndex != p.index) {
            SetError(error, partPath + ": is part " + std::to_string(ph->partIndex) +
                            " but the index lists it as part " + std::to_string(p.index));
            Close();
            return false;
        }

        if (verifyParts && !VerifyPart(i, error)) {
            Close();
            return false;
        }
    }

    return true;
}

void AssetPack::Close () {
    for (Mapping& m : partMaps_) UnmapFile(m);
    partMaps_.clear();
    UnmapFile(indexMap_);
    header_  = nullptr;
    buckets_ = nullptr;
    assets_  = nullptr;
    parts_   = nullptr;
    names_   = nullptr;
    path_.clear();
}

// lookup

const StrdAsset* AssetPack::Find (uint64_t id) const {
    if (header_ == nullptr || header_->assetCount == 0) return nullptr;

    const uint32_t mask = header_->bucketCount - 1;
    uint32_t slot = static_cast<uint32_t>(id) & mask;

    // Open addressing with linear probing. Load factor is capped at 0.5 by the writer,
    // so the expected probe count is under 1.5 and the worst case is bounded by the
    // bucket count - hence the hard loop limit rather than a while(true).
    for (uint32_t probe = 0; probe <= mask; ++probe) {
        const uint32_t entry = buckets_[slot];
        if (entry == kEmptyBucket) return nullptr;
        if (entry < header_->assetCount && assets_[entry].id == id) return &assets_[entry];
        slot = (slot + 1) & mask;
    }
    return nullptr;
}

const StrdAsset* AssetPack::Find (const std::string& logicalPath) const {
    return Find(AssetIdFromPath(logicalPath));
}

const char* AssetPack::NameData (const StrdAsset& a) const {
    if (names_ == nullptr) return "";
    return names_ + a.nameOffset;
}

std::string AssetPack::NameString (const StrdAsset& a) const {
    if (names_ == nullptr) return std::string();
    return std::string(names_ + a.nameOffset, a.nameLength);
}

const StrdAsset* AssetPack::AssetAt (uint32_t i) const {
    if (header_ == nullptr || i >= header_->assetCount) return nullptr;
    return &assets_[i];
}

AssetInfo AssetPack::InfoOf (const StrdAsset& a) const {
    AssetInfo info;
    info.id          = a.id;
    info.name        = NameString(a);
    info.type        = static_cast<AssetType>(a.type);
    info.codec       = static_cast<Codec>(a.codec);
    info.size        = a.originalSize;
    info.storedSize  = a.storedSize;
    info.contentHash = a.contentHash;
    info.partIndex   = a.partIndex;
    info.flags       = a.flags;
    if (const StrdPart* p = PartRecord(a.partIndex)) info.partNumber = p->index;
    return info;
}

AssetInfo AssetPack::InfoAt (uint32_t i) const {
    if (const StrdAsset* a = AssetAt(i)) return InfoOf(*a);
    return AssetInfo{};
}

const StrdPart* AssetPack::PartRecord (uint32_t partIndex) const {
    if (header_ == nullptr || partIndex >= header_->partCount) return nullptr;
    return &parts_[partIndex];
}

PartInfo AssetPack::PartAt (uint32_t i) const {
    PartInfo info;
    const StrdPart* p = PartRecord(i);
    if (p == nullptr) return info;
    info.number     = p->index;
    info.fileName   = std::string(names_ + p->nameOffset, p->nameLength);
    info.fileSize   = p->fileSize;
    info.fileHash   = p->fileHash;
    info.assetCount = p->assetCount;
    info.flags      = p->flags;
    info.mapped     = i < partMaps_.size() && partMaps_[i].data != nullptr;
    return info;
}

const uint8_t* AssetPack::PartBytes (uint32_t partIndex, uint64_t& sizeOut) const {
    sizeOut = 0;
    if (partIndex >= partMaps_.size()) return nullptr;
    sizeOut = partMaps_[partIndex].size;
    return static_cast<const uint8_t*>(partMaps_[partIndex].data);
}

// reading

const uint8_t* AssetPack::MappedData (const StrdAsset& a) const {
    if (static_cast<Codec>(a.codec) != Codec::None) return nullptr;
    uint64_t partSize = 0;
    const uint8_t* part = PartBytes(a.partIndex, partSize);
    if (part == nullptr) return nullptr;
    if (a.offset > partSize || a.storedSize > partSize - a.offset) return nullptr;
    return part + a.offset;
}

bool AssetPack::OwnsMappedPointer (const void* p) const {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    for (const Mapping& m : partMaps_) {
        const uint8_t* begin = static_cast<const uint8_t*>(m.data);
        if (begin != nullptr && q >= begin && q < begin + m.size) return true;
    }
    return false;
}

bool AssetPack::Read (const StrdAsset& a, std::vector<uint8_t>& out, std::string* error) const {
    return ReadRange(a, 0, a.originalSize, out, error);
}

bool AssetPack::ReadRange (const StrdAsset& a, uint64_t offset, uint64_t size,
                           std::vector<uint8_t>& out, std::string* error) const {
    out.clear();

    uint64_t partSize = 0;
    const uint8_t* part = PartBytes(a.partIndex, partSize);
    if (part == nullptr) {
        SetError(error, "asset refers to part " + std::to_string(a.partIndex) + ", which is not mapped");
        return false;
    }
    if (a.offset > partSize || a.storedSize > partSize - a.offset) {
        SetError(error, "asset payload runs past the end of its part");
        return false;
    }

    // Clamp the window to the asset. Asking for the tail of a file is normal (that is
    // what mip streaming does); asking for something entirely past the end is an empty
    // answer, not an error, because that is what a plain file read would give.
    if (offset >= a.originalSize) return true;
    const uint64_t avail = a.originalSize - offset;
    if (size > avail) size = avail;
    if (size == 0) return true;

    return Decode(a, part + a.offset, offset, size, out, error);
}

bool AssetPack::Decode (const StrdAsset& a, const uint8_t* src,
                        uint64_t wantOffset, uint64_t wantSize,
                        std::vector<uint8_t>& out, std::string* error) const {
    switch (static_cast<Codec>(a.codec)) {

    case Codec::None: {
        // A stored asset is already the answer; the "decode" is a copy of the window.
        out.resize(static_cast<size_t>(wantSize));
        std::memcpy(out.data(), src + wantOffset, static_cast<size_t>(wantSize));
        return true;
    }

    case Codec::Zstd: {
        // One frame over the whole asset: any window costs a full decompress. The
        // packer only chooses this for assets it also marks non-streamable.
        std::vector<uint8_t> whole;
        whole.resize(static_cast<size_t>(a.originalSize));
        const size_t got = ZSTD_decompress(whole.data(), whole.size(),
                                           src, static_cast<size_t>(a.storedSize));
        if (ZSTD_isError(got) || got != a.originalSize) {
            SetError(error, std::string("zstd decompress failed: ") +
                            (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output"));
            return false;
        }
        out.resize(static_cast<size_t>(wantSize));
        std::memcpy(out.data(), whole.data() + wantOffset, static_cast<size_t>(wantSize));
        return true;
    }

    case Codec::ZstdChunked: {
        if (a.storedSize < sizeof(ChunkTableHeader)) {
            SetError(error, "chunked asset is missing its frame table");
            return false;
        }
        ChunkTableHeader table{};
        std::memcpy(&table, src, sizeof(table));

        const uint64_t tableBytes = sizeof(ChunkTableHeader) +
                                    uint64_t(table.frameCount + 1) * sizeof(uint64_t);
        if (table.chunkSize == 0 || table.frameCount == 0 || tableBytes > a.storedSize ||
            table.uncompressedSize != a.originalSize) {
            SetError(error, "chunked asset has an inconsistent frame table");
            return false;
        }

        const uint64_t* frameOffsets =
            reinterpret_cast<const uint64_t*>(src + sizeof(ChunkTableHeader));

        // Only the frames the window overlaps are decoded. That is the whole point of
        // this codec: a 200 MB texture compressed end to end would have to be fully
        // decompressed to hand back its top mip.
        const uint64_t firstFrame = wantOffset / table.chunkSize;
        const uint64_t lastFrame  = (wantOffset + wantSize - 1) / table.chunkSize;
        if (lastFrame >= table.frameCount) {
            SetError(error, "chunked read runs past the last frame");
            return false;
        }

        out.resize(static_cast<size_t>(wantSize));

        std::vector<uint8_t> frame;
        frame.resize(table.chunkSize);

        for (uint64_t f = firstFrame; f <= lastFrame; ++f) {
            const uint64_t begin = frameOffsets[f];
            const uint64_t end   = frameOffsets[f + 1];
            if (end < begin || end > a.storedSize) {
                SetError(error, "chunked asset frame " + std::to_string(f) + " is out of range");
                return false;
            }

            // The last frame is short whenever the asset is not a whole multiple of
            // the chunk size.
            const uint64_t frameOrigin = f * table.chunkSize;
            const uint64_t frameLen    = std::min<uint64_t>(table.chunkSize,
                                                            a.originalSize - frameOrigin);

            const size_t got = ZSTD_decompress(frame.data(), static_cast<size_t>(frameLen),
                                               src + begin, static_cast<size_t>(end - begin));
            if (ZSTD_isError(got) || got != frameLen) {
                SetError(error, std::string("zstd frame decompress failed: ") +
                                (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output"));
                return false;
            }

            // Copy just the overlap of this frame with the requested window.
            const uint64_t copyBegin = std::max(wantOffset, frameOrigin);
            const uint64_t copyEnd   = std::min(wantOffset + wantSize, frameOrigin + frameLen);
            if (copyEnd > copyBegin) {
                std::memcpy(out.data() + (copyBegin - wantOffset),
                            frame.data() + (copyBegin - frameOrigin),
                            static_cast<size_t>(copyEnd - copyBegin));
            }
        }
        return true;
    }

    default:
        SetError(error, "asset uses codec " + std::to_string(a.codec) + ", which this build does not know");
        return false;
    }
}

// integrity

bool AssetPack::VerifyAsset (const StrdAsset& a, std::string* error) const {
    std::vector<uint8_t> bytes;
    if (!Read(a, bytes, error)) return false;
    const uint64_t got = Hash64(bytes.data(), bytes.size());
    if (got != a.contentHash) {
        SetError(error, NameString(a) + ": content hash mismatch");
        return false;
    }
    return true;
}

bool AssetPack::VerifyPart (uint32_t partIndex, std::string* error) const {
    const StrdPart* p = PartRecord(partIndex);
    if (p == nullptr) {
        SetError(error, "no such part");
        return false;
    }
    uint64_t partSize = 0;
    const uint8_t* bytes = PartBytes(partIndex, partSize);
    if (bytes == nullptr) {
        SetError(error, "part is not mapped");
        return false;
    }

    // The part's own headerHash field is excluded, because it is computed over the
    // rest of the header and cannot cover itself. Everything else is hashed exactly
    // as it sits on disk.
    Hasher64 hasher;
    StafpHeader head{};
    std::memcpy(&head, bytes, sizeof(head));
    const uint64_t storedHeaderHash = head.headerHash;
    head.headerHash = 0;
    hasher.Update(&head, sizeof(head));
    hasher.Update(bytes + sizeof(StafpHeader), static_cast<size_t>(partSize - sizeof(StafpHeader)));

    const uint64_t headerOnly = Hash64(&head, sizeof(head));
    if (headerOnly != storedHeaderHash) {
        SetError(error, PartAt(partIndex).fileName + ": part header is corrupt");
        return false;
    }
    if (hasher.Digest() != p->fileHash) {
        SetError(error, PartAt(partIndex).fileName +
                        ": file hash mismatch — the part is damaged or was replaced");
        return false;
    }
    return true;
}

bool AssetPack::VerifyAll (std::string* error,
                           void (*progress)(uint32_t, uint32_t, void*), void* userdata) const {
    if (header_ == nullptr) {
        SetError(error, "pack is not open");
        return false;
    }
    for (uint32_t i = 0; i < header_->partCount; ++i) {
        if (progress) progress(i, header_->partCount, userdata);
        if (!VerifyPart(i, error)) return false;
    }
    if (progress) progress(header_->partCount, header_->partCount, userdata);
    return true;
}

// shared helpers

AssetType ClassifyByExtension (const std::string& path) {
    const std::string e = LowerExtension(path);
    if (e.empty()) return AssetType::Binary;

    if (e == "dds" || e == "ktx" || e == "ktx2" || e == "basis")            return AssetType::Texture;
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" ||
        e == "bmp" || e == "hdr" || e == "exr" || e == "gif" || e == "psd") return AssetType::Image;
    if (e == "wiscene")                                                     return AssetType::Model;
    if (e == "stsd")                                                        return AssetType::Scene;
    if (e == "wav" || e == "flac" || e == "aiff")                           return AssetType::Sound;
    if (e == "ogg" || e == "mp3" || e == "m4a" || e == "opus")              return AssetType::Music;
    if (e == "mp4" || e == "h264" || e == "mkv" || e == "webm")             return AssetType::Video;
    if (e == "lua")                                                         return AssetType::Script;
    if (e == "ttf" || e == "otf")                                           return AssetType::Font;
    if (e == "cso" || e == "spv" || e == "hlsl" || e == "hlsli")            return AssetType::Shader;
    if (e == "staod" || e == "anim")                                        return AssetType::Animation;
    if (e == "json")                                                        return AssetType::Json;
    if (e == "txt" || e == "md" || e == "csv" || e == "ini" ||
        e == "xml" || e == "yaml" || e == "yml")                            return AssetType::Text;
    return AssetType::Binary;
}

bool IsCompressedContainer (const std::string& path) {
    const std::string e = LowerExtension(path);
    // Formats whose bytes are ALREADY entropy-coded or block-compressed. Running zstd
    // over one of these typically saves 1-3% and costs a decompress on every read
    // and, worse, it gives up the zero-copy mapped read that mip and audio streaming
    // depend on. Raw-sample formats (bmp, tga, hdr, exr, psd, wav) are deliberately NOT
    // in this list: they compress like any other buffer.
    return e == "png"  || e == "jpg"  || e == "jpeg" || e == "gif"  || e == "webp" ||
           e == "dds"  || e == "ktx"  || e == "ktx2" || e == "basis" ||
           e == "ogg"  || e == "mp3"  || e == "m4a"  || e == "opus" || e == "flac" ||
           e == "mp4"  || e == "h264" || e == "mkv"  || e == "webm" ||
           e == "zst"  || e == "zip"  || e == "7z"   || e == "gz";
}

Codec DefaultCodecFor (const std::string& path, AssetType type, uint64_t size) {
    if (IsCompressedContainer(path)) return Codec::None;
    if (type == AssetType::Texture || type == AssetType::Image ||
        type == AssetType::Music   || type == AssetType::Video) {
        // A raw-sample file that landed on one of the "media" types - .bmp, .tga, .hdr,
        // .wav. Chunked so it stays streamable while still shrinking.
        return size >= kDefaultChunkSize ? Codec::ZstdChunked : Codec::Zstd;
    }
    return DefaultCodecFor(type, size);
}

Codec DefaultCodecFor (AssetType type, uint64_t size) {
    switch (type) {
    // Already-compressed containers. zstd over a .dds or an .ogg typically buys a few
    // percent and costs a decompress on every single read - and, worse, it takes away
    // the zero-copy mapped path that mip and audio streaming rely on. Stored wins.
    case AssetType::Texture:
    case AssetType::Image:
    case AssetType::Music:
    case AssetType::Video:
        return Codec::None;

    // Uncompressed audio and everything text-shaped compresses well and is read whole.
    case AssetType::Sound:
    case AssetType::Script:
    case AssetType::Text:
    case AssetType::Json:
    case AssetType::Font:
    case AssetType::Shader:
    case AssetType::Animation:
        return size >= kDefaultChunkSize ? Codec::ZstdChunked : Codec::Zstd;

    // A .wiscene or a mesh blob is large, compressible and read in ranges, which is
    // exactly the case chunked framing exists for.
    case AssetType::Model:
    case AssetType::Mesh:
    case AssetType::Scene:
    case AssetType::Material:
    case AssetType::Binary:
    default:
        return size >= kDefaultChunkSize ? Codec::ZstdChunked : Codec::Zstd;
    }
}

const char* ToString (AssetType t) {
    switch (t) {
    case AssetType::Texture:   return "Texture";
    case AssetType::Image:     return "Image";
    case AssetType::Model:     return "Model";
    case AssetType::Mesh:      return "Mesh";
    case AssetType::Material:  return "Material";
    case AssetType::Sound:     return "Sound";
    case AssetType::Music:     return "Music";
    case AssetType::Video:     return "Video";
    case AssetType::Script:    return "Script";
    case AssetType::Font:      return "Font";
    case AssetType::Shader:    return "Shader";
    case AssetType::Scene:     return "Scene";
    case AssetType::Animation: return "Animation";
    case AssetType::Text:      return "Text";
    case AssetType::Json:      return "Json";
    case AssetType::Binary:    return "Binary";
    case AssetType::Custom:    return "Custom";
    default:                   return "Unknown";
    }
}

const char* ToString (Codec c) {
    switch (c) {
    case Codec::None:        return "stored";
    case Codec::Zstd:        return "zstd";
    case Codec::ZstdChunked: return "zstd-chunked";
    default:                 return "?";
    }
}

std::string FormatBytes (uint64_t bytes) {
    char buf[64];
    if (bytes < 1024ull) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", double(bytes) / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

std::string PartFileName (const std::string& baseName, uint32_t partNumber) {
    return baseName + ".stafp" + std::to_string(partNumber);
}

} // namespace st::asset
