#include "dev/DevBuild.h"

#include "wiBacklog.h"
#include "wiTimer.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// Everything the build baked in. simtary_add_app defines these on the framework target
// only when SIMTARY_DEV_BUILD is on (cmake/SimtaryApp.cmake, "dev build").
#ifndef ST_DEV_BUILD
#define ST_DEV_BUILD 0
#endif
#ifndef ST_DEV_ASSETS_DIR
#define ST_DEV_ASSETS_DIR ""
#endif
#ifndef ST_DEV_SCENE_SUBDIR
#define ST_DEV_SCENE_SUBDIR "scenes"
#endif
#ifndef ST_DEV_PACK
#define ST_DEV_PACK 0
#endif
#ifndef ST_DEV_STPACK
#define ST_DEV_STPACK ""
#endif
#ifndef ST_DEV_PACK_NAME
#define ST_DEV_PACK_NAME "content"
#endif
#ifndef ST_DEV_PACK_PART_SIZE
#define ST_DEV_PACK_PART_SIZE 50
#endif
#ifndef ST_DEV_PACK_ON_CONFLICT
#define ST_DEV_PACK_ON_CONFLICT ""
#endif
#ifndef ST_PROJECT_CONTENT_DIR
#define ST_PROJECT_CONTENT_DIR ""
#endif
#ifndef ST_PROJECT_RESOURCE_DIR
#define ST_PROJECT_RESOURCE_DIR ""
#endif

namespace st::dev {

namespace {

// zstd level for the launch sync. The build packs at 9; a sync runs while someone waits for
// the game to open, and level 3 packs several times faster for a few percent more bytes.
constexpr int kSyncLevel = 3;
// Where the sync writes, relative to the working directory (= the exe's folder, which is
// where the game resolves "assets/..." from).
constexpr const char* kPackSubdir = "assets/resources";
constexpr const char* kStampFile  = "assets/.devsync";
constexpr const char* kLogFile    = "devsync.log";

SyncReport g_last;

fs::path U8 (const std::string& s) { return fs::u8path(s); }

uint64_t Fnv (uint64_t h, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool HasExtension (const fs::path& p, const char* const* exts) {
    if (exts == nullptr) return true;
    std::string e = p.extension().string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    for (const char* const* x = exts; *x; ++x)
        if (e == *x) return true;
    return false;
}

// Fold every file under `root` (path, size, modification time) into `h`. Any added,
// removed, renamed or edited file changes the result - which a "newest timestamp" check
// would miss for a deletion, or for a file restored from an older copy.
void HashTree (const std::string& root, const char* const* exts, uint64_t& h, size_t& files) {
    if (root.empty()) return;
    std::error_code ec;
    const fs::path base = U8(root);
    if (!fs::is_directory(base, ec)) return;
    for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !HasExtension(it->path(), exts)) continue;
        const std::string rel = it->path().lexically_relative(base).generic_u8string();
        const uint64_t size = (uint64_t)it->file_size(ec);
        const auto time = it->last_write_time(ec).time_since_epoch().count();
        h = Fnv(h, rel.data(), rel.size());
        h = Fnv(h, &size, sizeof(size));
        h = Fnv(h, &time, sizeof(time));
        ++files;
    }
}

const char* const kSceneExts[] = { ".wiscene", ".stsd", nullptr };

uint64_t SourceHash (size_t& files) {
    uint64_t h = 1469598103934665603ull;
    files = 0;
    HashTree(SourceContentDir(), nullptr, h, files);
    HashTree(SourceSceneDir(), kSceneExts, h, files);
    HashTree(SourceResourceDir(), nullptr, h, files);
    // A rebuilt packer, or different pack settings, invalidate the output as surely as
    // an edited asset does.
    std::error_code ec;
    const std::string stpack = ST_DEV_STPACK;
    if (!stpack.empty()) {
        const auto t = fs::last_write_time(U8(stpack), ec).time_since_epoch().count();
        h = Fnv(h, &t, sizeof(t));
    }
    const std::string settings = std::string(ST_DEV_PACK_NAME) + "|" + std::to_string(ST_DEV_PACK_PART_SIZE) +
                                 "|" + ST_DEV_PACK_ON_CONFLICT + "|" + std::to_string(kSyncLevel);
    return Fnv(h, settings.data(), settings.size());
}

std::string ReadStamp () {
    std::ifstream in(U8(kStampFile));
    std::string s;
    std::getline(in, s);
    return s;
}

void WriteStamp (uint64_t hash) {
    std::error_code ec;
    fs::create_directories(U8(kStampFile).parent_path(), ec);
    std::ofstream out(U8(kStampFile), std::ios::trunc);
    out << std::hex << hash << "\n";
}

std::string Hex (uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)v);
    return buf;
}

