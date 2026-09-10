// stpack - build and inspect Simtary asset packages.
//
// Runs during the CMake build (see simtary_pack_assets in cmake/SimtaryApp.cmake) and
// by hand. Links Framework/io/asset + Framework/io/Nbt.cpp + the vendored zstd, and
// nothing else - no engine, no graphics device - which is what lets it be a build step
// rather than something the game has to be running to do.
//
//   stpack pack   <contentDir> --out <dir> [--scene-dir <dir>] [--scene-src <dir>]
//                 [--resource-dir <dir>]... [--on-conflict override|add|keep]
//                 [--name content] [--part-size 50] [--chunk 256] [--level 9] [--stored]
//                 [--aggressive] [--strict]
//   stpack unpack <index.strd> --out <dir> [--filter <substring>] [--rebuild-scenes]
//   stpack scene  <map.stsd> --out <map.wiscene> [--pack <index.strd>]
//   stpack resources <map.wiscene | map.stsd> --out <dir> [--pack <index.strd>]...
//   stpack info   <index.strd | map.stsd> [--assets]
//   stpack verify <index.strd>
//
// `pack` is the forward conversion: every .wiscene under contentDir (and under
// --scene-src) is split into a .stsd plus its resources, every other file under
// contentDir is added as it is, and the result is one .strd index next to N .stafp<N>
// parts. --scene-src is what lets a project keep its maps OUT of the packed tree: a
// .wiscene is the SOURCE a .stsd is converted from, not an asset that ships, so
// assets/contents/ can mean "everything here goes into the package" with nothing to
// except out of it. `scene` and `unpack --rebuild-scenes` are the
// reverse, and they exist because a format you cannot get back out of is a format
// nobody should adopt.
//
// A .stsd found among the sources is NOT converted. It is already the converted form -
// there is nothing left to split - so it passes through byte for byte to --scene-dir
// (or into the package when there is no --scene-dir), and only its reference list is
// read, to report which of the resources it needs the package will not hold. That is
// what makes "drop a new .stsd in and rebuild" a swap of one map instead of a build
// that quietly ships a map with no textures.
//
// --resource-dir is the other half of that swap: a loose tree of the resource FILES a
// hand-placed map needs, merged into the package under the same relative names the map
// asks for. `resources` is what fills that tree - it writes out the files one map needs,
// either straight out of the .wiscene that embeds them or out of the package a .stsd was
// packed alongside, so moving a map between projects is copying two things instead of
// moving a 39 MB .wiscene. --on-conflict decides what happens when such a file has the same logical
// path as a resource a map already embedded:
//
//   override  the loose file wins - it is added first, and the map's embedded copy is
//             skipped. Every reference to that path, in every map, resolves to the new
//             bytes. This is the default: a resource tree exists to be authoritative.
//   add       both are kept - the loose file is packed under the first free
//             "<name>.<n>.<ext>". Existing objects keep pointing at the embedded copy;
//             the renamed asset is there to be pointed at deliberately, because the
//             names inside a map's entity blob cannot be rewritten from out here.
//   keep      the loose file is ignored. Nothing in the package changes.
//
// Exit code is 0 on success, 1 on any failure, so CMake stops the build on a bad pack.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

// argument parsing

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
    // For options that may be repeated - --resource-dir is given once per tree.
    std::vector<std::string> GetAll (const std::string& key) const {
        std::vector<std::string> all;
        for (const auto& o : options) if (o.first == key) all.push_back(o.second);
        return all;
    }
    uint64_t GetUint (const std::string& key, uint64_t def) const {
        const std::string v = Get(key);
        if (v.empty()) return def;
        return std::strtoull(v.c_str(), nullptr, 10);
    }
};

