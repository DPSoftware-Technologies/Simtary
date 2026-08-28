// stpack — build and inspect Simtary asset packages.
//
// Runs during the CMake build (see simtary_pack_assets in cmake/SimtaryApp.cmake) and
// by hand. Links Framework/io/asset + Framework/io/Nbt.cpp + the vendored zstd, and
// nothing else — no engine, no graphics device — which is what lets it be a build step
// rather than something the game has to be running to do.
//
//   stpack pack   <contentDir> --out <dir> [--scene-dir <dir>] [--name content]
//                 [--part-size 50] [--chunk 256] [--level 9] [--stored] [--aggressive]
//   stpack unpack <index.strd> --out <dir> [--filter <substring>] [--rebuild-scenes]
//   stpack scene  <map.stsd> --out <map.wiscene> [--pack <index.strd>]
//   stpack info   <index.strd | map.stsd> [--assets]
//   stpack verify <index.strd>
//
// `pack` is the forward conversion: every .wiscene under contentDir is split into a
// .stsd plus its resources, every other file is added as it is, and the result is one
// .strd index next to N .stafp<N> parts. `scene` and `unpack --rebuild-scenes` are the
// reverse, and they exist because a format you cannot get back out of is a format
// nobody should adopt.
//
// Exit code is 0 on success, 1 on any failure, so CMake stops the build on a bad pack.

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

namespace {

bool g_quiet = false;

void Say (const std::string& text) {
    if (!g_quiet) std::printf("%s\n", text.c_str());
}

int Fail (const std::string& text) {
    std::fprintf(stderr, "stpack: %s\n", text.c_str());
    return 1;
}

fs::path U8Path (const std::string& s) { return fs::u8path(s); }

std::string LowerExtNoDot (const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

bool WriteWholeFile (const std::string& path, const void* data, uint64_t size, std::string* error) {
    std::error_code ec;
    const fs::path p = U8Path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { if (error) *error = "cannot write " + path; return false; }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) { if (error) *error = "write failed for " + path; return false; }
    return true;
}

bool ReadWholeFile (const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    std::error_code ec;
    const auto size = fs::file_size(U8Path(path), ec);
    if (ec) { if (error) *error = "cannot stat " + path; return false; }
    std::ifstream in(U8Path(path), std::ios::binary);
    if (!in.is_open()) { if (error) *error = "cannot open " + path; return false; }
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!in) { if (error) *error = "read failed for " + path; return false; }
    return true;
}

// ── argument parsing ───────────────────────────────────────────────────────────

struct Args {
    std::string              command;
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> options;   // --key [value]

    bool Has (const std::string& key) const {
        for (const auto& o : options) if (o.first == key) return true;
        return false;
    }
    std::string Get (const std::string& key, const std::string& def = std::string()) const {
        for (const auto& o : options) if (o.first == key) return o.second;
        return def;
    }
    uint64_t GetUint (const std::string& key, uint64_t def) const {
        const std::string v = Get(key);
        if (v.empty()) return def;
        return std::strtoull(v.c_str(), nullptr, 10);
    }
};

// Flags that stand alone; everything else takes the next token as its value.
bool IsBooleanFlag (const std::string& key) {
    return key == "stored" || key == "aggressive" ||
           key == "rebuild-scenes" || key == "assets" || key == "quiet" || key == "verify";
}

Args ParseArgs (int argc, char** argv) {
    Args args;
    if (argc > 1) args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            std::string key = a.substr(2);
            const size_t eq = key.find('=');
            if (eq != std::string::npos) {
                args.options.emplace_back(key.substr(0, eq), key.substr(eq + 1));
            } else if (IsBooleanFlag(key)) {
                args.options.emplace_back(key, "1");
            } else if (i + 1 < argc) {
                args.options.emplace_back(key, argv[++i]);
            } else {
                args.options.emplace_back(key, "1");
            }
        } else if (a == "-o" && i + 1 < argc) {
            args.options.emplace_back("out", argv[++i]);
        } else if (a == "-q") {
            args.options.emplace_back("quiet", "1");
        } else {
            args.positional.push_back(a);
        }
    }
    return args;
}