// Run a tool and wait for it, its output going to `logPath`. Returns the exit code, or
// -1 when it could not be started.
int RunTool (const std::string& exe, const std::vector<std::string>& args, const std::string& logPath) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    };
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    for (const std::string& a : args) cmd += L" \"" + widen(a) + L"\"";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW(widen(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
    }
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    if (!started) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return int(code);
#else
    std::string cmd = "'" + exe + "'";
    for (const std::string& a : args) cmd += " '" + a + "'";
    cmd += " > '" + logPath + "' 2>&1";
    const int status = std::system(cmd.c_str());
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

// Copy mode, for a project that does not pack: the build copies assets/contents/ into
// <exe>/assets and the maps beside it, so the sync does the same for whatever changed.
size_t CopyNewer (const std::string& from, const std::string& to, const char* const* exts) {
    size_t copied = 0;
    if (from.empty()) return 0;
    std::error_code ec;
    const fs::path base = U8(from);
    if (!fs::is_directory(base, ec)) return 0;
    for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !HasExtension(it->path(), exts)) continue;
        const fs::path dst = U8(to) / it->path().lexically_relative(base);
        std::error_code e2;
        if (fs::exists(dst, e2) && fs::file_size(dst, e2) == it->file_size(e2) &&
            fs::last_write_time(dst, e2) >= it->last_write_time(e2))
            continue;
        fs::create_directories(dst.parent_path(), e2);
        if (fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, e2)) ++copied;
    }
    return copied;
}

