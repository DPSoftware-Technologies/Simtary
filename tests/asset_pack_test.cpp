// Round-trip tests for the asset package formats: XXH64, .staod/.stafp packing,
// .wiscene split/merge and .stsd serialisation.
//
// Returns the number of failed checks (0 = success), so it works as a CTest test too.
// Everything runs in a temp directory that is removed on the way out; no engine, no
// graphics device, no fixtures on disk.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "io/asset/AssetFormat.h"
#include "io/asset/AssetPack.h"
#include "io/asset/AssetPackWriter.h"
#include "io/asset/SceneDescriptor.h"
#include "io/asset/StHash.h"

namespace fs = std::filesystem;
using namespace st::asset;

static int failures = 0;

#define CHECK(cond, what)                                                            \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));             \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

namespace {

void AppendU64 (std::vector<uint8_t>& out, uint64_t v) {
    const size_t at = out.size();
    out.resize(at + sizeof(v));
    std::memcpy(out.data() + at, &v, sizeof(v));
}

void PatchU64 (std::vector<uint8_t>& out, size_t at, uint64_t v) {
    std::memcpy(out.data() + at, &v, sizeof(v));
}

// Deterministic pseudo-random filler. Not std::rand, because the same bytes have to
// come out on every platform for a byte-comparison test to mean anything.
std::vector<uint8_t> Filler (size_t size, uint64_t seed, bool compressible) {
    std::vector<uint8_t> out(size);
    uint64_t x = seed * 6364136223846793005ull + 1442695040888963407ull;
    for (size_t i = 0; i < size; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        // A compressible buffer has a small alphabet, which is what a mesh blob or a
        // script looks like to zstd; an incompressible one is what a .dds looks like.
        out[i] = compressible ? static_cast<uint8_t>('a' + ((x >> 33) % 4))
                              : static_cast<uint8_t>(x >> 33);
    }
    return out;
}

struct FakeResource {
    std::string          name;
    uint32_t             flags;
    std::vector<uint8_t> data;
};

// A minimal but layout-accurate .wiscene: the archive header, Scene::Serialize's
// reserved word and jump pair, some opaque "entity" bytes, and an embedded resource
// block. Everything the converter reads, and nothing it does not.
std::vector<uint8_t> MakeFakeWiscene (const std::vector<uint8_t>& ecsPayload,
                                      const std::vector<FakeResource>& resources) {
    std::vector<uint8_t> file;
    AppendU64(file, 94);        // archive version
    AppendU64(file, 0);         // properties: no thumbnail, not compressed
    AppendU64(file, 0);         // Scene::Serialize's uint32_t reserved, widened to 8 bytes

    const size_t jumpBeforeAt = file.size();
    AppendU64(file, 0);
    const size_t jumpAfterAt = file.size();
    AppendU64(file, 0);

    file.insert(file.end(), ecsPayload.begin(), ecsPayload.end());

    const uint64_t jumpBefore = file.size();
    AppendU64(file, resources.size());
    for (const FakeResource& r : resources) {
        AppendU64(file, r.name.size());
        file.insert(file.end(), r.name.begin(), r.name.end());
        AppendU64(file, r.flags);
        AppendU64(file, r.data.size());
        file.insert(file.end(), r.data.begin(), r.data.end());
    }
    const uint64_t jumpAfter = file.size();

    PatchU64(file, jumpBeforeAt, jumpBefore);
    PatchU64(file, jumpAfterAt,  jumpAfter);
    return file;
}

// ── tests ──────────────────────────────────────────────────────────────────────

void TestHash () {
    // The published XXH64 vector for an empty input with seed 0. If this moves, the
    // implementation is not XXH64 any more and every shipped .staod is unreadable.
    CHECK(Hash64("", 0) == 0xEF46DB3751D8E999ull, "XXH64 empty-input vector");

    // The streaming form has to agree with the one-shot form at every chunk boundary —
    // that is where a hand-written accumulator gets it wrong.
    const std::vector<uint8_t> data = Filler(9999, 7, false);
    const uint64_t oneShot = Hash64(data.data(), data.size());
    for (size_t chunk : {1u, 7u, 31u, 32u, 33u, 64u, 1000u, 9999u}) {
        Hasher64 h;
        for (size_t at = 0; at < data.size(); at += chunk)
            h.Update(data.data() + at, std::min(chunk, data.size() - at));
        CHECK(h.Digest() == oneShot, "streaming XXH64 matches one-shot");
    }

    // Path canonicalisation drives asset identity, so case and slash style must not
    // produce two different IDs while the stored name keeps its case.
    CHECK(AssetIdFromPath("Textures\\Wall.DDS") == AssetIdFromPath("textures/wall.dds"),
          "asset ID ignores case and slash style");
    CHECK(NormalizePath("./Textures\\Wall.DDS") == "Textures/Wall.DDS",
          "NormalizePath keeps case");
}

void TestWisceneRoundTrip () {
    const std::vector<uint8_t> ecs = Filler(4096, 1, true);
    std::vector<FakeResource> resources = {
        { "textures/Wall.dds", 2, Filler(5000,  2, false) },
        { "textures/Floor.dds", 2, Filler(3000, 3, false) },
        { "scripts/Door.lua",  0, Filler(700,   4, true)  },
    };

    const std::vector<uint8_t> original = MakeFakeWiscene(ecs, resources);

    std::string error;
    WisceneSplit split;
    CHECK(SplitWiscene(original.data(), original.size(), split, &error),
          error.empty() ? "SplitWiscene" : error.c_str());
    CHECK(split.archiveVersion == 94, "archive version read back");
    CHECK(split.resources.size() == resources.size(), "all resources found");

    for (size_t i = 0; i < split.resources.size() && i < resources.size(); ++i) {
        CHECK(split.resources[i].name == resources[i].name, "resource name preserved");
        CHECK(split.resources[i].size == resources[i].data.size(), "resource size preserved");
        CHECK(std::memcmp(split.Bytes() + split.resources[i].offset,
                          resources[i].data.data(), resources[i].data.size()) == 0,
              "resource bytes preserved");
    }

    // The stripped archive has to still be a valid archive, and it has to be smaller by
    // exactly the resource block it lost.
    CHECK(split.ecsArchive.size() < original.size(), "stripped archive is smaller");

    std::vector<ResourceToEmbed> embed;
    for (const FakeResource& r : resources)
        embed.push_back(ResourceToEmbed{ r.name, r.flags, r.data.data(), r.data.size() });

    std::vector<uint8_t> rebuilt;
    CHECK(MergeWiscene(split.ecsArchive.data(), split.ecsArchive.size(), embed, rebuilt, &error),
          error.empty() ? "MergeWiscene" : error.c_str());
    CHECK(rebuilt.size() == original.size(), "rebuilt .wiscene has the original size");
    CHECK(rebuilt == original, "rebuilt .wiscene is byte-identical to the original");

    // A truncated file must be refused, not walked off the end of.
    WisceneSplit bad;
    CHECK(!SplitWiscene(original.data(), original.size() / 2, bad, nullptr),
          "truncated .wiscene is refused");
    CHECK(!SplitWiscene(original.data(), 4, bad, nullptr), "tiny input is refused");
}

void TestPackRoundTrip (const fs::path& dir) {
    PackOptions options;
    // A tiny part budget so the multi-part path is actually exercised: without this the
    // whole test fits in one part and the part-splitting logic is never run.
    options.partSizeTarget   = 256 * 1024;
    options.chunkSize        = 16 * 1024;
    options.compressionLevel = 3;

    struct Entry { std::string name; std::vector<uint8_t> data; };
    std::vector<Entry> entries = {
        { "textures/wall.dds",   Filler(120 * 1024, 11, false) },  // stored (already compressed)
        { "textures/floor.dds",  Filler(200 * 1024, 12, false) },  // stored, forces a second part
        { "meshes/crate.bin",    Filler(300 * 1024, 13, true)  },  // chunked, spans frames
        { "scripts/door.lua",    Filler(64,         14, true)  },  // below the compress threshold
        { "audio/step.wav",      Filler(90 * 1024,  15, true)  },
    };

    std::string error;
    {
        AssetPackWriter writer;
        CHECK(writer.Begin(dir.string(), "test", options, &error),
              error.empty() ? "writer.Begin" : error.c_str());
        for (const Entry& e : entries) {
            CHECK(writer.AddAuto(e.name, e.data.data(), e.data.size(), AssetFlag_None, &error),
                  error.empty() ? "writer.AddAuto" : error.c_str());
        }
        // A duplicate must be rejected rather than silently shadowing the first copy.
        CHECK(!writer.AddAuto(entries[0].name, entries[0].data.data(), entries[0].data.size(),
                              AssetFlag_None, nullptr),
              "duplicate logical path is rejected");
        CHECK(writer.Finish(&error), error.empty() ? "writer.Finish" : error.c_str());
        CHECK(writer.Stats().assetCount == entries.size(), "all assets recorded");
        CHECK(writer.Stats().partCount >= 2, "content spilled into more than one part");
    }

    AssetPack pack;
    CHECK(pack.Open((dir / "test.staod").string(), &error, /*verifyParts*/ true),
          error.empty() ? "pack.Open" : error.c_str());
    CHECK(pack.AssetCount() == entries.size(), "index holds every asset");

    for (const Entry& e : entries) {
        const StaodAsset* a = pack.Find(e.name);
        CHECK(a != nullptr, "lookup by path");
        if (a == nullptr) continue;

        CHECK(pack.NameString(*a) == e.name, "stored name round-trips");
        CHECK(a->originalSize == e.data.size(), "original size round-trips");

        std::vector<uint8_t> got;
        CHECK(pack.Read(*a, got, &error), error.empty() ? "pack.Read" : error.c_str());
        CHECK(got == e.data, "asset bytes round-trip");

        CHECK(pack.VerifyAsset(*a, &error), error.empty() ? "VerifyAsset" : error.c_str());

        // Ranged reads are what streaming does, and the interesting cases are the ones
        // that start and end inside a compression frame rather than on its boundary.
        if (e.data.size() > 40000) {
            const uint64_t offsets[] = { 0, 1, 15 * 1024, 16 * 1024, 16 * 1024 + 1, 33000 };
            for (uint64_t at : offsets) {
                std::vector<uint8_t> window;
                CHECK(pack.ReadRange(*a, at, 5000, window, &error),
                      error.empty() ? "pack.ReadRange" : error.c_str());
                CHECK(window.size() == 5000, "ranged read returns the requested length");
                CHECK(std::memcmp(window.data(), e.data.data() + at, window.size()) == 0,
                      "ranged read returns the right bytes");
            }
            // Past the end is an empty answer, not a failure — a plain file read behaves
            // the same way and the engine relies on it while walking mip chains.
            std::vector<uint8_t> tail;
            CHECK(pack.ReadRange(*a, e.data.size() + 10, 100, tail, &error), "read past end succeeds");
            CHECK(tail.empty(), "read past end is empty");

            std::vector<uint8_t> clamped;
            CHECK(pack.ReadRange(*a, e.data.size() - 10, 100, clamped, &error), "clamped read succeeds");
            CHECK(clamped.size() == 10, "clamped read is truncated to what exists");
        }
    }

    CHECK(pack.Find("no/such/asset.dds") == nullptr, "missing asset is a clean miss");
    CHECK(pack.Find("TEXTURES/WALL.DDS") != nullptr, "lookup is case-insensitive");
    CHECK(pack.VerifyAll(&error), error.empty() ? "VerifyAll" : error.c_str());

    // An already-compressed asset must stay stored, or the zero-copy streaming path is
    // gone — that is the property the whole codec-choice table exists to protect.
    if (const StaodAsset* dds = pack.Find("textures/wall.dds")) {
        CHECK(static_cast<Codec>(dds->codec) == Codec::None, "incompressible asset stays stored");
        CHECK(pack.MappedData(*dds) != nullptr, "stored asset is readable zero-copy");
        CHECK(pack.OwnsMappedPointer(pack.MappedData(*dds)), "mapped pointer is recognised as ours");
    }
    if (const StaodAsset* mesh = pack.Find("meshes/crate.bin")) {
        CHECK(static_cast<Codec>(mesh->codec) == Codec::ZstdChunked, "compressible asset is chunked");
        CHECK(mesh->storedSize < mesh->originalSize, "compression actually saved something");
        CHECK((mesh->flags & AssetFlag_Streamable) != 0, "chunked asset stays streamable");
    }

    pack.Close();

    // A part that does not belong to the index must be refused at Open(), not read.
    {
        const fs::path part = dir / "test.stafp1";
        std::error_code ec;
        const auto size = fs::file_size(part, ec);
        if (!ec && size > sizeof(StafpHeader) + 16) {
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            {
                std::ifstream in(part, std::ios::binary);
                in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            bytes[bytes.size() - 1] ^= 0xFF;   // flip a payload byte
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }

            AssetPack damaged;
            CHECK(damaged.Open((dir / "test.staod").string(), &error, /*verifyParts*/ true) == false,
                  "a damaged part is refused when verification is on");
        }
    }
}

void TestSceneDescriptor (const fs::path& dir) {
    const std::vector<uint8_t> ecs = Filler(200 * 1024, 21, true);
    std::vector<FakeResource> resources = {
        { "textures/Sky.dds", 2, Filler(70 * 1024, 22, false) },
        { "scripts/Main.lua", 0, Filler(2048,      23, true)  },
    };
    const std::vector<uint8_t> wiscene = MakeFakeWiscene(ecs, resources);

    const fs::path scenePath = dir / "map.wiscene";
    {
        std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
        CHECK(out.is_open(), "write the test .wiscene");
        out.write(reinterpret_cast<const char*>(wiscene.data()), static_cast<std::streamsize>(wiscene.size()));
    }

    std::string error;
    PackOptions options;
    options.compressionLevel = 3;

    AssetPackWriter writer;
    CHECK(writer.Begin((dir / "scene").string(), "map", options, &error),
          error.empty() ? "scene writer.Begin" : error.c_str());

    SceneDescriptor built;
    CHECK(BuildSceneDescriptor(scenePath.string(), "", &writer, built, &error),
          error.empty() ? "BuildSceneDescriptor" : error.c_str());
    CHECK(built.assets.size() == resources.size(), "every resource is referenced");
    CHECK(built.EcsArchive() != nullptr, "descriptor carries an entity payload");
    CHECK(writer.Finish(&error), error.empty() ? "scene writer.Finish" : error.c_str());

    const std::string stsdPath = (dir / "scene" / "map.stsd").string();
    SceneWriteOptions writeOptions;
    writeOptions.compressionLevel = 3;
    CHECK(WriteSceneDescriptor(stsdPath, built, writeOptions, &error),
          error.empty() ? "WriteSceneDescriptor" : error.c_str());

    // Header-only read: the cheap call the DevUI and the verifier make.
    SceneDescriptor meta;
    CHECK(ReadSceneDescriptor(stsdPath, meta, false, &error),
          error.empty() ? "ReadSceneDescriptor (metadata only)" : error.c_str());
    CHECK(meta.name == "map", "scene name round-trips");
    CHECK(meta.assets.size() == resources.size(), "asset list round-trips");
    CHECK(meta.archiveVersion == 94, "archive version round-trips");
    CHECK(meta.blobs.size() == 1 && meta.blobs[0].data.empty(),
          "metadata-only read leaves the blob unloaded");

    SceneDescriptor full;
    CHECK(ReadSceneDescriptor(stsdPath, full, true, &error),
          error.empty() ? "ReadSceneDescriptor (full)" : error.c_str());
    CHECK(full.EcsArchive() != nullptr && *full.EcsArchive() == *built.EcsArchive(),
          "entity payload survives the .stsd round trip");

    // And back to a .wiscene, which is the property that makes the format safe to adopt.
    AssetPack pack;
    CHECK(pack.Open((dir / "scene" / "map.staod").string(), &error),
          error.empty() ? "scene pack.Open" : error.c_str());

    std::vector<uint8_t> rebuilt;
    CHECK(RebuildWiscene(full, &pack, rebuilt, &error),
          error.empty() ? "RebuildWiscene" : error.c_str());
    CHECK(rebuilt.size() == wiscene.size(), "rebuilt map has the original size");
    CHECK(rebuilt == wiscene, "rebuilt map is byte-identical to the original .wiscene");
}

} // namespace

int main () {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "simtary_asset_pack_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("FAIL cannot create the temp directory %s\n", dir.string().c_str());
        return 1;
    }

    TestHash();
    TestWisceneRoundTrip();
    TestPackRoundTrip(dir);
    TestSceneDescriptor(dir);

    fs::remove_all(dir, ec);

    if (failures == 0) std::printf("asset_pack_test: all checks passed\n");
    else               std::printf("asset_pack_test: %d check(s) failed\n", failures);
    return failures;
}