// Flags that stand alone; everything else takes the next token as its value.
bool IsBooleanFlag (const std::string& key) {
    return key == "stored" || key == "aggressive" || key == "strict" ||
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
        "  stpack pack   <contentDir> --out <dir> [--scene-dir <dir>] [--scene-src <dir>]\n"
        "                [--resource-dir <dir>]... [--on-conflict override|add|keep]\n"
        "                [--mirror-resources <dir>]\n"
        "                [--name content] [--part-size 50] [--chunk 256] [--level 9]\n"
        "                [--stored] [--aggressive] [--strict]\n"
        "  stpack unpack <index.strd> --out <dir> [--filter <substring>] [--rebuild-scenes]\n"
        "  stpack scene  <map.stsd>    --out <map.wiscene> [--pack <index.strd>]\n"
        "  stpack resources <map.wiscene | map.stsd> --out <dir>\n"
        "                  [--pack <index.strd>]...  (a .stsd needs --pack)\n"
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
        "  --scene-src   an EXTRA directory searched for .wiscene sources, for a project\n"
        "                that keeps its maps outside the packed content tree. Their\n"
        "                resources still go into the package; only the sources live\n"
        "                elsewhere. A .stsd found there passes through unconverted\n"
        "  --resource-dir a loose tree of resource FILES merged into the package under\n"
        "                their relative names - what a hand-placed .stsd needs to find.\n"
        "                May be given more than once\n"
        "  --on-conflict what to do when a --resource-dir file has the same logical path\n"
        "                as a resource a map already embedded:\n"
        "                  override  the loose file wins; every reference resolves to it\n"
        "                            (default)\n"
        "                  add       keep both; the loose file is packed as <name>.<n>.<ext>\n"
        "                  keep      ignore the loose file\n"
        "  --mirror-resources  write a loose copy of every resource a converted\n"
        "                .wiscene embeds into this directory, skipping the files\n"
        "                already there. Point it at --resource-dir and the project\n"
        "                keeps its own copy of every map resource, so a .wiscene\n"
        "                can later leave without taking its textures with it\n"
        "  --strict      fail the pack when a passed-through .stsd references a resource\n"
        "                the package does not hold, instead of warning\n"
        "  -q            print nothing but errors\n");
}

// pack

// What to do when a file under --resource-dir carries the same logical path as
// something already in the package - all but always a resource a map embedded.
enum class Conflict { Override, Add, Keep };

const char* ToString (Conflict c) {
    switch (c) {
        case Conflict::Add:  return "add";
        case Conflict::Keep: return "keep";
        default:             return "override";
    }
}

bool ParseConflict (const std::string& text, Conflict& out) {
    if (text.empty() || text == "override" || text == "replace") { out = Conflict::Override; return true; }
    if (text == "add"  || text == "rename")                      { out = Conflict::Add;      return true; }
    if (text == "keep" || text == "skip")                        { out = Conflict::Keep;     return true; }
    return false;
}

// Every regular file under `root`, sorted, so two builds of the same tree produce the
// same pack. Directory iteration order is not specified, and an index that reshuffles
// every build defeats any attempt at shipping a delta patch.
void ListFilesSorted (const fs::path& root, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
}

// "textures/wall.dds" -> "textures/wall.1.dds", the first suffix nothing answers to.
// Conflict::Add keeps both copies, and two assets cannot share a logical path.
std::string FreeName (const AssetPackWriter& writer, const std::string& logical) {
    const size_t slash  = logical.find_last_of('/');
    const size_t dot    = logical.find_last_of('.');
    const bool   hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string stem = hasExt ? logical.substr(0, dot) : logical;
    const std::string ext  = hasExt ? logical.substr(dot)    : std::string();
    for (int n = 1; n < 10000; ++n) {
        const std::string candidate = NormalizePath(stem + "." + std::to_string(n) + ext);
        if (!writer.Contains(candidate)) return candidate;
    }
    return {};
}

// A map that arrived already converted, kept aside so its references can be checked
// once the whole package is known - not while it is still half built.
struct PassedMap {
    std::string     where;    // where it ended up, for the report
    SceneDescriptor scene;
};

