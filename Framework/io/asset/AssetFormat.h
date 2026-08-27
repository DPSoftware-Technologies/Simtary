#pragma once
// On-disk layout of the Simtary asset package: .staod + .stafp<N> + .stsd.
//
// This header is the SINGLE definition of those three formats. The build-time packer
// (tools/stpack) and the runtime reader (AssetPack) both include it and nothing else
// of each other, so a layout change is one edit and both sides move together.
//
// ── Why three files and not one ────────────────────────────────────────────────
//
//   .staod        "asset object descriptor" — the INDEX. Which asset lives in which
//                 part, at what offset, how big, how compressed, what it hashes to.
//                 Small (about 80 bytes per asset), read once, kept mapped.
//   .stafp<N>     "asset fragment part N" — the PAYLOAD. Raw asset bytes back to
//                 back. N is a literal decimal counter: foo.stafp1, foo.stafp2, ...
//                 A part is capped (default 50 MB, hard max 100 MB) so a patch ships
//                 one part instead of one monolith, and so 32-bit mmap windows and
//                 FAT32/ExFAT sticks stay viable.
//   .stsd         "scene descriptor" — one map. NBT metadata for everything a human
//                 or a tool wants to read, plus the engine's entity payload as an
//                 out-of-band blob (see below).
//
// ── Why the index is NOT NBT ───────────────────────────────────────────────────
//
// NBT is a linear, self-describing tree: finding one asset means walking every tag
// before it and allocating a Tag node for each. That is fine for a 40-key options
// file and wrong for a 100,000-entry index that has to answer "where is asset X"
// during a load screen. So .staod is a flat, fixed-stride, memory-mappable table
// with a power-of-two open-addressed hash bucket array in front of it:
//
//      id -> bucket = id & (bucketCount-1) -> linear probe -> assetTable[i]
//
// Lookup is O(1), touches two cache lines, allocates nothing, and parses nothing —
// the mapped bytes ARE the data structure. .stsd keeps NBT for the parts that want
// to be inspectable and hand-editable, and keeps the megabytes out of it.
//
// ── Endianness and packing ─────────────────────────────────────────────────────
//
// Little-endian, explicitly-sized fields, hand-padded so every struct has the same
// layout on every compiler without #pragma pack. This matches wi::Archive, which is
// also raw little-endian, and every platform the engine targets. NBT elsewhere in
// Framework/io is big-endian because it deliberately follows the classic NBT spec;
// that does not apply here.
//
// ── Compatibility ──────────────────────────────────────────────────────────────
//
// Every header carries a version. A reader refuses a version it does not know rather
// than guessing. Fields are append-only into the reserved tail; nothing is ever
// renumbered, because a shipped .staod on a player's disk outlives the source tree.

#include <cstdint>
#include <cstddef>