bool StartsWith (const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string Lower (std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

bool IsDevBuild () { return ST_DEV_BUILD != 0; }

std::string SourceAssetsDir ()  { return IsDevBuild() ? std::string(ST_DEV_ASSETS_DIR) : std::string(); }
std::string SourceContentDir () { return IsDevBuild() ? std::string(ST_PROJECT_CONTENT_DIR) : std::string(); }
std::string SourceResourceDir () { return IsDevBuild() ? std::string(ST_PROJECT_RESOURCE_DIR) : std::string(); }
std::string SourceSceneDir () {
    if (!IsDevBuild() || SourceAssetsDir().empty()) return std::string();
    // Named even when the folder does not exist yet: the first map saved creates it.
    return SourceAssetsDir() + "/" + ST_DEV_SCENE_SUBDIR;
}

const SyncReport& LastSync () { return g_last; }

SyncReport SyncAssets (bool force, const std::function<void(const std::string&)>& status) {
    SyncReport r;
    if (!IsDevBuild()) {
        r.message = "not a dev build";
        return g_last = r;
    }
    wi::Timer timer;
    auto say = [&](const std::string& s) { if (status) status(s); };

    say("Checking source assets");
    const uint64_t hash = SourceHash(r.files);
    const bool packing = ST_DEV_PACK != 0 && std::string(ST_DEV_STPACK).size() > 0;
    std::error_code ec;
    const std::string packIndex = std::string(kPackSubdir) + "/" + ST_DEV_PACK_NAME + ".strd";
    const bool outputMissing = packing && !fs::exists(U8(packIndex), ec);

    if (!force && !outputMissing && ReadStamp() == Hex(hash)) {
        r.seconds = timer.elapsed_seconds();
        r.message = "assets up to date (" + std::to_string(r.files) + " source files)";
        return g_last = r;
    }

    r.ran = true;
    if (packing) {
        say("Converting assets (stpack)");
        const std::string sceneOut = std::string("assets/") + ST_DEV_SCENE_SUBDIR;
        fs::create_directories(U8(kPackSubdir), ec);
        fs::create_directories(U8(sceneOut), ec);

        // The same conversion simtary_pack_assets runs at build time (cmake/SimtaryApp.cmake),
        // so a synced game loads exactly what a built one would.
        std::vector<std::string> args = { "pack", SourceContentDir(), "--out", kPackSubdir,
                                          "--scene-dir", sceneOut };
        std::error_code e2;
        if (fs::is_directory(U8(SourceSceneDir()), e2)) {
            args.push_back("--scene-src");
            args.push_back(SourceSceneDir());
        }
        if (!SourceResourceDir().empty()) {
            args.push_back("--resource-dir");
            args.push_back(SourceResourceDir());
            if (std::string(ST_DEV_PACK_ON_CONFLICT).size() > 0) {
                args.push_back("--on-conflict");
                args.push_back(ST_DEV_PACK_ON_CONFLICT);
            }
            args.push_back("--mirror-resources");
            args.push_back(SourceResourceDir());
        }
        args.push_back("--name");      args.push_back(ST_DEV_PACK_NAME);
        args.push_back("--part-size"); args.push_back(std::to_string(ST_DEV_PACK_PART_SIZE));
        args.push_back("--level");     args.push_back(std::to_string(kSyncLevel));

        const int code = RunTool(ST_DEV_STPACK, args, kLogFile);
        if (code != 0) {
            r.ok = false;
            r.message = code < 0 ? std::string("could not start ") + ST_DEV_STPACK
                                 : "stpack failed with code " + std::to_string(code) + " - see " + kLogFile;
        } else {
            r.message = "converted " + std::to_string(r.files) + " source files";
        }
    } else {
        say("Copying changed assets");
        size_t copied = CopyNewer(SourceContentDir(), "assets", nullptr);
        copied += CopyNewer(SourceSceneDir(), std::string("assets/") + ST_DEV_SCENE_SUBDIR, kSceneExts);
        r.message = "copied " + std::to_string(copied) + " changed file(s)";
    }

    if (r.ok) {
        // Hashed again AFTER the conversion: --mirror-resources may have just written new
        // files into the source tree, and those must not count as a change next launch.
        size_t files = 0;
        WriteStamp(SourceHash(files));
    }
    r.seconds = timer.elapsed_seconds();
    r.message += " in " + std::to_string(int(r.seconds * 1000.0)) + " ms";
    return g_last = r;
}

std::string SourcePathFor (const std::string& runtimePath) {
    if (!IsDevBuild() || runtimePath.empty() || SourceAssetsDir().empty()) return std::string();
    std::error_code ec;
    fs::path p = U8(runtimePath);
    const fs::path cwd = fs::current_path(ec);
    if (p.is_absolute()) {
        // Already in the source tree: nothing to mirror.
        const std::string rel = fs::weakly_canonical(p, ec).lexically_relative(
            fs::weakly_canonical(U8(SourceAssetsDir()), ec)).generic_u8string();
        if (!rel.empty() && !StartsWith(rel, "..")) return std::string();
        p = fs::weakly_canonical(p, ec).lexically_relative(fs::weakly_canonical(cwd, ec));
    }
    const std::string rel = Lower(p.lexically_normal().generic_u8string());
    const std::string original = p.lexically_normal().generic_u8string();
    const std::string scenes = std::string("assets/") + Lower(ST_DEV_SCENE_SUBDIR) + "/";
    if (StartsWith(rel, scenes))
        return SourceSceneDir() + "/" + original.substr(scenes.size());
    if (StartsWith(rel, "assets/") && !SourceContentDir().empty())
        return SourceContentDir() + "/" + original.substr(7);
    return std::string();
}

bool MirrorToSource (const std::string& runtimePath, std::string* written) {
    if (written) written->clear();
    const std::string dst = SourcePathFor(runtimePath);
    if (dst.empty()) return false;
    std::error_code ec;
    fs::create_directories(U8(dst).parent_path(), ec);
    if (!fs::copy_file(U8(runtimePath), U8(dst), fs::copy_options::overwrite_existing, ec)) {
        wi::backlog::post("Dev build: could not copy " + runtimePath + " to " + dst + ": " + ec.message(),
                          wi::backlog::LogLevel::Warning);
        return false;
    }
    if (written) *written = dst;
    return true;
}

std::string SourceFileForPackageAsset (const std::string& name) {
    if (!IsDevBuild() || name.empty()) return std::string();
    std::error_code ec;
    for (const std::string& dir : { SourceContentDir(), SourceResourceDir() }) {
        if (dir.empty()) continue;
        const std::string candidate = dir + "/" + name;
        if (fs::is_regular_file(U8(candidate), ec)) return candidate;
    }
    return std::string();
}

std::string NewSourceFileForPackageAsset (const std::string& name) {
    if (!IsDevBuild() || name.empty() || SourceContentDir().empty()) return std::string();
    return SourceContentDir() + "/" + name;
}

bool IsDevPackage (const std::string& dir, const std::string& baseName) {
    if (!IsDevBuild() || ST_DEV_PACK == 0 || baseName != ST_DEV_PACK_NAME) return false;
    std::error_code ec;
    return fs::equivalent(U8(dir), U8(kPackSubdir), ec);
}

bool TrashSourceFile (const std::string& path, std::string* movedTo) {
    if (movedTo) movedTo->clear();
    if (!IsDevBuild() || SourceAssetsDir().empty()) return false;
    std::error_code ec;
    const fs::path src = U8(path);
    if (!fs::is_regular_file(src, ec)) return false;

    // One folder per game session, so everything removed together can be put back together.
    static std::string session;
    if (session.empty()) {
        const std::time_t now = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&now));
        session = buf;
    }
    const fs::path assets = fs::weakly_canonical(U8(SourceAssetsDir()), ec);
    fs::path rel = fs::weakly_canonical(src, ec).lexically_relative(assets);
    if (rel.empty() || rel.generic_u8string().rfind("..", 0) == 0) rel = src.filename();
    const fs::path dst = assets / ".trash" / session / rel;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (ec) {
        ec.clear();
        if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) return false;
        fs::remove(src, ec);
    }
    if (movedTo) *movedTo = dst.generic_u8string();
    return true;
}

} // namespace st::dev
