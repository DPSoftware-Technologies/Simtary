#pragma once
// Resource Explorer - the DevUI window for the asset packages.
//
// Developer tooling, like everything else in devui/: drawn only while DevUI is visible,
// and never the game's own UI.
//
// Two jobs, and the second is why this is more than a listing:
//
//   INSPECT   see what is actually IN a package - which .stafp part each asset landed
//             in, at what offset, under which codec, what it hashes to, whether it is
//             streamable. A package is otherwise opaque, and "why is this build 4 GB"
//             is not a question a directory listing can answer.
//
//   MANAGE    open a package, add / remove / rename / recompress assets, and write it
//             back. Without this the only way to change one texture is to edit
//             assets/contents/ and rebuild the whole package from CMake, which is the
//             wrong loop for "swap this and look at it".
//
// The working set
//
// Editing is not done against the mounted package - it is memory-mapped and, on
// Windows, locked. Instead "Edit" loads a WORKING SET: one row per asset, each row
// remembering only WHERE its bytes come from (still in the source package, a file on
// disk, or a buffer this window built). Nothing is copied until Save.
//
// That indirection is what makes it cheap: opening a 40 GB package for editing costs a
// few hundred KB of rows, and a row whose bytes never changed is copied straight out of
// the old package into the new one.
//
// Save writes a complete new package into a staging folder FIRST, reading the untouched
// rows out of the still-mounted original, and only then unmounts, swaps the files and
// remounts. A crash or a disk-full at any point leaves the original package intact,
// which matters because the alternative is a half-written package that the reader
// correctly refuses and the developer has to rebuild from source.
//
// Drag-and-drop arrives as SDL_DROPFILE, routed from st::Run through
// st::App::HandleDroppedFile. SDL owns the string it hands over; the caller frees it.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "io/asset/AssetPack.h"
#include "render/api/Renderer.h"
#include "io/asset/AssetPackWriter.h"
#include "wiResourceManager.h"
#include "devui/imaudiopreview.h"

namespace st {

// ImGui drag-drop payload carrying one asset, so an asset can be dragged from this window
// onto anything that wants to consume one. Mirrors SIMTARY_ENTITY_PAYLOAD in
// devui/imhierarchy.h.
inline constexpr const char* SIMTARY_ASSET_PAYLOAD = "SIMTARY_ASSET";

// What the payload carries. The PATH is in here, not just the id, so a consumer needs to
// know nothing about the Resource Explorer's working set to use a dropped asset: a material
// texture slot takes `path` verbatim, because a mounted package resolves its own logical
// paths ("textures/wall.dds") through the asset-source override exactly as the engine
// stored them. Fixed size, trivially copyable - ImGui copies payloads by value.
struct AssetPayload {
    uint64_t         id   = 0;
    asset::AssetType type = asset::AssetType::Unknown;
    char             path[248] = {};   // NUL-terminated logical path; truncated if longer

    // True for what a scene can absorb as a model: the two forms the editor can merge.
    bool IsModel () const {
        const std::string p(path);
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string ext = p.substr(dot + 1);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return ext == "stsd" || ext == "wiscene";
    }

    // True for a .sttd: an object template, placed at the drop point rather than merged.
    bool IsTemplate () const {
        const std::string p(path);
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string ext = p.substr(dot + 1);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return ext == "sttd";
    }
};

class AssetExplorer {
public:
    // Content only - no Begin/End - so an editor layout can dock the same panel.
    void GUI ();
    // Window wrapper. `p_open` is the caller's visibility flag.
    void Draw (bool* p_open);

    // Queue a file or folder to import. Safe to call from the SDL event loop and from
    // wi::helper::FileDialog's worker thread; the path is only recorded here and the
    // reading happens on the next GUI(), because pulling a 200 MB texture off disk
    // inside the poll loop stalls the frame already in flight.
    void QueueImport (const std::string& path);

    // Queue a .strd to mount. Same threading contract as QueueImport: the file dialog
    // that produces this path runs on its own thread, and mounting touches the
    // AssetSystem, which the main thread reads.
    void QueueMount (const std::string& path);

    bool HasPendingImports () const;

    // Opening a template (double-click, or "Open template" on a row) is the editor's job;
    // the editor installs this. Unset = the row only says how to use it.
    std::function<void(const std::string& logicalPath)> onOpenTemplate;

