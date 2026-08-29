#pragma once
// .stsd — the scene (map) descriptor — plus the .wiscene split/merge that produces it.
//
// A .wiscene is one wi::Archive holding two unrelated things glued together: the
// entity/component payload, and a block of embedded resource FILES (every texture,
// mesh blob, sound and script the map touched). That is why Milistry's two maps are
// 37 MB and 39 MB, why they duplicate every shared texture between them, and why
// nothing in them can be streamed or patched independently.
//
// Splitting undoes exactly that glue and nothing else:
//
//     s1map.wiscene  ──>  s1map.stsd          entities + metadata, small
//                    └──>  content.stafp1..N   the resources, deduplicated, shared
//
// and merging puts it back, which is what makes the conversion safe to adopt: the
// map can always be turned back into a .wiscene the editor opens.
//
// ── What is in a .stsd ─────────────────────────────────────────────────────────
//
//   NBT block   metadata a person or a tool reads: name, source, engine archive
//               version, the list of asset IDs the map needs, and an entity index.
//   blob 0      a complete, valid .wiscene with its resource block emptied.
//
// The blob is bytes, not NBT, and that is deliberate. Wicked serializes ~40 component
// managers through their own versioned formats; transcribing them into NBT means
// re-implementing all of it and re-implementing it again on every engine merge, and
// the reverse conversion would have to be exact or the map is lost. Moving the bytes
// is exact by construction. NBT covers the part that benefits from being readable;
// the megabytes stay out of it, so opening a .stsd to read its name does not parse a
// tag tree the size of the map.
//
// No exceptions, no RTTI.

#include <cstdint>
#include <string>
#include <vector>

#include "AssetFormat.h"
#include "io/Nbt.h"

namespace st::asset {

// ── .wiscene surgery ───────────────────────────────────────────────────────────

// One entry of a .wiscene's embedded resource block.
struct EmbeddedResource {
    std::string name;         // path relative to the .wiscene, as the engine stored it
    uint32_t    engineFlags;  // wi::resourcemanager::Flags, preserved verbatim
    uint64_t    offset;       // where the bytes start in WisceneSplit::Bytes()
    uint64_t    size;
};

// The result of taking a .wiscene apart. Holds its own copy of the decompressed
// source when the input was a compressed archive, so `Bytes()` is valid either way.
struct WisceneSplit {
    uint64_t archiveVersion = 0;
    bool     wasCompressed  = false;

    // A complete .wiscene: the same header and the same entity bytes as the input,
    // with the embedded resource block replaced by an empty one. This is blob 0.
    std::vector<uint8_t> ecsArchive;

    // What was lifted out. `offset`/`size` index into Bytes().
    std::vector<EmbeddedResource> resources;

    const uint8_t* Bytes () const { return owned.empty() ? source : owned.data(); }
    uint64_t       Size  () const { return owned.empty() ? sourceSize : owned.size(); }

    // Filled by SplitWiscene; not for callers to set.
    const uint8_t*       source     = nullptr;
    uint64_t             sourceSize = 0;
    std::vector<uint8_t> owned;
};

// Take a .wiscene apart. `data`/`size` is the whole file. Fails on an archive older
// than kMinSupportedArchiveVersion, because before version 90 there is no jump pair
// and the resource block cannot be found without deserializing the scene — which
// would mean linking the engine into a build tool.
bool SplitWiscene (const uint8_t* data, uint64_t size, WisceneSplit& out, std::string* error = nullptr);

// One resource going back into a .wiscene.
struct ResourceToEmbed {
    std::string    name;
    uint32_t       engineFlags = 0;
    const uint8_t* data        = nullptr;
    uint64_t       size        = 0;
};

// Put a .wiscene back together from a split ECS archive and a set of resources. The
// output is byte-identical to the original input of SplitWiscene() whenever the same
// resources are supplied in the same order.
bool MergeWiscene (const uint8_t* ecsArchive, uint64_t ecsSize,
                   const std::vector<ResourceToEmbed>& resources,
                   std::vector<uint8_t>& out, std::string* error = nullptr);

// ── .stsd ──────────────────────────────────────────────────────────────────────

struct SceneAssetRef {
    uint64_t    id    = 0;
    std::string path;
    uint32_t    flags = 0;          // AssetFlags — how the pack stores it

    // The wi::resourcemanager::Flags the .wiscene recorded for this resource
    // (IMPORT_NORMALMAP, IMPORT_BLOCK_COMPRESSED, IMPORT_RETAIN_FILEDATA, ...). Carried
    // because the engine writes them per embedded resource and the rebuild has to put
    // the same values back: dropping them changes how a normal map is encoded on
    // reimport, and makes the round trip lossy in a way that only shows up as subtly
    // wrong shading.
    uint32_t    engineFlags = 0;
};

struct SceneBlob {
    StsdBlobKind         kind  = StsdBlobKind::Unknown;
    Codec                codec = Codec::None;
    std::vector<uint8_t> data;   // uncompressed, always — the codec applies on disk only
};

struct SceneDescriptor {
    std::string name;         // "s1map"
    std::string sourceFile;   // "scenes/s1map.wiscene", for provenance
    // The package build this map's resources were written alongside. ZERO means the map
    // is not bound to any particular build — an editor save whose resources were all
    // already mounted has nothing to bind to, and a UUID check against it is not a
    // mismatch, it is "no opinion".
    uint64_t    packUuidLo    = 0;
    uint64_t    packUuidHi    = 0;
    uint64_t    archiveVersion = 0;
    uint64_t    buildTimestamp = 0;

