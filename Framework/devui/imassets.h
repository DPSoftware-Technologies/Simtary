#pragma once
// Asset Explorer — the DevUI window for the mounted asset packages.
//
// Developer tooling, like everything else in devui/: it is drawn only while DevUI is
// visible and must never become the game's own UI.
//
// What it is for:
//   - see what is actually IN a shipped pack, and which .stafp part each asset landed
//     in, at what offset, under which codec. A pack is otherwise opaque, and "why is
//     this build 4 GB" is a question you cannot answer by looking at a directory.
//   - drag files in from Explorer/Finder and turn them into a pack without leaving the
//     game. A dropped .wiscene is converted the same way the build-time packer does it,
//     so the map and its resources both land in the tray.
//   - pull one asset back out to disk, which is the fast half of `stpack unpack`.
//
// Drag-and-drop arrives as SDL_DROPFILE, routed from st::Run through
// st::App::HandleDroppedFile. SDL owns the string it hands over; the caller frees it.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "io/asset/AssetPack.h"
#include "io/asset/AssetPackWriter.h"
#include "wiResourceManager.h"

namespace st {

// ImGui drag-drop payload carrying one asset ID, so an asset can be dragged from this
// window onto anything that wants to consume one. Mirrors SIMTARY_ENTITY_PAYLOAD in
// devui/imhierarchy.h.
inline constexpr const char* SIMTARY_ASSET_PAYLOAD = "SIMTARY_ASSET";

class AssetExplorer {
public:
    // Content only — no Begin/End — so an editor layout can dock the same panel.
    void GUI ();
    // Window wrapper. `p_open` is the caller's visibility flag.
    void Draw (bool* p_open);

    // Queue a file dropped onto the window. Safe to call from the SDL event loop; the
    // file is read and classified on the next Draw(), because reading a 200 MB texture
    // inside the poll loop stalls the frame that is already in flight.
    void QueueDrop (const std::string& path);

    bool HasPendingDrops () const;

private:
    // ── the import tray ────────────────────────────────────────────────────────
    // Files dropped in but not yet written to a package. Held with their bytes so the
    // source file can move or be deleted without breaking the pending build.
    struct Import {
        std::string          sourcePath;
        std::string          logicalPath;   // editable before writing
        std::vector<uint8_t> data;
        asset::AssetType     type  = asset::AssetType::Unknown;
        asset::Codec         codec = asset::Codec::None;
        bool                 autoCodec = true;
        bool                 include   = true;
        std::string          note;          // "converted from .wiscene", an error, ...
    };

    void DrawMounts ();
    void DrawBrowser ();
    void DrawInspector ();
    void DrawImportTray ();

    void ProcessQueuedDrops ();
    void AddImportFromFile (const std::string& path);
    void AddImportFromWiscene (const std::string& path);

    bool WritePackage (const std::string& outDir, const std::string& baseName,
                       bool mountAfter, std::string* error);
    bool ExtractSelected (const std::string& outDir, std::string* error);

    // Preview of the selected asset. Held as a wi::Resource so the texture stays alive
    // while ImGui is drawing with a raw pointer to it.
    void RefreshPreview ();

    // ── state ──────────────────────────────────────────────────────────────────
    std::mutex               dropMutex_;
    std::vector<std::string> queuedDrops_;

    std::vector<Import> imports_;

    char filter_[128] = {};
    int  typeFilter_  = -1;      // -1 = all, else an AssetType value
    int  mountFilter_ = -1;      // -1 = all mounts

    uint64_t    selectedId_   = 0;
    uint32_t    selectedMount_ = 0;
    std::string selectedName_;

    wi::Resource previewResource_;
    std::string  previewName_;
    std::string  previewText_;
    bool         previewIsText_ = false;

    char        packName_[64]   = "imported";
    char        packOutDir_[512] = {};
    int         packPartSizeMB_ = 50;
    int         packLevel_      = 9;
    bool        mountAfterWrite_ = true;

    std::string status_;
    bool        statusIsError_ = false;
};

} // namespace st