    // Rescan the loose .sttd files under assets/ on the next draw. The editor calls it after
    // a template save, so the new file is draggable before any package holds it.
    void InvalidateLooseTemplates () { looseTemplatesStamp_ = 0.0; }

private:
    // one row of the working set
    struct Entry {
        // Where the bytes live. Nothing is materialised until Save, except Memory
        // entries, which are the ones this window produced itself (a .wiscene split
        // into a .stsd has no file behind it).
        enum class Origin : uint8_t { Package, File, Memory };

        std::string          logicalPath;      // editable; the key the game looks up
        std::string          originalPath;     // the name it had in the source package ("" if added)
        asset::AssetType     type      = asset::AssetType::Unknown;
        asset::Codec         codec     = asset::Codec::None;
        bool                 autoCodec = true;
        uint32_t             flags     = 0;

        Origin               origin    = Origin::Package;
        uint64_t             sourceId  = 0;    // Origin::Package - id in the source pack
        std::string          filePath;         // Origin::File
        std::vector<uint8_t> bytes;            // Origin::Memory

        uint64_t             size        = 0;  // uncompressed
        uint64_t             storedSize  = 0;  // on disk; only meaningful for Package
        uint64_t             contentHash = 0;  // known for Package, computed for the rest
        uint32_t             partNumber  = 0;

        bool                 removed = false;
        bool                 added   = false;
        bool                 renamed = false;
        std::string          note;             // provenance, or why it failed
    };

    // One asset as the views draw it - from the mounts in browse mode, from the working
    // set in edit mode. Both views build their lists through CollectRows, which is what
    // stops them drifting apart.
    struct Row {
        uint64_t         id = 0;
        std::string      path;          // name inside the package ("scripts/ai.tp")
        std::string      mount;         // mount point it is reached under ("assets/")
        asset::AssetType type = asset::AssetType::Unknown;
        asset::Codec     codec = asset::Codec::None;
        uint64_t         size = 0, storedSize = 0, contentHash = 0;
        uint32_t         partNumber = 0, flags = 0;
        bool             removed = false, added = false, renamed = false;
        const char*      origin = "package";
    };
    // `applyFilters` false = every row (the browser's folder tree needs the whole shape).
    void CollectRows (std::vector<Row>& rows, bool applyFilters) const;

    // Browser view: the Unity-style one. Folder tree on the left (with type shortcuts),
    // breadcrumbs, and a grid of folder / file tiles - thumbnails for images.
    struct FolderNode {
        std::string name;                         // "scripts"
        std::string path;                         // "scripts/ai" (no trailing slash; "" = root)
        std::map<std::string, FolderNode> children; // keyed by lower-case name: sorted, case-folded
        uint32_t files = 0;                       // directly inside
        uint32_t total = 0;                       // inside, recursively
    };
    // The Browser tab: browser + inspector laid out to the panel's CURRENT size - side by
    // side when wide, stacked when narrow - with draggable dividers.
    void DrawBrowserLayout ();
    void DrawBrowser ();
    void DrawFolderTree (const FolderNode& node, int depth);
    void DrawTiles (const std::vector<Row>& rows);
    void RebuildTree (const std::vector<Row>& rows);
    // Double-click: enter a folder, open a script in the Script Editor.
    void OpenRow (const Row& row);
    void SelectRow (uint64_t id);
    void RowContextMenu (const Row& row);
    void RowDragSource (const Row& row);
    // Where a sound row's bytes come from, for the audio preview: `name` is left as the
    // package name unless the working set says otherwise; `bytes` is set for in-memory rows.
    void AudioSourceFor (uint64_t id, std::string& name, const std::vector<uint8_t>*& bytes) const;
    // A loaded thumbnail for an image row, or null while it is (or cannot be) loaded.
    const render::TextureObject* Thumbnail (const Row& row);

    // panels
    void DrawToolbar ();
    void DrawPackages ();
    void DrawAssetTable ();
    void DrawInspector ();
    void DrawFooter ();

    // working set
    void BeginEdit (uint32_t mountIndex);
    void BeginNew ();
    void DiscardEdit ();
    bool Editing () const { return editing_; }
    bool Dirty () const;