int CommandPack (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("pack needs a content directory"); }
    const std::string contentDir = args.positional[0];
    const std::string outDir     = args.Get("out");
    const std::string sceneDir   = args.Get("scene-dir");
    const std::string sceneSrc   = args.Get("scene-src");
    const std::string baseName   = args.Get("name", "content");
    const std::vector<std::string> resourceDirs = args.GetAll("resource-dir");
    const std::string mirrorDir = args.Get("mirror-resources");
    if (outDir.empty()) return Fail("pack needs --out <dir>");

    Conflict conflict = Conflict::Override;
    if (!ParseConflict(args.Get("on-conflict"), conflict))
        return Fail("--on-conflict takes override, add or keep");

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

    const fs::path root = U8Path(contentDir);
    std::vector<fs::path> files;
    ListFilesSorted(root, files);

    uint32_t sceneCount   = 0;
    uint64_t wisceneBytes = 0;

    // The maps, each paired with the root its relative path is measured against: the
    // content tree, plus --scene-src for a project that keeps its maps outside it. Both
    // end up in the same package, so where the SOURCE sits changes nothing.
    //
    // .wiscene and .stsd are sorted apart here because they are not two spellings of the
    // same input. A .wiscene is a source to convert; a .stsd is that conversion's OUTPUT,
    // and running the converter over one is not possible - there is no resource block
    // left in it to lift out. It passes through instead.
    std::vector<std::pair<fs::path, fs::path>> sceneFiles;   // .wiscene
    std::vector<std::pair<fs::path, fs::path>> sceneMaps;    // .stsd
    for (const fs::path& p : files) {
        const std::string ext = LowerExtNoDot(p);
        if      (ext == "wiscene") sceneFiles.emplace_back(p, root);
        else if (ext == "stsd")    sceneMaps.emplace_back(p, root);
    }
    if (!sceneSrc.empty()) {
        const fs::path sceneRoot = U8Path(sceneSrc);
        if (!fs::is_directory(sceneRoot, ec)) return Fail(sceneSrc + " is not a directory");

        std::vector<fs::path> found;
        ListFilesSorted(sceneRoot, found);
        for (const fs::path& p : found) {
            const std::string ext = LowerExtNoDot(p);
            if      (ext == "wiscene") sceneFiles.emplace_back(p, sceneRoot);
            else if (ext == "stsd")    sceneMaps.emplace_back(p, sceneRoot);
        }
    }

    // The loose resource trees: resource FILES under the same relative names the maps
    // ask for. This is the other half of hand-swapping a .stsd, whose resources are not
    // inside it and have to come from somewhere.
    std::vector<std::pair<fs::path, fs::path>> resourceFiles;
    for (const std::string& dir : resourceDirs) {
        const fs::path resRoot = U8Path(dir);
        if (!fs::is_directory(resRoot, ec)) return Fail(dir + " is not a directory");
        std::vector<fs::path> found;
        ListFilesSorted(resRoot, found);
        for (const fs::path& p : found) resourceFiles.emplace_back(p, resRoot);
    }

    uint32_t resourceCount = 0, resourceRenamed = 0, resourceSkipped = 0;
    bool     resourceFailed = false;

    // Called once, either before the maps or after them - and that ordering IS the
    // conflict policy. Going first means the writer already holds the path when a map
    // offers its embedded copy, so the existing skip-if-present rule drops the copy and
    // every reference to that path resolves to the loose bytes, in every map, without
    // rewriting anything. Going last means the map's copy is the one in the way.
    auto AddResourceTrees = [&] () {
        for (const auto& entry : resourceFiles) {
            std::error_code rel;
            const std::string logical =
                NormalizePath(fs::relative(entry.first, entry.second, rel).generic_string());
            if (rel || logical.empty()) continue;

            std::string target = logical;
            if (writer.Contains(logical)) {
                if (conflict == Conflict::Keep) { ++resourceSkipped; continue; }
                if (conflict == Conflict::Add) {
                    target = FreeName(writer, logical);
                    if (target.empty()) {
                        error          = logical + ": no free name left for --on-conflict add";
                        resourceFailed = true;
                        return;
                    }
                    ++resourceRenamed;
                    Say("  resource  " + logical + " -> " + target + "  (both kept)");
                } else {
                    // Override, and the path is taken already: a second resource tree, or
                    // the same file twice. First one wins, or the result would depend on
                    // the order the directories were listed in.
                    ++resourceSkipped;
                    continue;
                }
            }
            if (!writer.AddFile(target, entry.first.string(), AssetFlag_None, &error)) {
                resourceFailed = true;
                return;
            }
            ++resourceCount;
        }
    };

    if (conflict == Conflict::Override) {
        AddResourceTrees();
        if (resourceFailed) return Fail(error);
    }

    // Scenes next. Converting a map registers its resources, so a later loose copy of
    // the same texture is recognised as a duplicate and skipped instead of stored twice.
    for (const auto& entry : sceneFiles) {
        const fs::path& p         = entry.first;
        const fs::path& sceneRoot = entry.second;

        const std::string full   = p.string();
        std::string        relDir = fs::relative(p.parent_path(), sceneRoot, ec).generic_string();
        if (relDir == ".") relDir.clear();

        SceneDescriptor scene;
        // The resource prefix is empty on purpose: at runtime the .stsd's entity payload
        // is deserialised from MEMORY, so wi::Archive has no source directory and the
        // engine asks for exactly the relative names it stored.
        if (!BuildSceneDescriptor(full, "", &writer, scene, &error)) return Fail(error);

        // Keep a loose copy of everything this map embeds, next to the project's other
        // resources.
        //
        //	The resources of a .wiscene exist in exactly ONE place: inside that file. Move
        //	it out of the project - archive it, hand it to someone, replace it with the
        //	.stsd the editor saved - and the next pack has nothing to pack, the map ships
        //	naming textures no package holds, and the world comes up white. Mirroring here
        //	means the project keeps its own copy from the first build onwards, so the map
        //	source becomes something that can leave.
        //
        //	A file that is already there is never rewritten: it may be a deliberate
        //	override (that is what --on-conflict override is for), and rewriting 33 MB of
        //	textures every build to produce the bytes that are already on disk is time
        //	spent for nothing. The descriptor lists the names, so the common case - all
        //	present - costs a handful of stat() calls and the .wiscene is not touched again.
        if (!mirrorDir.empty()) {
            std::vector<std::string> missing;
            for (const SceneAssetRef& ref : scene.assets) {
                std::error_code stat;
                if (!fs::exists(U8Path(mirrorDir + "/" + ref.path), stat))
                    missing.push_back(ref.path);
            }
            if (!missing.empty()) {
                std::vector<uint8_t> source;
                if (!ReadWholeFile(full, source, &error)) return Fail(error);

                WisceneSplit split;
                if (!SplitWiscene(source.data(), source.size(), split, &error))
                    return Fail(full + ": " + error);

                //	The write rule itself lives in ExportSceneResources, because the editor
                //	does the same thing on save for the resources only it can see - the
                //	ones an in-editor model import brought in. `missing` above stays a
                //	cheap pre-check: it is what decides whether this map's .wiscene has to
                //	be read at all, and the export skips what is already there anyway.
                ResourceExport exported;
                if (!ExportSceneResources(split, mirrorDir, &exported, &error))
                    return Fail(error);
                if (exported.written > 0)
                    Say("  mirror  " + std::to_string(exported.written) + " resource(s) of " +
                        scene.name + " -> " + mirrorDir + "  " + FormatBytes(exported.bytes));
                if (exported.rejected > 0)
                    Say("  mirror  " + std::to_string(exported.rejected) + " resource name(s) of " +
                        scene.name + " refused: they point outside " + mirrorDir);
                if (exported.empty > 0)
                    Say("  mirror  " + std::to_string(exported.empty) + " resource(s) of " +
                        scene.name + " carry no bytes and were not written");
            }
        }

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
            // scenes folder - without this, contents/scenes/x.wiscene would land in
            // <scene-dir>/scenes/x.stsd. Anything deeper is preserved. A --scene-src root
            // IS the scenes folder, so its files have no prefix to strip to begin with.
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

    // Maps that arrive already converted. Nothing is converted and nothing is
    // re-serialised - the bytes are copied through exactly as they came, because a .stsd
    // out of the editor or another build is the authored artefact, and rewriting it here
    // would hand the author back a file they did not write. Its reference list is read
    // for one reason only: to report, further down, what it needs and will not find.
    std::vector<PassedMap> passed;
    for (const auto& entry : sceneMaps) {
        const fs::path& p         = entry.first;
        const fs::path& sceneRoot = entry.second;

        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(p.string(), bytes, &error)) return Fail(error);

        PassedMap map;
        if (!ParseSceneDescriptor(bytes.data(), bytes.size(), map.scene, false, &error))
            return Fail(p.string() + ": " + error);

        std::string relDir = fs::relative(p.parent_path(), sceneRoot, ec).generic_string();
        if (relDir == ".") relDir.clear();

        const std::string name = p.filename().string();
        if (sceneDir.empty()) {
            const std::string logical =
                NormalizePath((relDir.empty() ? std::string() : relDir + "/") + name);
            if (writer.Contains(logical)) {
                Say("  map     " + logical + "  (already in the package, skipped)");
                continue;
            }
            // Codec::None: the entity blob inside is compressed already, and a second
            // pass over it buys nothing but build time.
            if (!writer.Add(logical, bytes.data(), bytes.size(), AssetType::Scene,
                            Codec::None, AssetFlag_None, &error))
                return Fail(error);
            map.where = logical;
        } else {
            std::string sub = relDir;
            if (sub == "scenes")                   sub.clear();
            else if (sub.rfind("scenes/", 0) == 0) sub.erase(0, 7);

            const std::string outPath =
                sceneDir + "/" + (sub.empty() ? std::string() : sub + "/") + name;
            // Copying a file onto itself truncates it mid-read, which is what would
            // happen on a project whose scene folder IS the scene output folder.
            std::error_code same;
            if (!fs::equivalent(p, U8Path(outPath), same)) {
                if (!WriteWholeFile(outPath, bytes.data(), bytes.size(), &error)) return Fail(error);
            }
            map.where = outPath;
        }
        Say("  map     " + map.where + "  (passed through, " +
            std::to_string(map.scene.assets.size()) + " resources)");
        passed.push_back(std::move(map));
    }

    if (conflict != Conflict::Override) {
        AddResourceTrees();
        if (resourceFailed) return Fail(error);
    }

    // Then everything else. The .wiscene sources are skipped - they have been converted,
    // and shipping both would double the size for no gain. The .stsd maps are skipped
    // too: they went through above, as maps rather than as files.
    uint32_t fileCount = 0;
    for (const fs::path& p : files) {
        const std::string ext = LowerExtNoDot(p);
        if (ext == "wiscene" || ext == "stsd") continue;
        const std::string logical = NormalizePath(fs::relative(p, root, ec).generic_string());
        if (ec || logical.empty()) continue;
        if (writer.Contains(logical)) continue;   // already pulled in as a scene resource
        if (!writer.AddFile(logical, p.string(), AssetFlag_None, &error)) return Fail(error);
        ++fileCount;
    }

    // What a passed-through map asks for that the package will not hold. This is the
    // whole reason the pass-through reads a reference list at all: a hand-swapped map
    // whose resources nobody supplied loads into a world with no textures, and finding
    // that out at build time beats finding it out in the game.
    uint32_t missingTotal = 0;
    for (const PassedMap& map : passed) {
        std::vector<std::string> missing;
        for (const SceneAssetRef& ref : map.scene.assets)
            if (!writer.Contains(ref.path)) missing.push_back(ref.path);
        if (missing.empty()) continue;

        missingTotal += static_cast<uint32_t>(missing.size());
        std::fprintf(stderr, "stpack: %s needs %zu resource%s the package does not have:\n",
                     map.where.c_str(), missing.size(), missing.size() == 1 ? "" : "s");
        for (size_t i = 0; i < missing.size() && i < 8; ++i)
            std::fprintf(stderr, "          %s\n", missing[i].c_str());
        if (missing.size() > 8)
            std::fprintf(stderr, "          ... and %zu more\n", missing.size() - 8);
        std::fprintf(stderr, "        put them under --resource-dir, or pack the .wiscene they came from\n");
    }
    if (missingTotal && args.Has("strict")) {
        writer.Abort();
        return Fail(std::to_string(missingTotal) + " scene resources are missing from the package");
    }

    if (!writer.Finish(&error)) return Fail(error);

    const PackStats& s = writer.Stats();
    Say("stpack: " + baseName + " -> " + std::to_string(s.partCount) + " part" +
        (s.partCount == 1 ? "" : "s") + ", " + std::to_string(s.assetCount) + " assets");
    Say("  scenes converted  " + std::to_string(sceneCount) +
        (wisceneBytes ? "  (" + FormatBytes(wisceneBytes) + " of .wiscene)" : ""));
    if (!passed.empty())
        Say("  maps passed       " + std::to_string(passed.size()) + "  (.stsd, unconverted)" +
            (missingTotal ? "  - " + std::to_string(missingTotal) + " resources MISSING" : ""));
    if (!resourceFiles.empty())
        Say("  resources merged  " + std::to_string(resourceCount) +
            "  (" + std::string(ToString(conflict)) + ": " +
            std::to_string(resourceRenamed) + " renamed, " +
            std::to_string(resourceSkipped) + " skipped)");
    Say("  loose files       " + std::to_string(fileCount));
    Say("  payload           " + FormatBytes(s.originalBytes) + " -> " + FormatBytes(s.storedBytes) +
        (s.originalBytes ? "  (" + std::to_string(int(100.0 * double(s.storedBytes) /
                                                      double(s.originalBytes))) + "%)" : ""));
    Say("  index             " + FormatBytes(s.indexBytes));
    return 0;
}