void PrintUsage () {
    std::printf(
        "stpack - build and inspect Simtary asset packages\n"
        "\n"
        "  stpack pack   <contentDir> --out <dir> [--scene-dir <dir>] [--name content]\n"
        "                [--part-size 50] [--chunk 256] [--level 9] [--stored] [--aggressive]\n"
        "  stpack unpack <index.strd> --out <dir> [--filter <substring>] [--rebuild-scenes]\n"
        "  stpack scene  <map.stsd>    --out <map.wiscene> [--pack <index.strd>]\n"
        "  stpack info   <index.strd | map.stsd> [--assets]\n"
        "  stpack verify <index.strd>\n"
        "\n"
        "  --part-size   megabytes per .stafp part (default 50, maximum 100)\n"
        "  --chunk       kilobytes per compression frame (default 256)\n"
        "  --level       zstd level 1..19 (default 9, matching wi::Archive)\n"
        "  --stored      store everything uncompressed\n"
        "  --scene-dir   write the generated .stsd maps here as loose files instead of\n"
        "                packing them. A leading \"scenes/\" is stripped, so\n"
        "                contents/scenes/x.wiscene -> <scene-dir>/x.stsd\n"
        "  -q            print nothing but errors\n");
}

// ── pack ───────────────────────────────────────────────────────────────────────

int CommandPack (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("pack needs a content directory"); }
    const std::string contentDir = args.positional[0];
    const std::string outDir     = args.Get("out");
    const std::string sceneDir   = args.Get("scene-dir");
    const std::string baseName   = args.Get("name", "content");
    if (outDir.empty()) return Fail("pack needs --out <dir>");

    std::error_code ec;
    if (!fs::is_directory(U8Path(contentDir), ec)) return Fail(contentDir + " is not a directory");

    PackOptions options;
    options.partSizeTarget    = args.GetUint("part-size", kDefaultPartSize / (1024 * 1024)) * 1024 * 1024;
    options.chunkSize         = static_cast<uint32_t>(args.GetUint("chunk", kDefaultChunkSize / 1024) * 1024);
    options.compressionLevel  = static_cast<int>(args.GetUint("level", 9));
    options.forceStored       = args.Has("stored");
    options.aggressive        = args.Has("aggressive");

    SceneWriteOptions sceneOptions;
    sceneOptions.blobCodec        = options.forceStored ? Codec::None : Codec::ZstdChunked;
    sceneOptions.chunkSize        = options.chunkSize;
    sceneOptions.compressionLevel = options.compressionLevel;

    std::string error;
    AssetPackWriter writer;
    if (!writer.Begin(outDir, baseName, options, &error)) return Fail(error);

    // Sorted so two builds of the same tree produce the same pack. Directory iteration
    // order is not specified, and an index that reshuffles every build defeats any
    // attempt at shipping a delta patch.
    const fs::path root = U8Path(contentDir);
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    uint32_t sceneCount = 0;
    uint64_t wisceneBytes = 0;

    // Scenes first. Converting a map registers its resources, so a later loose copy of
    // the same texture is recognised as a duplicate and skipped instead of stored twice.
    for (const fs::path& p : files) {
        if (LowerExtNoDot(p) != "wiscene") continue;

        const std::string full   = p.string();
        std::string        relDir = fs::relative(p.parent_path(), root, ec).generic_string();
        if (relDir == ".") relDir.clear();

        SceneDescriptor scene;
        // The resource prefix is empty on purpose: at runtime the .stsd's entity payload
        // is deserialised from MEMORY, so wi::Archive has no source directory and the
        // engine asks for exactly the relative names it stored.
        if (!BuildSceneDescriptor(full, "", &writer, scene, &error)) return Fail(error);

        std::vector<uint8_t> stsd;
        if (!SerializeSceneDescriptor(scene, stsd, sceneOptions, &error)) return Fail(error);

        if (sceneDir.empty()) {
            // No scene directory: the map goes into the package like everything else.
            const std::string logical =
                NormalizePath((relDir.empty() ? std::string() : relDir + "/") + scene.name + ".stsd");
            if (!writer.Add(logical, stsd.data(), stsd.size(), AssetType::Scene,
                            sceneOptions.blobCodec == Codec::None ? Codec::None : Codec::ZstdChunked,
                            AssetFlag_Generated, &error))
                return Fail(error);
            Say("  scene   " + logical + "  (" + std::to_string(scene.assets.size()) + " resources)");
        } else {
            // Maps stay loose, in their own folder, and only their RESOURCES go into the
            // package. A .stsd is a few KB of metadata next to a compressed entity blob,
            // so keeping it visible costs nothing and buys a lot: a map can be diffed,
            // listed and hand-swapped without unpacking anything.
            //
            // A leading "scenes/" is stripped because the scene directory already IS the
            // scenes folder — without this, contents/scenes/x.wiscene would land in
            // <scene-dir>/scenes/x.stsd. Anything deeper is preserved.
            std::string sub = relDir;
            if (sub == "scenes")                        sub.clear();
            else if (sub.rfind("scenes/", 0) == 0)      sub.erase(0, 7);

            const std::string outPath =
                sceneDir + "/" + (sub.empty() ? std::string() : sub + "/") + scene.name + ".stsd";
            if (!WriteWholeFile(outPath, stsd.data(), stsd.size(), &error)) return Fail(error);
            Say("  scene   " + outPath + "  (" + std::to_string(scene.assets.size()) + " resources)");
        }

        wisceneBytes += fs::file_size(p, ec);
        ++sceneCount;
    }

    // Then everything else. The .wiscene sources are skipped — they have been converted,
    // and shipping both would double the size for no gain.
    uint32_t fileCount = 0;
    for (const fs::path& p : files) {
        if (LowerExtNoDot(p) == "wiscene") continue;
        const std::string logical = NormalizePath(fs::relative(p, root, ec).generic_string());
        if (ec || logical.empty()) continue;
        if (writer.Contains(logical)) continue;   // already pulled in as a scene resource
        if (!writer.AddFile(logical, p.string(), AssetFlag_None, &error)) return Fail(error);
        ++fileCount;
    }

    if (!writer.Finish(&error)) return Fail(error);

    const PackStats& s = writer.Stats();
    Say("stpack: " + baseName + " -> " + std::to_string(s.partCount) + " part" +
        (s.partCount == 1 ? "" : "s") + ", " + std::to_string(s.assetCount) + " assets");
    Say("  scenes converted  " + std::to_string(sceneCount) +
        (wisceneBytes ? "  (" + FormatBytes(wisceneBytes) + " of .wiscene)" : ""));
    Say("  loose files       " + std::to_string(fileCount));
    Say("  payload           " + FormatBytes(s.originalBytes) + " -> " + FormatBytes(s.storedBytes) +
        (s.originalBytes ? "  (" + std::to_string(int(100.0 * double(s.storedBytes) /
                                                      double(s.originalBytes))) + "%)" : ""));
    Say("  index             " + FormatBytes(s.indexBytes));
    return 0;
}