namespace st::asset {

// ── magic values ───────────────────────────────────────────────────────────────
// 8 bytes so the struct stays 8-aligned and so `file` / a hex editor shows it plainly.
inline constexpr char kStaodMagic[8] = { 'S','T','A','O','D','\0','\0','\0' };
inline constexpr char kStafpMagic[8] = { 'S','T','A','F','P','\0','\0','\0' };
inline constexpr char kStsdMagic [8] = { 'S','T','S','D','\0','\0','\0','\0' };

inline constexpr uint32_t kStaodVersion = 1;
inline constexpr uint32_t kStafpVersion = 1;
inline constexpr uint32_t kStsdVersion  = 1;

// Payload alignment inside a part. 4096 = one page and one NVMe sector, so a ranged
// read of an asset never straddles a page it does not need, and a mapped view starts
// where the OS wants it to. Costs at most 4 KB of slack per asset.
inline constexpr uint64_t kPartAlignment = 4096;

// Part size policy. `Default` is what the packer targets; `Max` is the ceiling the
// caller may raise it to. A single asset larger than the cap gets a part to itself
// and that part exceeds the cap — splitting one asset across two files would cost a
// second seek on every read of it, which is the one thing this format exists to
// avoid.
inline constexpr uint64_t kDefaultPartSize = 50ull  * 1024 * 1024;
inline constexpr uint64_t kMaxPartSize     = 100ull * 1024 * 1024;

// Frame size for Codec::ZstdChunked. Big enough that zstd's ratio is close to
// one-shot, small enough that a ranged read wastes at most one frame at each end.
inline constexpr uint32_t kDefaultChunkSize = 256 * 1024;

// ── enums ──────────────────────────────────────────────────────────────────────

// What an asset IS. Drives the DevUI explorer's grouping and icons, the packer's
// default codec choice, and nothing in the read path — the bytes are the bytes.
enum class AssetType : uint16_t {
    Unknown   = 0,
    Texture   = 1,   // dds / ktx / basis — GPU-ready, often streamed by mip
    Image     = 2,   // png / jpg / tga / bmp — decoded at load
    Model     = 3,   // .wiscene used as a model
    Mesh      = 4,   // raw mesh blob
    Material  = 5,
    Sound     = 6,   // wav / ogg — short, fully loaded
    Music     = 7,   // ogg / mp3 — long, streamed
    Video     = 8,   // mp4 / h264
    Script    = 9,   // lua
    Font      = 10,
    Shader    = 11,  // cso / spv
    Scene     = 12,  // .stsd
    Animation = 13,  // .staod animation descriptor (the NBT kind), .anim
    Text      = 14,
    Json      = 15,
    Binary    = 16,  // known blob, no further meaning
    Custom    = 1000 // project-defined; userFlags carries the discriminator
};

// How the payload is stored. The choice is per asset, not per pack, because the
// right answer differs by content: a .dds is already block-compressed and zstd buys
// ~2% for a decompress on every read, while a .lua or a mesh blob halves.
enum class Codec : uint16_t {
    None        = 0,  // stored verbatim. The only codec that supports a zero-copy
                      // mapped read, which is what makes texture mip streaming work.
    Zstd        = 1,  // one zstd frame over the whole asset. Smallest, but ANY read
                      // decompresses the whole thing — not streamable.
    ZstdChunked = 2   // fixed-size frames + an offset table. A ranged read touches
                      // only the frames it overlaps, so it is both compressed AND
                      // seekable. Default for anything large and compressible.
};

// Per-asset flags. Bit values are on-disk state — append only.
enum AssetFlags : uint32_t {
    AssetFlag_None       = 0,
    AssetFlag_Streamable = 1u << 0, // engine may read sub-ranges (texture mips, audio)
    AssetFlag_Preload    = 1u << 1, // pull into memory when the pack is mounted
    AssetFlag_Generated  = 1u << 2, // produced by the packer, not authored
    AssetFlag_FromScene  = 1u << 3, // lifted out of a .wiscene's embedded resource block
};

// Per-part flags.
enum PartFlags : uint32_t {
    PartFlag_None      = 0,
    PartFlag_Oversized = 1u << 0, // holds one asset larger than the size cap
};

// Per-pack flags (in the .staod header).
enum PackFlags : uint32_t {
    PackFlag_None       = 0,
    PackFlag_Verified   = 1u << 0, // hashes were computed at build time and are trustworthy
};

// ── .staod ─────────────────────────────────────────────────────────────────────
//
// Layout:
//   [StaodHeader        ] 128 B at offset 0
//   [bucket table       ] uint32_t[bucketCount], kEmptyBucket = 0xFFFFFFFF
//   [asset table        ] StaodAsset[assetCount], sorted ascending by id
//   [part table         ] StaodPart[partCount]
//   [name heap          ] UTF-8 logical paths, NUL-terminated, referenced by offset
//
// Everything after the header is covered by `indexHash`, so one XXH64 over the tail
// says whether the index itself is intact before a single field of it is believed.

inline constexpr uint32_t kEmptyBucket = 0xFFFFFFFFu;

struct StaodHeader {                 // 128 bytes
    char     magic[8];               // kStaodMagic
    uint32_t version;                // kStaodVersion
    uint32_t flags;                  // PackFlags

    // Identifies this pack SET. Every .stafp<N> written alongside carries the same
    // pair, which is how a mismatched or stale part is caught before it is read
    // rather than after it has produced garbage geometry.
    uint64_t packUuidLo;
    uint64_t packUuidHi;

    uint32_t partCount;
    uint32_t assetCount;
    uint32_t bucketCount;            // power of two, >= assetCount * 2 (load factor <= 0.5)
    uint32_t nameHeapSize;

    uint64_t bucketTableOffset;
    uint64_t assetTableOffset;
    uint64_t partTableOffset;
    uint64_t nameHeapOffset;