    Entry*       FindEntry (uint64_t id);
    const Entry* FindEntry (uint64_t id) const;
    // Reads an entry's bytes from wherever they live. The one place that knows about
    // all three origins.
    bool ResolveBytes (const Entry& entry, std::vector<uint8_t>& out, std::string* error) const;

    void ProcessQueuedImports ();
    void AddFromFile (const std::string& path);
    void AddFromWiscene (const std::string& path);
    // A .stsd is already the converted form, so it is taken as it is - the split that
    // AddFromWiscene performs has nothing left to do. Its reference list is read only to
    // say, on the row, how many of the resources it needs the working set is missing.
    void AddFromStsd (const std::string& path);
    void AddEntry (Entry entry);

    bool SaveWorkingSet (const std::string& outDir, const std::string& baseName,
                         std::string* error);
    // Dev build (Framework/dev/DevBuild.h): write the working set's edits back into the
    // project's source assets - added and renamed rows as files, removed rows moved to
    // assets/.trash/. Without it the launch sync would rebuild the package from source and
    // silently undo the edit. Codec and type choices are not carried: the packer decides
    // those again from the file.
    bool ExportEditsToSource (std::string* summary, std::string* error);
    bool ExtractEntries (const std::vector<uint64_t>& ids, const std::string& outDir,
                         std::string* error);

    // Preview of the selected asset. Held as a wi::Resource so the texture stays alive
    // while ImGui is drawing with a raw pointer into it.
    void RefreshPreview ();
    void SetStatus (const std::string& text, bool isError);

    // state
    mutable std::mutex       importMutex_;

    // Loose .sttd files under assets/ that no mounted package holds - templates saved this
    // session. Listed in browse mode beside the packages, rescanned every few seconds.
    struct LooseTemplate { std::string path; uint64_t size = 0; };
    mutable std::vector<LooseTemplate> looseTemplates_;
    mutable double                     looseTemplatesStamp_ = 0.0;
    void RefreshLooseTemplates () const;
    std::vector<std::string> queuedImports_;
    std::vector<std::string> queuedMounts_;

    // The working set, and where it came from. `sourcePack_` stays mounted for the
    // whole edit so untouched rows can be copied out of it at Save time.
    bool                     editing_ = false;
    std::vector<Entry>       entries_;
    const asset::AssetPack*  sourcePack_ = nullptr;
    std::string              sourceDir_;      // folder holding the package being edited
    std::string              sourceBaseName_; // "content" for content.strd

    // Filters and selection.
    char                  filter_[128] = {};
    int                   typeFilter_  = -1;   // -1 = all, else an AssetType value
    int                   mountFilter_ = -1;   // -1 = all mounts (browse mode only)
    bool                  showRemoved_ = true;
    std::vector<uint64_t> selection_;
    uint64_t              lastClicked_ = 0;

    // Browser state.
    bool        browserView_ = true;   // false = the detailed table
    std::string browseFolder_;         // current folder ("" = the root)
    int         shortcutType_ = -1;    // a left-pane "All Scripts"-style shortcut, -1 = none
    float       tileSize_ = 76.0f;
    float       treeWidth_ = 210.0f;      // dragged by the divider; clamped to the panel
    float       inspectorWidth_ = 260.0f; // side-by-side layout only
    FolderNode  tree_;
    uint64_t    treeSignature_ = 0;    // rows the tree was built from; rebuilt when it moves
    struct Thumb {
        wi::Resource resource;
        render::TextureRef texture;
        uint64_t     lastUsed = 0;
        bool         failed = false;
    };
    std::unordered_map<uint64_t, Thumb> thumbs_;
    uint64_t    frame_ = 0;
    int         thumbBudget_ = 0;      // loads still allowed this frame

    // Preview.
    wi::Resource previewResource_;
    devui::AudioPreview audio_;   // sound playback (inspector player + tile buttons)
    uint64_t     previewId_ = 0;
    std::string  previewText_;
    bool         previewIsText_ = false;

    // Save options.
    char packName_[64]    = "content";
    char packOutDir_[512] = {};
    int  packPartSizeMB_  = 50;
    int  packLevel_       = 9;
    bool mountAfterWrite_ = true;

    std::string status_;
    bool        statusIsError_ = false;
};

} // namespace st