// ── unpack ─────────────────────────────────────────────────────────────────────

int CommandUnpack (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("unpack needs an index file"); }
    const std::string indexPath = args.positional[0];
    const std::string outDir    = args.Get("out");
    const std::string filter    = args.Get("filter");
    if (outDir.empty()) return Fail("unpack needs --out <dir>");

    std::string error;
    AssetPack pack;
    if (!pack.Open(indexPath, &error)) return Fail(error);

    uint32_t written = 0, rebuilt = 0;
    for (uint32_t i = 0; i < pack.AssetCount(); ++i) {
        const StrdAsset* a = pack.AssetAt(i);
        const std::string name = pack.NameString(*a);
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;

        std::vector<uint8_t> bytes;
        if (!pack.Read(*a, bytes, &error)) return Fail(name + ": " + error);

        // A .stsd can come back out either as itself or as the .wiscene it was made
        // from. The second is what an artist wants: it opens in the editor.
        if (args.Has("rebuild-scenes") && static_cast<AssetType>(a->type) == AssetType::Scene) {
            SceneDescriptor scene;
            if (!ParseSceneDescriptor(bytes.data(), bytes.size(), scene, true, &error))
                return Fail(name + ": " + error);

            std::vector<uint8_t> wiscene;
            if (!RebuildWiscene(scene, &pack, wiscene, &error)) return Fail(name + ": " + error);

            std::string outName = name;
            const size_t dot = outName.find_last_of('.');
            if (dot != std::string::npos) outName.erase(dot);
            outName += ".wiscene";

            if (!WriteWholeFile(outDir + "/" + outName, wiscene.data(), wiscene.size(), &error))
                return Fail(error);
            Say("  scene   " + outName + "  " + FormatBytes(wiscene.size()));
            ++rebuilt;
            continue;
        }

        if (!WriteWholeFile(outDir + "/" + name, bytes.data(), bytes.size(), &error)) return Fail(error);
        ++written;
    }

    Say("stpack: extracted " + std::to_string(written) + " assets" +
        (rebuilt ? " and rebuilt " + std::to_string(rebuilt) + " scenes" : "") +
        " to " + outDir);
    return 0;
}

