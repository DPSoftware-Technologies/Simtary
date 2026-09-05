#include "imassets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "io/asset/AssetSystem.h"
#include "io/asset/SceneDescriptor.h"
#include "io/asset/StHash.h"
#include "wiHelper.h"
#include "wiBacklog.h"

namespace fs = std::filesystem;

namespace st {

namespace {

fs::path U8Path (const std::string& s) { return fs::u8path(s); }

bool ReadWholeFile (const std::string& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    const auto size = fs::file_size(U8Path(path), ec);
    if (ec) return false;
    std::ifstream in(U8Path(path), std::ios::binary);
    if (!in.is_open()) return false;
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

bool WriteWholeFile (const std::string& path, const void* data, uint64_t size) {
    std::error_code ec;
    const fs::path p = U8Path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

uint64_t FileSizeOf (const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(U8Path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

std::string LowerExtNoDot (const std::string& path) {
    std::string e = U8Path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

// Two paths that name the same file. Compared through the filesystem rather than as
// strings, because "assets/content.strd" and an absolute path to the same file are the
// same package and unmounting the wrong one leaves a locked file behind.
bool SamePath (const std::string& a, const std::string& b) {
    std::error_code ec;
    if (fs::exists(U8Path(a), ec) && fs::exists(U8Path(b), ec))
        if (fs::equivalent(U8Path(a), U8Path(b), ec) && !ec) return true;
    return fs::weakly_canonical(U8Path(a), ec) == fs::weakly_canonical(U8Path(b), ec);
}

// Rough "is this worth showing as text". Anything with a NUL, or a high share of
// non-printables, is binary whatever its extension claims.
bool LooksLikeText (const uint8_t* data, size_t size) {
    const size_t look = std::min<size_t>(size, 4096);
    size_t odd = 0;
    for (size_t i = 0; i < look; ++i) {
        const uint8_t c = data[i];
        if (c == 0) return false;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++odd;
    }
    return look == 0 || odd * 20 < look;
}

const char* CodecName (asset::Codec c) { return asset::ToString(c); }

void TypeCombo (const char* label, int* value, bool allowAll) {
    // Built from the enum rather than hard-coded, so a new AssetType appears here
    // without a second edit.
    static const asset::AssetType kTypes[] = {
        asset::AssetType::Unknown, asset::AssetType::Texture, asset::AssetType::Image,
        asset::AssetType::Model,   asset::AssetType::Mesh,    asset::AssetType::Material,
        asset::AssetType::Sound,   asset::AssetType::Music,   asset::AssetType::Video,
        asset::AssetType::Script,  asset::AssetType::Font,    asset::AssetType::Shader,
        asset::AssetType::Scene,   asset::AssetType::Animation, asset::AssetType::Text,
        asset::AssetType::Json,    asset::AssetType::Binary,  asset::AssetType::Custom,
    };

    const char* preview = (*value < 0) ? "All types"
                                       : asset::ToString(static_cast<asset::AssetType>(*value));
    if (!ImGui::BeginCombo(label, preview)) return;
    if (allowAll && ImGui::Selectable("All types", *value < 0)) *value = -1;
    for (asset::AssetType t : kTypes) {
        const int v = static_cast<int>(t);
        if (ImGui::Selectable(asset::ToString(t), *value == v)) *value = v;
    }
    ImGui::EndCombo();
}

} // namespace

void AssetExplorer::SetStatus (const std::string& text, bool isError) {
    status_        = text;
    statusIsError_ = isError;
    if (isError && !text.empty())
        wi::backlog::post("Resource Explorer: " + text, wi::backlog::LogLevel::Warning);
}

// imports

void AssetExplorer::QueueImport (const std::string& path) {
    std::lock_guard<std::mutex> lock(importMutex_);
    queuedImports_.push_back(path);
}

void AssetExplorer::QueueMount (const std::string& path) {
    std::lock_guard<std::mutex> lock(importMutex_);
    queuedMounts_.push_back(path);
}

bool AssetExplorer::HasPendingImports () const {
    std::lock_guard<std::mutex> lock(importMutex_);
    return !queuedImports_.empty() || !queuedMounts_.empty();
}

void AssetExplorer::ProcessQueuedImports () {
    std::vector<std::string> pending, mounts;
    {
        std::lock_guard<std::mutex> lock(importMutex_);
        pending.swap(queuedImports_);
        mounts.swap(queuedMounts_);
    }

    for (const std::string& index : mounts) {
        std::string error;
        if (AssetSystem::Get().Mount(index, "assets/", &error)) {
            AssetSystem::Get().Install();
            SetStatus("Mounted " + U8Path(index).filename().string(), false);
        } else {
            SetStatus(error, true);
        }
    }
    if (pending.empty()) return;

    // Something was dropped with no package open. Start a new one rather than dropping
    // the files on the floor - that is what the gesture meant.
    if (!editing_) BeginNew();

    size_t before = entries_.size();
    for (const std::string& path : pending) {
        std::error_code ec;
        if (fs::is_directory(U8Path(path), ec)) {
            // A dropped folder imports its whole tree, keeping the structure as the
            // logical path - which is what "drop my textures folder in" means.
            const fs::path root = U8Path(path);
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;

                Entry entry;
                entry.origin      = Entry::Origin::File;
                entry.filePath    = it->path().string();
                entry.logicalPath = asset::NormalizePath(
                    fs::relative(it->path(), root.parent_path(), ec).generic_string());
                entry.size        = FileSizeOf(entry.filePath);
                entry.type        = asset::ClassifyByExtension(entry.logicalPath);
                AddEntry(std::move(entry));
            }
            continue;
        }
        if (LowerExtNoDot(path) == "wiscene") AddFromWiscene(path);
        else                                  AddFromFile(path);
    }

    const size_t added = entries_.size() - before;
    if (added > 0) SetStatus(std::to_string(added) + " asset(s) added — not written until you Save", false);
}

void AssetExplorer::AddEntry (Entry entry) {
    entry.logicalPath = asset::NormalizePath(entry.logicalPath);
    if (entry.logicalPath.empty()) return;
    if (entry.type == asset::AssetType::Unknown)
        entry.type = asset::ClassifyByExtension(entry.logicalPath);
    entry.added = true;

    // Same logical path as an existing row: replace its bytes instead of adding a
    // duplicate the writer would refuse anyway. That is what dropping a new version of
    // a texture on top of the old one is asking for.
    const uint64_t id = asset::AssetIdFromPath(entry.logicalPath);
    for (Entry& existing : entries_) {
        if (asset::AssetIdFromPath(existing.logicalPath) != id) continue;
        const bool wasAdded = existing.added;
        entry.added   = wasAdded;
        entry.renamed = !wasAdded;              // an edit to a row that came from the package
        entry.note    = existing.removed ? "replaces a removed asset" : "replaced";
        existing = std::move(entry);
        return;
    }
    entries_.push_back(std::move(entry));
}

void AssetExplorer::AddFromFile (const std::string& path) {
    Entry entry;
    entry.origin      = Entry::Origin::File;
    entry.filePath    = path;
    entry.logicalPath = asset::NormalizePath(U8Path(path).filename().string());
    entry.size        = FileSizeOf(path);
    if (entry.size == 0 && !fs::exists(U8Path(path))) {
        entry.note = "could not be read";
    }
    AddEntry(std::move(entry));
}

void AssetExplorer::AddFromWiscene (const std::string& path) {
    // The same split the build-time packer performs: the map becomes a .stsd and every
    // resource it embedded becomes its own row, deduplicated against what is already
    // in the working set.
    std::vector<uint8_t> source;
    if (!ReadWholeFile(path, source)) {
        SetStatus(path + " could not be read", true);
        return;
    }

    std::string error;
    asset::WisceneSplit split;
    if (!asset::SplitWiscene(source.data(), source.size(), split, &error)) {
        SetStatus(U8Path(path).filename().string() + ": " + error, true);
        return;
    }

    asset::SceneDescriptor scene;
    scene.name           = U8Path(path).stem().string();
    scene.sourceFile     = asset::NormalizePath(U8Path(path).filename().string());
    scene.archiveVersion = split.archiveVersion;

    asset::SceneBlob ecs;
    ecs.kind  = asset::StsdBlobKind::EcsArchive;
    ecs.codec = asset::Codec::ZstdChunked;
    ecs.data  = std::move(split.ecsArchive);
    scene.blobs.push_back(std::move(ecs));

    for (const asset::EmbeddedResource& r : split.resources) {
        const std::string logical = asset::NormalizePath(r.name);

        asset::SceneAssetRef ref;
        ref.id          = asset::AssetIdFromPath(logical);
        ref.path        = logical;
        ref.flags       = asset::AssetFlag_FromScene;
        ref.engineFlags = r.engineFlags;
        scene.assets.push_back(ref);

        // Already present - from the package or from an earlier drop - so the map just
        // references it. This is the deduplication that makes two maps sharing a texture
        // cost one copy.
        bool already = false;
        for (const Entry& e : entries_)
            if (!e.removed && asset::AssetIdFromPath(e.logicalPath) == ref.id) already = true;
        if (already) continue;

        Entry entry;
        entry.origin      = Entry::Origin::Memory;
        entry.logicalPath = logical;
        entry.bytes.assign(split.Bytes() + r.offset, split.Bytes() + r.offset + r.size);
        entry.size        = entry.bytes.size();
        entry.contentHash = asset::Hash64(entry.bytes.data(), entry.bytes.size());
        entry.flags       = asset::AssetFlag_FromScene;
        entry.note        = "from " + scene.name + ".wiscene";
        AddEntry(std::move(entry));
    }

    std::vector<uint8_t> stsd;
    if (!asset::SerializeSceneDescriptor(scene, stsd, asset::SceneWriteOptions{}, &error)) {
        SetStatus(error, true);
        return;
    }

    Entry map;
    map.origin      = Entry::Origin::Memory;
    map.logicalPath = asset::NormalizePath("scenes/" + scene.name + ".stsd");
    map.bytes       = std::move(stsd);
    map.size        = map.bytes.size();
    map.contentHash = asset::Hash64(map.bytes.data(), map.bytes.size());
    map.type        = asset::AssetType::Scene;
    map.codec       = asset::Codec::None;   // the entity blob inside is already compressed
    map.autoCodec   = false;
    map.note        = std::to_string(split.resources.size()) + " resources extracted";
    AddEntry(std::move(map));
}

// working set

void AssetExplorer::BeginEdit (uint32_t mountIndex) {
    AssetSystem& assets = AssetSystem::Get();
    const asset::AssetPack* pack = assets.PackAt(mountIndex);
    if (pack == nullptr) return;

    entries_.clear();
    selection_.clear();
    entries_.reserve(pack->AssetCount());

    // Rows only - no bytes. An untouched row is copied straight from this package into
    // the new one at Save time, so opening a 40 GB package costs a few hundred KB.
    for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
        const asset::StrdAsset* a = pack->AssetAt(i);
        Entry entry;
        entry.origin      = Entry::Origin::Package;
        entry.sourceId    = a->id;
        entry.logicalPath = pack->NameString(*a);
        entry.type        = static_cast<asset::AssetType>(a->type);
        entry.codec       = static_cast<asset::Codec>(a->codec);
        entry.autoCodec   = false;   // keep what the package already decided
        entry.flags       = a->flags;
        entry.size        = a->originalSize;
        entry.storedSize  = a->storedSize;
        entry.contentHash = a->contentHash;
        entry.partNumber  = pack->PartAt(a->partIndex).number;
        entries_.push_back(std::move(entry));
    }

    sourcePack_ = pack;
    const std::string indexPath = pack->Path();
    sourceDir_      = U8Path(indexPath).parent_path().string();
    sourceBaseName_ = U8Path(indexPath).stem().string();
    editing_        = true;

    std::snprintf(packName_,   sizeof(packName_),   "%s", sourceBaseName_.c_str());
    std::snprintf(packOutDir_, sizeof(packOutDir_), "%s", sourceDir_.c_str());

    SetStatus("Editing " + sourceBaseName_ + ".strd — " +
              std::to_string(entries_.size()) + " assets. Nothing is written until Save.", false);
}

void AssetExplorer::BeginNew () {
    entries_.clear();
    selection_.clear();
    sourcePack_ = nullptr;
    sourceDir_.clear();
    sourceBaseName_.clear();
    editing_ = true;
    std::snprintf(packName_, sizeof(packName_), "%s", "content");
    SetStatus("New package — add files, then Save.", false);
}

void AssetExplorer::DiscardEdit () {
    entries_.clear();
    selection_.clear();
    sourcePack_ = nullptr;
    sourceDir_.clear();
    sourceBaseName_.clear();
    editing_ = false;
    previewId_ = 0;
    previewResource_ = wi::Resource();
    SetStatus("Changes discarded.", false);
}

bool AssetExplorer::Dirty () const {
    for (const Entry& e : entries_)
        if (e.added || e.removed || e.renamed) return true;
    return false;
}

AssetExplorer::Entry* AssetExplorer::FindEntry (uint64_t id) {
    for (Entry& e : entries_)
        if (asset::AssetIdFromPath(e.logicalPath) == id) return &e;
    return nullptr;
}

const AssetExplorer::Entry* AssetExplorer::FindEntry (uint64_t id) const {
    return const_cast<AssetExplorer*>(this)->FindEntry(id);
}

bool AssetExplorer::ResolveBytes (const Entry& entry, std::vector<uint8_t>& out,
                                  std::string* error) const {
    switch (entry.origin) {
    case Entry::Origin::Memory:
        out = entry.bytes;
        return true;

    case Entry::Origin::File:
        if (!ReadWholeFile(entry.filePath, out)) {
            if (error) *error = entry.filePath + " could not be read";
            return false;
        }
        return true;

    case Entry::Origin::Package: {
        // Straight out of the package this working set was opened from. This is why
        // Save stages before unmounting: the source has to still be mapped here.
        if (sourcePack_ == nullptr) {
            if (error) *error = entry.logicalPath + ": source package is no longer mounted";
            return false;
        }
        const asset::StrdAsset* a = sourcePack_->Find(entry.sourceId);
        if (a == nullptr) {
            if (error) *error = entry.logicalPath + ": no longer in the source package";
            return false;
        }
        return sourcePack_->Read(*a, out, error);
    }
    }
    if (error) *error = "unknown asset origin";
    return false;
}

// save

bool AssetExplorer::SaveWorkingSet (const std::string& outDir, const std::string& baseName,
                                    std::string* error) {
    if (outDir.empty() || baseName.empty()) {
        if (error) *error = "output folder and package name are both required";
        return false;
    }

    std::error_code ec;
    const std::string stageDir   = outDir + "/.stpack_stage";
    const std::string targetIndex = outDir + "/" + baseName + ".strd";

    fs::remove_all(U8Path(stageDir), ec);

    asset::PackOptions options;
    options.partSizeTarget   = uint64_t(std::max(1, packPartSizeMB_)) * 1024 * 1024;
    options.compressionLevel = packLevel_;

    // 1. write the whole new package into staging
    // Everything is written before anything is destroyed. Untouched rows are read out
    // of the source package, which is still mounted at this point.
    {
        asset::AssetPackWriter writer;
        if (!writer.Begin(stageDir, baseName, options, error)) return false;

        for (const Entry& entry : entries_) {
            if (entry.removed) continue;

            std::vector<uint8_t> bytes;
            if (!ResolveBytes(entry, bytes, error)) {
                fs::remove_all(U8Path(stageDir), ec);
                return false;
            }
            const asset::Codec codec = entry.autoCodec
                ? asset::DefaultCodecFor(entry.logicalPath, entry.type, bytes.size())
                : entry.codec;
            if (!writer.Add(entry.logicalPath, bytes.data(), bytes.size(),
                            entry.type, codec, entry.flags, error)) {
                fs::remove_all(U8Path(stageDir), ec);
                return false;
            }
        }
        if (!writer.Finish(error)) {
            fs::remove_all(U8Path(stageDir), ec);
            return false;
        }
    }

    // 2. unmount whatever currently owns the target files
    // A mounted package is memory-mapped, and on Windows that means the file cannot be
    // replaced while it is mapped. Find it by path rather than by assuming it is the
    // one being edited: "Save As" over a different mounted package is legal.
    AssetSystem& assets = AssetSystem::Get();
    bool wasMounted = false;
    std::string mountedPath, mountPoint;
    for (uint32_t i = 0; i < assets.MountCount(); ++i) {
        const AssetSystem::MountInfo info = assets.MountAt(i);
        if (!SamePath(info.path, targetIndex)) continue;
        wasMounted  = true;
        mountedPath = info.path;
        mountPoint  = info.mountPoint;
        break;
    }
    if (wasMounted) {
        sourcePack_ = nullptr;          // it is about to be destroyed
        assets.Unmount(mountedPath);
    }

    // 3. replace the files
    // Old parts are deleted by pattern, not by count: the new package may have fewer
    // parts than the old one, and an orphaned .stafpN left beside the new index is a
    // file the reader will refuse the whole set over.
    for (fs::directory_iterator it(U8Path(outDir), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.rfind(baseName + ".stafp", 0) == 0 || name == baseName + ".strd")
            fs::remove(it->path(), ec);
    }
    for (fs::directory_iterator it(U8Path(stageDir), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        fs::rename(it->path(), U8Path(outDir) / it->path().filename(), ec);
        if (ec) {
            // Across volumes rename fails; copy instead. Rare, but a staging folder on a
            // different drive is a perfectly ordinary thing for someone to configure.
            ec.clear();
            fs::copy_file(it->path(), U8Path(outDir) / it->path().filename(),
                          fs::copy_options::overwrite_existing, ec);
        }
    }
    fs::remove_all(U8Path(stageDir), ec);

    // 4. remount and rebind the working set to what is now on disk
    if (wasMounted || mountAfterWrite_) {
        std::string mountError;
        if (!assets.Mount(targetIndex, mountPoint.empty() ? "assets/" : mountPoint, &mountError)) {
            if (error) *error = "package written, but it could not be mounted: " + mountError;
            editing_ = false;
            entries_.clear();
            return false;
        }
        assets.Install();

        for (uint32_t i = 0; i < assets.MountCount(); ++i) {
            if (!SamePath(assets.MountAt(i).path, targetIndex)) continue;
            BeginEdit(i);   // reload from the package that now exists
            break;
        }
    } else {
        editing_ = false;
        entries_.clear();
    }
    return true;
}

bool AssetExplorer::ExtractEntries (const std::vector<uint64_t>& ids, const std::string& outDir,
                                    std::string* error) {
    uint32_t written = 0;
    for (uint64_t id : ids) {
        const Entry* entry = FindEntry(id);
        if (entry == nullptr || entry->removed) continue;

        std::vector<uint8_t> bytes;
        if (!ResolveBytes(*entry, bytes, error)) return false;
        if (!WriteWholeFile(outDir + "/" + entry->logicalPath, bytes.data(), bytes.size())) {
            if (error) *error = "cannot write " + outDir + "/" + entry->logicalPath;
            return false;
        }
        ++written;
    }
    SetStatus("Extracted " + std::to_string(written) + " asset(s) to " + outDir, false);
    return true;
}

// preview

void AssetExplorer::RefreshPreview () {
    const uint64_t id = selection_.empty() ? 0 : selection_.back();
    if (previewId_ == id) return;
    previewId_       = id;
    previewResource_ = wi::Resource();
    previewText_.clear();
    previewIsText_ = false;
    if (id == 0) return;

    const Entry* entry = FindEntry(id);
    if (entry == nullptr) return;

    if (entry->type == asset::AssetType::Texture || entry->type == asset::AssetType::Image) {
        // For a package asset the engine's own loader resolves the name through the
        // mounted asset source, so there is no need to hand it bytes. For a file that
        // has not been packed yet, the path on disk works just as well.
        const std::string name = (entry->origin == Entry::Origin::File)
                               ? entry->filePath : entry->logicalPath;
        if (entry->origin != Entry::Origin::Memory)
            previewResource_ = wi::resourcemanager::Load(name);
        else
            previewResource_ = wi::resourcemanager::Load(entry->logicalPath,
                                                         wi::resourcemanager::Flags::NONE,
                                                         entry->bytes.data(), entry->bytes.size());
        return;
    }

    if (entry->size > 256 * 1024) return;   // do not page a big blob in just to look at it

    std::vector<uint8_t> bytes;
    if (!ResolveBytes(*entry, bytes, nullptr)) return;
    if (!LooksLikeText(bytes.data(), bytes.size())) return;
    previewIsText_ = true;
    previewText_.assign(reinterpret_cast<const char*>(bytes.data()),
                        std::min<size_t>(bytes.size(), 8192));
}

// UI

void AssetExplorer::DrawToolbar () {
    AssetSystem& assets = AssetSystem::Get();

    if (!editing_) {
        ImGui::TextDisabled("Browsing mounted packages.");
        ImGui::SameLine();
        if (ImGui::Button("New package")) BeginNew();
        ImGui::SameLine();
        if (ImGui::Button("Mount...")) {
            wi::helper::FileDialogParams params;
            params.type        = wi::helper::FileDialogParams::OPEN;
            params.description = "Simtary asset index";
            params.extensions.push_back("strd");
            // The dialog runs on its own thread, so the callback only parks the path;
            // mounting happens on the next frame, on this thread.
            wi::helper::FileDialog(params, [this](std::string path) { QueueMount(path); });
        }
        return;
    }

    ImGui::Text("Editing:");
    ImGui::SameLine();
    if (sourceBaseName_.empty()) ImGui::TextUnformatted("(new package)");
    else                         ImGui::TextUnformatted((sourceBaseName_ + ".strd").c_str());
    if (Dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "* unsaved");
    }

    ImGui::SameLine();
    if (ImGui::Button("Add files...")) {
        wi::helper::FileDialogParams params;
        params.type        = wi::helper::FileDialogParams::OPEN;
        params.description = "Any file";
        params.multiselect = true;   // the callback fires once per selected file
        wi::helper::FileDialog(params, [this](std::string path) { QueueImport(path); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Add folder...")) {
        const std::string dir = wi::helper::FolderDialog("Add every file under");
        if (!dir.empty()) QueueImport(dir);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!Dirty() && !sourceBaseName_.empty());
    if (ImGui::Button("Save")) {
        std::string error;
        const std::string dir = packOutDir_[0] ? std::string(packOutDir_) : sourceDir_;
        if (SaveWorkingSet(dir, packName_, &error))
            SetStatus(std::string(packName_) + ".strd written to " + dir, false);
        else
            SetStatus(error, true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard")) DiscardEdit();

    ImGui::SetNextItemWidth(150);
    ImGui::InputText("name", packName_, sizeof(packName_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragInt("MB/part", &packPartSizeMB_, 1.0f, 1,
                   static_cast<int>(asset::kMaxPartSize / (1024 * 1024)));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::DragInt("zstd", &packLevel_, 0.2f, 1, 19);
    ImGui::SameLine();
    ImGui::Checkbox("mount after save", &mountAfterWrite_);

    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##outdir", "output folder (blank = in place)",
                             packOutDir_, sizeof(packOutDir_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string dir = wi::helper::FolderDialog("Write the package to");
        if (!dir.empty()) std::snprintf(packOutDir_, sizeof(packOutDir_), "%s", dir.c_str());
    }
    (void)assets;
}

void AssetExplorer::DrawPackages () {
    AssetSystem& assets = AssetSystem::Get();
    if (assets.MountCount() == 0) {
        ImGui::TextDisabled("No asset package mounted.");
        ImGui::TextWrapped("The game is reading loose files. Set AppConfig::assetPacks, build with "
                           "simtary_add_app(PACK_ASSETS), or drop files onto this window and save "
                           "a package from them.");
        return;
    }

    if (ImGui::BeginTable("mounts", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("index");
        ImGui::TableSetupColumn("mount");
        ImGui::TableSetupColumn("assets");
        ImGui::TableSetupColumn("parts");
        ImGui::TableSetupColumn("payload");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < assets.MountCount(); ++i) {
            const AssetSystem::MountInfo info = assets.MountAt(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(U8Path(info.path).filename().string().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.path.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(info.mountPoint.empty() ? "/" : info.mountPoint.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", info.assetCount);
            ImGui::TableNextColumn(); ImGui::Text("%u", info.partCount);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::FormatBytes(info.payloadSize).c_str());
            ImGui::TableNextColumn();

            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(editing_);
            if (ImGui::SmallButton("Edit")) BeginEdit(i);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Verify")) {
                std::string error;
                const asset::AssetPack* pack = assets.PackAt(i);
                // A full sequential read of every part. Fine on demand, never on startup.
                const bool ok = pack && pack->VerifyAll(&error);
                SetStatus(ok ? (U8Path(info.path).filename().string() + " verified") : error, !ok);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(editing_ && sourcePack_ == assets.PackAt(i));
            if (ImGui::SmallButton("Unmount")) {
                assets.Unmount(info.path);
                selection_.clear();
                ImGui::EndDisabled();
                ImGui::PopID();
                break;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Parts, so "which .stafp do I have to reship" has an answer.
    for (uint32_t i = 0; i < assets.MountCount(); ++i) {
        const asset::AssetPack* pack = assets.PackAt(i);
        if (pack == nullptr) continue;
        if (!ImGui::TreeNode((void*)(intptr_t)i, "Parts of %s",
                             U8Path(pack->Path()).filename().string().c_str()))
            continue;
        for (uint32_t p = 0; p < pack->PartCount(); ++p) {
            const asset::PartInfo part = pack->PartAt(p);
            ImGui::Text("%-22s %10s  %u assets  hash %016llx%s",
                        part.fileName.c_str(), asset::FormatBytes(part.fileSize).c_str(),
                        part.assetCount, (unsigned long long)part.fileHash,
                        (part.flags & asset::PartFlag_Oversized) ? "  [oversized]" : "");
        }
        ImGui::TreePop();
    }
}

void AssetExplorer::DrawAssetTable () {
    AssetSystem& assets = AssetSystem::Get();

    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "filter by path...", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    TypeCombo("##type", &typeFilter_, true);
    if (!editing_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        const std::string preview = mountFilter_ < 0 ? "All packages"
            : U8Path(assets.MountAt(static_cast<uint32_t>(mountFilter_)).path).filename().string();
        if (ImGui::BeginCombo("##mount", preview.c_str())) {
            if (ImGui::Selectable("All packages", mountFilter_ < 0)) mountFilter_ = -1;
            for (uint32_t i = 0; i < assets.MountCount(); ++i) {
                const std::string name = U8Path(assets.MountAt(i).path).filename().string();
                if (ImGui::Selectable(name.c_str(), mountFilter_ == (int)i)) mountFilter_ = (int)i;
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::SameLine();
        ImGui::Checkbox("show removed", &showRemoved_);
    }

    const std::string filter = asset::CanonicalPath(filter_);

    // In browse mode the rows come from the mounts; in edit mode they come from the
    // working set. Building one index list keeps the drawing code below identical for
    // both, which is what stops the two views from drifting apart.
    struct Row {
        uint64_t    id;
        std::string path;
        asset::AssetType type;
        asset::Codec     codec;
        uint64_t    size, storedSize, contentHash;
        uint32_t    partNumber, flags;
        bool        removed, added, renamed;
        const char* origin;
    };
    std::vector<Row> rows;

    if (editing_) {
        rows.reserve(entries_.size());
        for (const Entry& e : entries_) {
            if (e.removed && !showRemoved_) continue;
            if (typeFilter_ >= 0 && static_cast<int>(e.type) != typeFilter_) continue;
            if (!filter.empty() && asset::CanonicalPath(e.logicalPath).find(filter) == std::string::npos) continue;
            rows.push_back(Row{
                asset::AssetIdFromPath(e.logicalPath), e.logicalPath, e.type,
                e.autoCodec ? asset::DefaultCodecFor(e.logicalPath, e.type, e.size) : e.codec,
                e.size, e.storedSize, e.contentHash, e.partNumber, e.flags,
                e.removed, e.added, e.renamed,
                e.origin == Entry::Origin::Package ? "package"
                    : (e.origin == Entry::Origin::File ? "file" : "generated") });
        }
    } else {
        for (uint32_t m = 0; m < assets.MountCount(); ++m) {
            if (mountFilter_ >= 0 && (int)m != mountFilter_) continue;
            const asset::AssetPack* pack = assets.PackAt(m);
            if (pack == nullptr) continue;
            for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
                const asset::StrdAsset* a = pack->AssetAt(i);
                if (typeFilter_ >= 0 && a->type != typeFilter_) continue;
                const std::string name = pack->NameString(*a);
                if (!filter.empty() && asset::CanonicalPath(name).find(filter) == std::string::npos) continue;
                rows.push_back(Row{ a->id, name, static_cast<asset::AssetType>(a->type),
                                    static_cast<asset::Codec>(a->codec), a->originalSize,
                                    a->storedSize, a->contentHash,
                                    pack->PartAt(a->partIndex).number, a->flags,
                                    false, false, false, "package" });
            }
        }
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("assets", editing_ ? 7 : 6, flags, ImVec2(0, 240))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("type",   ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("size",   ImGuiTableColumnFlags_WidthFixed, 75);
    ImGui::TableSetupColumn("stored", ImGuiTableColumnFlags_WidthFixed, 75);
    ImGui::TableSetupColumn("codec",  ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("part",   ImGuiTableColumnFlags_WidthFixed, 44);
    if (editing_) ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableHeadersRow();

    // Sorting matters more here than it looks: "sort by size descending" is how you find
    // out what is actually making a build big.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
            std::sort(rows.begin(), rows.end(), [&s](const Row& a, const Row& b) {
                bool less = false;
                switch (s.ColumnIndex) {
                case 1:  less = a.type < b.type; break;
                case 2:  less = a.size < b.size; break;
                case 3:  less = a.storedSize < b.storedSize; break;
                case 4:  less = a.codec < b.codec; break;
                case 5:  less = a.partNumber < b.partNumber; break;
                default: less = a.path < b.path; break;
                }
                return s.SortDirection == ImGuiSortDirection_Ascending ? less : !less;
            });
        }
    }

    for (const Row& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(row.id & 0x7FFFFFFF));

        const bool selected = std::find(selection_.begin(), selection_.end(), row.id) != selection_.end();
        if (row.removed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.45f, 0.45f, 1.0f));

        if (ImGui::Selectable(row.path.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            // Ctrl adds to the selection, a plain click replaces it - the convention
            // every file manager uses, and the reason Extract can act on a set.
            if (ImGui::GetIO().KeyCtrl) {
                auto it = std::find(selection_.begin(), selection_.end(), row.id);
                if (it != selection_.end()) selection_.erase(it);
                else                        selection_.push_back(row.id);
            } else {
                selection_.assign(1, row.id);
            }
            lastClicked_ = row.id;
        }
        if (row.removed) ImGui::PopStyleColor();

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            // Path and type travel WITH the id: the drop sites (a material texture slot, the
            // editor viewport) live outside this window and must not have to reach back into
            // its working set to find out what was dragged.
            AssetPayload payload;
            payload.id   = row.id;
            payload.type = row.type;
            std::snprintf(payload.path, sizeof(payload.path), "%s", row.path.c_str());
            ImGui::SetDragDropPayload(SIMTARY_ASSET_PAYLOAD, &payload, sizeof(payload));
            ImGui::TextUnformatted(row.path.c_str());
            if (payload.IsModel())
                ImGui::TextDisabled("drop on the viewport to place it in the scene");
            else
                ImGui::TextDisabled("drop on an asset field in Properties");
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();

        ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::ToString(row.type));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::FormatBytes(row.size).c_str());
        ImGui::TableNextColumn();
        if (row.storedSize > 0) ImGui::TextUnformatted(asset::FormatBytes(row.storedSize).c_str());
        else                    ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(CodecName(row.codec));
        ImGui::TableNextColumn();
        if (row.partNumber > 0) ImGui::Text("%u", row.partNumber);
        else                    ImGui::TextDisabled("-");

        if (editing_) {
            ImGui::TableNextColumn();
            if (row.removed)      ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.45f, 1.0f), "removed");
            else if (row.added)   ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f),  "new");
            else if (row.renamed) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),   "edited");
            else                  ImGui::TextDisabled("%s", row.origin);
        }
    }
    ImGui::EndTable();

    ImGui::TextDisabled("%zu shown, %zu selected", rows.size(), selection_.size());

    // actions on the selection
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::Button("Extract...")) {
        const std::string dir = wi::helper::FolderDialog("Extract selected assets to");
        if (!dir.empty()) {
            std::string error;
            if (!ExtractEntries(selection_, dir, &error)) SetStatus(error, true);
        }
    }
    if (editing_) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            // Marked, not erased: the row has to survive so Save knows to leave it out,
            // and so the removal can be undone before anything is written.
            for (uint64_t id : selection_)
                if (Entry* e = FindEntry(id)) e->removed = true;
            SetStatus(std::to_string(selection_.size()) + " marked for removal", false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore")) {
            for (uint64_t id : selection_)
                if (Entry* e = FindEntry(id)) e->removed = false;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Select all")) {
        selection_.clear();
        for (const Row& row : rows) selection_.push_back(row.id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear selection")) selection_.clear();
}

void AssetExplorer::DrawInspector () {
    if (selection_.empty()) {
        ImGui::TextDisabled("Select an asset to inspect it.");
        return;
    }

    const uint64_t id = selection_.back();

    // In edit mode the working set is authoritative; in browse mode go to the mounts.
    const asset::AssetPack* pack = nullptr;

    std::string       path;
    asset::AssetType  type  = asset::AssetType::Unknown;
    asset::Codec      codec = asset::Codec::None;
    uint64_t size = 0, storedSize = 0, contentHash = 0;
    uint32_t flags = 0, partNumber = 0;
    std::string note, origin;

    if (editing_) {
        Entry* entry = FindEntry(id);
        if (entry == nullptr) { ImGui::TextDisabled("Selection is gone."); return; }

        // The logical path is editable here and nowhere else: it is the key the game
        // looks an asset up by, so renaming it is a real content change.
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s", entry->logicalPath.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", buffer, sizeof(buffer))) {
            const std::string next = asset::NormalizePath(buffer);
            if (!next.empty() && next != entry->logicalPath) {
                entry->logicalPath = next;
                entry->renamed     = true;
                selection_.assign(1, asset::AssetIdFromPath(next));
            }
        }

        int typeValue = static_cast<int>(entry->type);
        ImGui::SetNextItemWidth(160);
        TypeCombo("type", &typeValue, false);
        if (typeValue != static_cast<int>(entry->type)) {
            entry->type    = static_cast<asset::AssetType>(typeValue);
            entry->renamed = true;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        const asset::Codec effective = entry->autoCodec
            ? asset::DefaultCodecFor(entry->logicalPath, entry->type, entry->size)
            : entry->codec;
        const std::string codecLabel = entry->autoCodec
            ? (std::string("auto (") + CodecName(effective) + ")") : CodecName(effective);
        if (ImGui::BeginCombo("codec", codecLabel.c_str())) {
            if (ImGui::Selectable("auto", entry->autoCodec)) {
                entry->autoCodec = true;
                entry->renamed   = true;
            }
            for (asset::Codec c : { asset::Codec::None, asset::Codec::Zstd, asset::Codec::ZstdChunked }) {
                if (ImGui::Selectable(CodecName(c), !entry->autoCodec && entry->codec == c)) {
                    entry->autoCodec = false;
                    entry->codec     = c;
                    entry->renamed   = true;
                }
            }
            ImGui::EndCombo();
        }

        path        = entry->logicalPath;
        type        = entry->type;
        codec       = effective;
        size        = entry->size;
        storedSize  = entry->storedSize;
        contentHash = entry->contentHash;
        flags       = entry->flags;
        partNumber  = entry->partNumber;
        note        = entry->note;
        origin      = entry->origin == Entry::Origin::Package ? "in the package"
                    : (entry->origin == Entry::Origin::File ? entry->filePath : "generated here");
    } else {
        const asset::StrdAsset* a = nullptr;
        AssetSystem& assets = AssetSystem::Get();
        for (uint32_t m = 0; m < assets.MountCount() && a == nullptr; ++m) {
            const asset::AssetPack* p = assets.PackAt(m);
            if (p == nullptr) continue;
            if ((a = p->Find(id)) != nullptr) pack = p;
        }
        if (a == nullptr) { ImGui::TextDisabled("Selection is gone."); return; }

        path        = pack->NameString(*a);
        type        = static_cast<asset::AssetType>(a->type);
        codec       = static_cast<asset::Codec>(a->codec);
        size        = a->originalSize;
        storedSize  = a->storedSize;
        contentHash = a->contentHash;
        flags       = a->flags;
        partNumber  = pack->PartAt(a->partIndex).number;
        origin      = "in " + U8Path(pack->Path()).filename().string();
        ImGui::TextUnformatted(path.c_str());
    }

    ImGui::Separator();
    ImGui::Text("id            %016llx", (unsigned long long)id);
    ImGui::Text("source        %s", origin.c_str());
    ImGui::Text("size          %s", asset::FormatBytes(size).c_str());
    if (storedSize > 0) {
        ImGui::Text("stored        %s  (%d%%)", asset::FormatBytes(storedSize).c_str(),
                    size ? int(100.0 * double(storedSize) / double(size)) : 100);
    }
    if (partNumber > 0) ImGui::Text("part          %u", partNumber);
    if (contentHash)    ImGui::Text("content hash  %016llx", (unsigned long long)contentHash);
    ImGui::Text("streamable    %s", (flags & asset::AssetFlag_Streamable) ? "yes" : "no");
    if (!(flags & asset::AssetFlag_Streamable))
        ImGui::TextDisabled("  whole-frame compression: any read decodes the whole asset");
    if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    (void)type;
    (void)codec;

    if (pack != nullptr) {
        if (ImGui::Button("Verify")) {
            std::string error;
            const asset::StrdAsset* a = pack->Find(id);
            const bool ok = a && pack->VerifyAsset(*a, &error);
            SetStatus(ok ? (path + " matches its hash") : error, !ok);
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Copy path")) ImGui::SetClipboardText(path.c_str());

    RefreshPreview();
    if (previewResource_.IsValid()) {
        const wi::graphics::Texture& tex = previewResource_.GetTexture();
        if (tex.IsValid()) {
            const float avail  = ImGui::GetContentRegionAvail().x;
            const float side   = std::min(avail, 240.0f);
            const float aspect = tex.desc.height ? float(tex.desc.width) / float(tex.desc.height) : 1.0f;
            ImGui::Image((ImTextureID)&tex,
                         aspect >= 1.0f ? ImVec2(side, side / aspect) : ImVec2(side * aspect, side));
            ImGui::TextDisabled("%u x %u", tex.desc.width, tex.desc.height);
        }
    } else if (previewIsText_) {
        ImGui::InputTextMultiline("##preview", previewText_.data(), previewText_.size(),
                                  ImVec2(-1, 140), ImGuiInputTextFlags_ReadOnly);
    }
}

void AssetExplorer::DrawFooter () {
    if (editing_) {
        uint64_t total = 0;
        uint32_t live = 0, removed = 0, added = 0;
        for (const Entry& e : entries_) {
            if (e.removed) { ++removed; continue; }
            ++live;
            total += e.size;
            if (e.added) ++added;
        }
        ImGui::Separator();
        ImGui::Text("%u assets, %s", live, asset::FormatBytes(total).c_str());
        if (added || removed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "  +%u  -%u", added, removed);
        }
    }

    if (!status_.empty()) {
        ImGui::Separator();
        // Wrapped, because an error carries a path and a reason and truncating it hides
        // the half that says what to do about it.
        ImGui::PushStyleColor(ImGuiCol_Text, statusIsError_
            ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", status_.c_str());
        ImGui::PopStyleColor();
    }
}

void AssetExplorer::GUI () {
    ProcessQueuedImports();

    DrawToolbar();
    ImGui::Separator();

    if (ImGui::BeginTabBar("assettabs")) {
        if (ImGui::BeginTabItem(editing_ ? "Assets (editing)" : "Assets")) {
            DrawAssetTable();
            ImGui::Separator();
            DrawInspector();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Packages")) {
            DrawPackages();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    DrawFooter();
}

void AssetExplorer::Draw (bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Resource Explorer", p_open)) GUI();
    ImGui::End();
}

} // namespace st