    // Every asset the map needs, in the pack. The runtime uses this to preload or to
    // check that the pack it mounted actually covers the map before loading it, which
    // turns "half the world is untextured" into one clear message at the door.
    std::vector<SceneAssetRef> assets;

    // Blob 0 is the ECS archive; the rest are optional (thumbnail, baked data).
    std::vector<SceneBlob> blobs;

    // Anything the project wants to carry along — spawn points, gameplay bounds,
    // level rules. Written into the NBT under "project" and handed back untouched.
    nbt::Tag project = nbt::Tag::Compound();

    const std::vector<uint8_t>* EcsArchive () const;
};

// Progress out of a read that can take a visible amount of time. The only one that
// does is inflating the entity blob — 36 MB of zstd is a pause the player sees, and
// without this the loading window sits frozen on the line before it.
//
// `report` is called from whatever thread is doing the read, which for
// ReadSceneDescriptor is the caller's. Keep it cheap: it fires once per compression
// frame, so roughly every 256 KB.
struct ReadProgress {
    void (*report)(uint64_t done, uint64_t total, void* userdata) = nullptr;
    void* userdata = nullptr;
};

struct SceneWriteOptions {
    Codec    blobCodec        = Codec::ZstdChunked;
    uint32_t chunkSize        = kDefaultChunkSize;
    int      compressionLevel = 9;
};

// Serialise to memory. WriteSceneDescriptor() is this plus a file write; the packer
// uses the memory form so a converted map goes straight into a pack part without ever
// touching the disk as a loose file.
bool SerializeSceneDescriptor (const SceneDescriptor& scene, std::vector<uint8_t>& out,
                               const SceneWriteOptions& options = SceneWriteOptions{},
                               std::string* error = nullptr);

bool WriteSceneDescriptor (const std::string& path, const SceneDescriptor& scene,
                           const SceneWriteOptions& options = SceneWriteOptions{},
                           std::string* error = nullptr);

// `loadBlobs == false` reads the header and the NBT only. That is the cheap call the
// DevUI scene list and the pack verifier make — it touches a few KB of a file whose
// blob may be 30 MB.
bool ReadSceneDescriptor (const std::string& path, SceneDescriptor& out,
                          bool loadBlobs = true, std::string* error = nullptr,
                          const ReadProgress& progress = ReadProgress{});

// Same, from memory — this is the runtime path, where the .stsd itself came out of a
// pack and was never a file.
bool ParseSceneDescriptor (const uint8_t* data, uint64_t size, SceneDescriptor& out,
                           bool loadBlobs = true, std::string* error = nullptr,
                           const ReadProgress& progress = ReadProgress{});

// ── conversion ─────────────────────────────────────────────────────────────────

// Convert one .wiscene: write `<outDir>/<name>.stsd` and hand every embedded resource
// to `writer` under `resourcePrefix + <its name>`. A resource already in the writer is
// skipped rather than duplicated, which is what makes two maps sharing a texture cost
// one copy instead of two.
//
// `writer` may be null, in which case the resources are dropped and only the entity
// payload is written — useful for inspecting a map, useless for running one.
class AssetPackWriter;

// Split one .wiscene and build its SceneDescriptor, handing every embedded resource to
// `writer`. Does not write anything itself — the caller decides whether the .stsd
// becomes a file or a pack entry.
bool BuildSceneDescriptor (const std::string& wiscenePath,
                           const std::string& resourcePrefix,
                           AssetPackWriter*   writer,
                           SceneDescriptor&   out,
                           std::string* error = nullptr);

// Same, from a .wiscene that is already in memory — which is what the in-game editor
// has after wi::scene::Scene::Serialize, with no file in between.
//
// `splitOut` keeps the resource BYTES alive and reachable: the descriptor only records
// names and IDs, so a caller that wants to decide per resource whether to pack it
// (the editor packs only what the mounted packages do not already hold) needs the split
// itself. Read them at `splitOut.Bytes() + r.offset`.
bool BuildSceneDescriptorFromMemory (const uint8_t* wisceneBytes, uint64_t size,
                                     const std::string& name,
                                     const std::string& sourceFile,
                                     const std::string& resourcePrefix,
                                     SceneDescriptor& out,
                                     WisceneSplit&    splitOut,
                                     std::string* error = nullptr);

// Convenience: Build + Serialize + write `<outDir>/<name>.stsd`.
bool ConvertWiscene (const std::string& wiscenePath,
                     const std::string& outDir,
                     const std::string& resourcePrefix,
                     AssetPackWriter*   writer,
                     const SceneWriteOptions& options,
                     std::string* error = nullptr);

// The reverse: rebuild a .wiscene from a descriptor plus a pack to pull its resources
// out of. `pack` may be null, which produces a scene with no embedded resources — valid,
// loadable, and untextured.
class AssetPack;
bool RebuildWiscene (const SceneDescriptor& scene,
                     const AssetPack*       pack,
                     std::vector<uint8_t>&  out,
                     std::string* error = nullptr);

} // namespace st::asset