// ── scene (single-map reverse) ─────────────────────────────────────────────────

int CommandScene (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("scene needs a .stsd file"); }
    const std::string stsdPath = args.positional[0];
    const std::string outPath  = args.Get("out");
    const std::string packPath = args.Get("pack");
    if (outPath.empty()) return Fail("scene needs --out <map.wiscene>");

    std::string error;
    SceneDescriptor scene;
    if (!ReadSceneDescriptor(stsdPath, scene, true, &error)) return Fail(error);

    AssetPack pack;
    const AssetPack* packPtr = nullptr;
    if (!packPath.empty()) {
        if (!pack.Open(packPath, &error)) return Fail(error);
        packPtr = &pack;
        if (scene.packUuidLo != pack.UuidLo() || scene.packUuidHi != pack.UuidHi()) {
            // Not fatal — the assets may still all be there — but it is the single most
            // likely reason a rebuild comes out missing textures, so it gets said.
            Say("stpack: warning: " + stsdPath + " was converted against a different pack build");
        }
    } else if (!scene.assets.empty()) {
        Say("stpack: warning: no --pack given, so " + std::to_string(scene.assets.size()) +
            " resources will not be embedded");
    }

    std::vector<uint8_t> wiscene;
    if (!RebuildWiscene(scene, packPtr, wiscene, &error)) return Fail(error);
    if (!WriteWholeFile(outPath, wiscene.data(), wiscene.size(), &error)) return Fail(error);

    Say("stpack: " + outPath + "  " + FormatBytes(wiscene.size()) + "  (" +
        std::to_string(scene.assets.size()) + " resources embedded)");
    return 0;
}

// ── info ───────────────────────────────────────────────────────────────────────

int InfoScene (const std::string& path, const Args&) {
    std::string error;
    SceneDescriptor scene;
    if (!ReadSceneDescriptor(path, scene, false, &error)) return Fail(error);

    std::printf("%s\n", path.c_str());
    std::printf("  name            %s\n", scene.name.c_str());
    std::printf("  source          %s\n", scene.sourceFile.c_str());
    std::printf("  archive version %llu\n", static_cast<unsigned long long>(scene.archiveVersion));
    std::printf("  pack uuid       %016llx%016llx\n",
                static_cast<unsigned long long>(scene.packUuidHi),
                static_cast<unsigned long long>(scene.packUuidLo));
    std::printf("  blobs           %zu\n", scene.blobs.size());
    std::printf("  assets          %zu\n", scene.assets.size());
    for (const SceneAssetRef& a : scene.assets)
        std::printf("    %016llx  %s\n", static_cast<unsigned long long>(a.id), a.path.c_str());
    return 0;
}