    uint64_t totalPayloadSize;       // sum of every part's payload, for progress bars
    uint64_t buildTimestamp;         // seconds since epoch, informational
    uint64_t indexHash;              // XXH64 of [sizeof(StaodHeader), end of file)
    uint64_t reserved[3];
};
static_assert(sizeof(StaodHeader) == 128, "StaodHeader is an on-disk layout");

struct StaodPart {                   // 64 bytes
    uint32_t index;                  // the N in .stafp<N>; 1-based
    uint32_t flags;                  // PartFlags
    uint64_t fileSize;               // exact byte size of the part file
    // XXH64 of the WHOLE part file, header included, with StafpHeader::headerHash
    // zeroed while hashing (that field is computed after the rest of the header, so
    // including it would be self-referential). Verifying a part is one sequential
    // read — the point is catching a truncated download or a bad sector before the
    // player walks into a level with half a wall in it.
    uint64_t fileHash;
    uint64_t nameOffset;             // file name in the name heap, e.g. "content.stafp1"
    uint32_t nameLength;
    uint32_t assetCount;             // how many assets live in this part
    uint64_t reserved[3];
};
static_assert(sizeof(StaodPart) == 64, "StaodPart is an on-disk layout");

struct StaodAsset {                  // 80 bytes
    uint64_t id;                     // XXH64 of the canonical logical path
    uint64_t offset;                 // byte offset of the payload within its part file
    uint64_t storedSize;             // bytes on disk (post-compression)
    uint64_t originalSize;           // bytes after decompression
    uint64_t contentHash;            // XXH64 of the ORIGINAL (decompressed) bytes
    uint64_t nameOffset;             // logical path in the name heap
    uint32_t nameLength;
    uint32_t partIndex;              // index INTO the part table, not the N of the file
    uint16_t type;                   // AssetType
    uint16_t codec;                  // Codec
    uint32_t flags;                  // AssetFlags
    uint32_t chunkSize;              // Codec::ZstdChunked only; 0 otherwise
    uint32_t reserved[3];
};
static_assert(sizeof(StaodAsset) == 80, "StaodAsset is an on-disk layout");

// ── .stafp<N> ──────────────────────────────────────────────────────────────────
//
// Layout:
//   [StafpHeader ] 64 B at offset 0
//   [padding     ] to kPartAlignment
//   [asset bytes ] each starting at a kPartAlignment boundary
//
// Deliberately no per-asset header in the part. Everything needed to read an asset is
// already in the mapped .staod, so a cold read is exactly one seek and one read of
// exactly the right length. A per-asset header would cost a second read, or a larger
// one, for information the reader already has.

struct StafpHeader {                 // 64 bytes
    char     magic[8];               // kStafpMagic
    uint32_t version;                // kStafpVersion
    uint32_t partIndex;              // the N in the file name; 1-based
    uint64_t packUuidLo;             // must match the .staod
    uint64_t packUuidHi;
    uint64_t payloadOffset;          // first asset byte; == kPartAlignment
    uint64_t payloadSize;
    uint64_t assetCount;
    uint64_t headerHash;             // XXH64 of this header with this field zeroed
};
static_assert(sizeof(StafpHeader) == 64, "StafpHeader is an on-disk layout");

// Codec::ZstdChunked payload prologue, at the asset's `offset`:
//   [ChunkTableHeader][uint64_t frameOffset[frameCount + 1]][frame 0][frame 1]...
// frameOffset values are relative to the START of the asset payload, and the extra
// trailing entry is the end of the last frame, so frame i is
// [frameOffset[i], frameOffset[i+1]) with no special case for the last one.
struct ChunkTableHeader {            // 16 bytes
    uint32_t chunkSize;              // uncompressed bytes per frame (last may be short)
    uint32_t frameCount;
    uint64_t uncompressedSize;
};
static_assert(sizeof(ChunkTableHeader) == 16, "ChunkTableHeader is an on-disk layout");

// ── .stsd ──────────────────────────────────────────────────────────────────────
//
// Layout:
//   [StsdHeader  ] 96 B at offset 0
//   [NBT block   ] one uncompressed big-endian NBT compound, root name "stsd"
//   [blob table  ] StsdBlob[blobCount]
//   [blob bytes  ] each kPartAlignment-aligned
//
// The NBT block carries what a person or a tool reads: scene name, source file,
// the referenced asset ID list, and an entity index (names, parents, transforms,
// native-component metadata) that makes a map greppable without loading the engine.
//
// The blob region carries what the ENGINE reads. Blob 0 is a complete, valid
// wi::Archive `.wiscene` — the original one, with its embedded resource block
// emptied. Keeping it as a whole archive rather than transcribing the ECS into NBT
// is deliberate:
//
//   - Wicked serializes ~40 component managers with their own versioned formats.
//     Transcribing them means re-implementing all of it, and re-implementing it
//     again every time the engine core moves. The engine already has a correct,
//     maintained serializer; this format's job is to move BYTES, not to duplicate it.
//   - It makes conversion exactly reversible. Splitting is a header patch plus a
//     copy, so the .wiscene that comes back out is byte-identical modulo resource
//     ordering — which is the property that lets a project adopt this and still
//     open the map in the editor.
//
// So the NBT is authoritative for metadata, the blob is authoritative for entities,
// and the two are never asked the same question.

enum class StsdBlobKind : uint32_t {
    Unknown       = 0,
    EcsArchive    = 1,  // blob 0: the resource-free .wiscene
    Thumbnail     = 2,  // jpg/png preview of the map
    Navmesh       = 3,  // reserved for a baked navigation mesh
    Lightmap      = 4,  // reserved
    ProjectCustom = 1000
};

struct StsdBlob {                    // 48 bytes
    uint64_t offset;                 // from the start of the .stsd file
    uint64_t storedSize;
    uint64_t originalSize;
    uint64_t contentHash;            // XXH64 of the original bytes
    uint32_t kind;                   // StsdBlobKind
    uint16_t codec;                  // Codec
    uint16_t reserved0;
    uint32_t chunkSize;              // ZstdChunked only
    uint32_t reserved1;
};
static_assert(sizeof(StsdBlob) == 48, "StsdBlob is an on-disk layout");

struct StsdHeader {                  // 96 bytes
    char     magic[8];               // kStsdMagic
    uint32_t version;                // kStsdVersion
    uint32_t flags;

