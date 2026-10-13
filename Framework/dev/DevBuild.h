#pragma once
// Dev build mode - configure with -DSIMTARY_DEV_BUILD=ON (or the *-dev presets).
//
// A normal build treats the project's assets/ folder as INPUT to the build: the build
// converts it (stpack: package + .stsd maps) into <exe>/assets, and the game only ever
// sees that output. Two things go wrong with that while developing:
//
//   - whatever the game SAVES (a map from the editor, a package edited in the Resource
//     Explorer) lands in the build output, which the next build or clean overwrites
//   - an asset changed on disk is not seen until the next build re-packs it
//
// A dev build closes both loops:
//
//   save      editor saves are mirrored into the SOURCE tree (assets/scenes/,
//             assets/contents/, assets/resources/), so they survive and ship
//   autosync  at launch, before anything is mounted, the source tree is compared with what
//             the last sync saw. If anything changed - added, edited, renamed, deleted -
//             the same stpack conversion the build runs is run again into <exe>/assets.
//             Nothing changed costs one stat per source file.
//
// Command line: --no-devsync skips the launch sync, --devsync forces it.
// Outside a dev build every function here is a no-op that reports "not a dev build".

#include <functional>
#include <string>

namespace st::dev {

// True in a binary configured with SIMTARY_DEV_BUILD.
bool IsDevBuild ();

struct SyncReport {
    bool        ran     = false; // the conversion actually ran (something had changed)
    bool        ok      = true;
    size_t      files   = 0;     // source files looked at
    double      seconds = 0.0;
    std::string message;         // one line for the log / DevUI
};

// Bring <exe>/assets up to date with the project's source assets. st::Run calls this
// before any package is mounted (a mounted package is locked on Windows).
// `status` gets phase text while it works; it may be empty.
SyncReport SyncAssets (bool force, const std::function<void(const std::string&)>& status);
// What the launch sync did, for the DevUI.
const SyncReport& LastSync ();

// The project's source folders, as baked in at configure time. Empty outside a dev build.
std::string SourceAssetsDir ();   // <project>/assets
std::string SourceContentDir ();  // <project>/assets/contents
std::string SourceSceneDir ();    // <project>/assets/scenes
std::string SourceResourceDir (); // <project>/assets/resources

// Where a file the game wrote under its own assets/ belongs in the source tree:
//   assets/scenes/x.stsd   ->  <project>/assets/scenes/x.stsd
//   assets/anything/else   ->  <project>/assets/contents/anything/else
// Empty when the path is not under the game's assets/ (or already in the source tree).
std::string SourcePathFor (const std::string& runtimePath);

// Copy a file the game just wrote into its source location. Returns false (and leaves
// `written` empty) when there is nowhere to put it or the copy failed.
bool MirrorToSource (const std::string& runtimePath, std::string* written = nullptr);

// Package assets (names as stored inside content.strd: "textures/wall.dds").
// The file a package asset was built from: content dir first, then resource dir. Empty
// when neither holds it (it came out of a map's embedded resources, say).
std::string SourceFileForPackageAsset (const std::string& name);
// Where a NEW package asset of that name is written: the content dir.
std::string NewSourceFileForPackageAsset (const std::string& name);
// True when `dir` / `baseName` is the package the dev build syncs from source - the one
// whose edits must go back to the source tree or the next launch would undo them.
bool IsDevPackage (const std::string& dir, const std::string& baseName);
// Move a source file into <project>/assets/.trash/<timestamp>/ instead of deleting it, so a
// removal made from inside the game can always be undone by hand.
bool TrashSourceFile (const std::string& path, std::string* movedTo = nullptr);

} // namespace st::dev