int CommandInfo (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("info needs a file"); }
    const std::string path = args.positional[0];

    if (LowerExtNoDot(U8Path(path)) == "stsd") return InfoScene(path, args);

    std::string error;
    AssetPack pack;
    if (!pack.Open(path, &error)) return Fail(error);

    std::printf("%s\n", path.c_str());
    std::printf("  pack uuid       %016llx%016llx\n",
                static_cast<unsigned long long>(pack.UuidHi()),
                static_cast<unsigned long long>(pack.UuidLo()));
    std::printf("  assets          %u\n", pack.AssetCount());
    std::printf("  parts           %u\n", pack.PartCount());
    std::printf("  payload         %s\n", FormatBytes(pack.TotalPayloadSize()).c_str());

    for (uint32_t i = 0; i < pack.PartCount(); ++i) {
        const PartInfo p = pack.PartAt(i);
        std::printf("    %-24s %10s  %u assets  %016llx%s\n",
                    p.fileName.c_str(), FormatBytes(p.fileSize).c_str(), p.assetCount,
                    static_cast<unsigned long long>(p.fileHash),
                    (p.flags & PartFlag_Oversized) ? "  [oversized]" : "");
    }

    // Per-type roll-up: the number that actually tells you where a build's size went.
    struct Row { uint32_t count = 0; uint64_t original = 0; uint64_t stored = 0; };
    Row rows[32] = {};
    Row total;
    for (uint32_t i = 0; i < pack.AssetCount(); ++i) {
        const StrdAsset* a = pack.AssetAt(i);
        const uint32_t slot = a->type < 32 ? a->type : 0;
        rows[slot].count++;
        rows[slot].original += a->originalSize;
        rows[slot].stored   += a->storedSize;
        total.count++;
        total.original += a->originalSize;
        total.stored   += a->storedSize;
    }
    std::printf("  %-12s %7s %12s %12s\n", "type", "count", "original", "stored");
    for (uint32_t t = 0; t < 32; ++t) {
        if (rows[t].count == 0) continue;
        std::printf("  %-12s %7u %12s %12s\n", ToString(static_cast<AssetType>(t)), rows[t].count,
                    FormatBytes(rows[t].original).c_str(), FormatBytes(rows[t].stored).c_str());
    }
    std::printf("  %-12s %7u %12s %12s\n", "total", total.count,
                FormatBytes(total.original).c_str(), FormatBytes(total.stored).c_str());

    if (args.Has("assets")) {
        for (uint32_t i = 0; i < pack.AssetCount(); ++i) {
            const StrdAsset* a = pack.AssetAt(i);
            std::printf("  %016llx  part%-3u @%-12llu %10s %-13s %s\n",
                        static_cast<unsigned long long>(a->id),
                        pack.PartAt(a->partIndex).number,
                        static_cast<unsigned long long>(a->offset),
                        FormatBytes(a->originalSize).c_str(),
                        ToString(static_cast<Codec>(a->codec)),
                        pack.NameString(*a).c_str());
        }
    }
    return 0;
}

// ── verify ─────────────────────────────────────────────────────────────────────

int CommandVerify (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("verify needs an index file"); }
    const std::string path = args.positional[0];

    std::string error;
    AssetPack pack;
    if (!pack.Open(path, &error)) return Fail(error);

    // Part hashes first — one sequential read each, and a bad part explains every asset
    // failure that would follow it.
    for (uint32_t i = 0; i < pack.PartCount(); ++i) {
        if (!pack.VerifyPart(i, &error)) return Fail(error);
        Say("  ok  " + pack.PartAt(i).fileName);
    }

    uint32_t bad = 0;
    for (uint32_t i = 0; i < pack.AssetCount(); ++i) {
        const StrdAsset* a = pack.AssetAt(i);
        if (!pack.VerifyAsset(*a, &error)) {
            std::fprintf(stderr, "stpack: %s\n", error.c_str());
            ++bad;
        }
    }
    if (bad) return Fail(std::to_string(bad) + " of " + std::to_string(pack.AssetCount()) +
                         " assets failed their content hash");

    Say("stpack: " + path + " verified - " + std::to_string(pack.PartCount()) + " parts, " +
        std::to_string(pack.AssetCount()) + " assets");
    return 0;
}

} // namespace

int main (int argc, char** argv) {
    if (argc < 2) { PrintUsage(); return 1; }

    const Args args = ParseArgs(argc, argv);
    g_quiet = args.Has("quiet");

    if (args.command == "pack")   return CommandPack(args);
    if (args.command == "unpack") return CommandUnpack(args);
    if (args.command == "scene")  return CommandScene(args);
    if (args.command == "info")   return CommandInfo(args);
    if (args.command == "verify") return CommandVerify(args);
    if (args.command == "help" || args.command == "--help" || args.command == "-h") {
        PrintUsage();
        return 0;
    }

    PrintUsage();
    return Fail("unknown command \"" + args.command + "\"");
}