    uint64_t packUuidLo;             // the pack this map's resources were packed into
    uint64_t packUuidHi;

    uint64_t nbtOffset;
    uint64_t nbtSize;

    uint64_t blobTableOffset;
    uint32_t blobCount;
    uint32_t reserved0;

    uint64_t assetRefCount;          // how many asset IDs the NBT's "assets" list holds
    uint64_t fileHash;               // XXH64 of [sizeof(StsdHeader), end of file)
    uint64_t reserved1[2];
};
static_assert(sizeof(StsdHeader) == 96, "StsdHeader is an on-disk layout");

// ── wi::Archive facts the converter relies on ──────────────────────────────────
//
// Splitting a .wiscene needs three things about the container, and all three are
// stable and checked at runtime rather than assumed:
//
//   1. The file opens with `uint64_t version` then `uint64_t properties`
//      (wi::Archive::Header, 16 B). properties.bits.thumbnail_data_size is the low 32
//      bits and properties.bits.compressed is bit 32.
//   2. From archive version 90 onward, wi::scene::Scene::Serialize writes, right
//      after a `uint32_t reserved` that the archive stores as 4 bytes:
//          uint64_t jumpBefore   // absolute offset of the embedded resource block
//          uint64_t jumpAfter    // absolute offset just past it
//      Both are offsets into the DECOMPRESSED stream, measured from byte 0 of the
//      file. That pair is what makes the split a patch instead of a re-encode: the
//      entity bytes are [afterJumps, jumpBefore) and the resources are
//      [jumpBefore, jumpAfter), and neither has to be understood to be moved.
//   3. The resource block is
//          uint64_t count
//          repeated: uint64_t nameLen, nameLen bytes, uint64_t flags, uint64_t dataLen,
//                    dataLen bytes
//      because wi::Archive widens every integer to 64 bits on the wire and writes a
//      std::string as length-then-chars.
//
// Archive versions below 90 have no jump pair and therefore no way to find the
// resource block without deserializing; the converter refuses them and says so.
inline constexpr uint64_t kMinSupportedArchiveVersion = 90;
inline constexpr uint32_t kArchiveHeaderSize          = 16;

} // namespace st::asset