// unpack

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

// scene (single-map reverse)

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
        const bool bound = scene.packUuidLo != 0 || scene.packUuidHi != 0;
        if (bound && (scene.packUuidLo != pack.UuidLo() || scene.packUuidHi != pack.UuidHi())) {
            // Not fatal - the assets may still all be there - but it is the single most
            // likely reason a rebuild comes out missing textures, so it gets said.
            Say("stpack: warning: " + stsdPath + " was converted against a different pack build");
        }
        // A zero UUID is not a mismatch: it means the map was never bound to one
        // particular package build, which is what an editor save is when every resource
        // it references already lived in whatever was mounted at the time.
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

// resources (materialise one map's resources as loose files)

// The companion to the pass-through. A .stsd carries no resource bytes - it names them,
// and the bytes live in the package it was converted alongside. So a map copied out of
// one project into another references a set of files the new project has never seen,
// and the swap ships a world with no textures.
//
// This writes exactly the files that map names, under exactly the names it uses, which
// is the layout --resource-dir expects. Copy the map into assets/scenes/, fill
// assets/resources/ from here, and the map is portable: its .wiscene source never has to
// stay in the project, and the next pack merges the resources back in.
//
// Two sources, because both are things a person actually has:
//
//   a .wiscene   the resources are INSIDE it. Nothing else is needed - this is the one
//                to use when the map's source is on hand, including the case of moving
//                a .wiscene out of a project and wanting its textures to stay behind
//   a .stsd      the map names its resources but does not carry them, so --pack says
//                which package to lift them out of
int CommandResources (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("resources needs a .stsd or .wiscene"); }
    const std::string mapPath = args.positional[0];
    const std::string outDir  = args.Get("out");
    const std::vector<std::string> packPaths = args.GetAll("pack");
    if (outDir.empty()) return Fail("resources needs --out <dir>");

    std::string error;

    // A .wiscene is self-contained: split it and write the resource block out as files.
    if (LowerExtNoDot(U8Path(mapPath)) == "wiscene") {
        std::vector<uint8_t> source;
        if (!ReadWholeFile(mapPath, source, &error)) return Fail(error);

        WisceneSplit split;
        if (!SplitWiscene(source.data(), source.size(), split, &error))
            return Fail(U8Path(mapPath).filename().string() + ": " + error);

        uint64_t bytes = 0;
        for (const EmbeddedResource& r : split.resources) {
            const std::string logical = NormalizePath(r.name);
            if (logical.empty()) continue;
            if (!WriteWholeFile(outDir + "/" + logical, split.Bytes() + r.offset, r.size, &error))
                return Fail(error);
            bytes += r.size;
            Say("  " + logical + "  " + FormatBytes(r.size));
        }
        Say("stpack: " + std::to_string(split.resources.size()) + " resources -> " + outDir +
            "  " + FormatBytes(bytes));
        return 0;
    }

    if (packPaths.empty())
        return Fail("resources needs --pack <index.strd> for a .stsd (a .wiscene carries its own)");
    SceneDescriptor scene;
    if (!ReadSceneDescriptor(mapPath, scene, false, &error)) return Fail(error);

    // Several packages are allowed because a game can ship a base package and patches
    // over it, and a map may name resources from either. First one holding the id wins,
    // which is the order they were given on the command line.
    std::vector<std::unique_ptr<AssetPack>> packs;
    for (const std::string& path : packPaths) {
        auto pack = std::unique_ptr<AssetPack>(new AssetPack());
        if (!pack->Open(path, &error)) return Fail(error);
        packs.push_back(std::move(pack));
    }

    uint32_t written = 0, missing = 0;
    uint64_t bytes   = 0;
    for (const SceneAssetRef& ref : scene.assets) {
        const AssetPack* from  = nullptr;
        const StrdAsset* found = nullptr;
        for (const auto& pack : packs) {
            found = pack->Find(ref.id);
            if (found != nullptr) { from = pack.get(); break; }
        }
        if (found == nullptr) {
            std::fprintf(stderr, "stpack: %s is in no given package\n", ref.path.c_str());
            ++missing;
            continue;
        }

        std::vector<uint8_t> data;
        if (!from->Read(*found, data, &error)) return Fail(error);
        if (!WriteWholeFile(outDir + "/" + ref.path, data.data(), data.size(), &error))
            return Fail(error);
        ++written;
        bytes += data.size();
        Say("  " + ref.path + "  " + FormatBytes(data.size()));
    }

    Say("stpack: " + std::to_string(written) + " of " + std::to_string(scene.assets.size()) +
        " resources -> " + outDir + "  " + FormatBytes(bytes));
    if (missing)
        return Fail(std::to_string(missing) + " resource(s) were in none of the packages given");
    return 0;
}

// info

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

// verify

int CommandVerify (const Args& args) {
    if (args.positional.empty()) { PrintUsage(); return Fail("verify needs an index file"); }
    const std::string path = args.positional[0];

    std::string error;
    AssetPack pack;
    if (!pack.Open(path, &error)) return Fail(error);

    // Part hashes first - one sequential read each, and a bad part explains every asset
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
    if (args.command == "resources") return CommandResources(args);
    if (args.command == "info")   return CommandInfo(args);
    if (args.command == "verify") return CommandVerify(args);
    if (args.command == "help" || args.command == "--help" || args.command == "-h") {
        PrintUsage();
        return 0;
    }

    PrintUsage();
    return Fail("unknown command \"" + args.command + "\"");
}
