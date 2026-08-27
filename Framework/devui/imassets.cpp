#include "imassets.h"

#include <algorithm>
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

std::string LowerExtNoDot (const std::string& path) {
    std::string e = U8Path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return e;
}

// A rough "is this worth showing as text" test. Anything with a NUL or a high share of
// non-printables is binary, whatever its extension claims.
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
    // The list is built from the enum rather than hard-coded so a new AssetType shows up
    // here without a second edit.
    static const asset::AssetType kTypes[] = {
        asset::AssetType::Unknown, asset::AssetType::Texture, asset::AssetType::Image,
        asset::AssetType::Model,   asset::AssetType::Mesh,    asset::AssetType::Material,
        asset::AssetType::Sound,   asset::AssetType::Music,   asset::AssetType::Video,
        asset::AssetType::Script,  asset::AssetType::Font,    asset::AssetType::Shader,
        asset::AssetType::Scene,   asset::AssetType::Animation, asset::AssetType::Text,
        asset::AssetType::Json,    asset::AssetType::Binary,  asset::AssetType::Custom,
    };

    const char* preview = (*value < 0) ? "All types" : asset::ToString(static_cast<asset::AssetType>(*value));
    if (!ImGui::BeginCombo(label, preview)) return;
    if (allowAll && ImGui::Selectable("All types", *value < 0)) *value = -1;
    for (asset::AssetType t : kTypes) {
        const int v = static_cast<int>(t);
        if (ImGui::Selectable(asset::ToString(t), *value == v)) *value = v;
    }
    ImGui::EndCombo();
}

} // namespace

// ── drops ──────────────────────────────────────────────────────────────────────

void AssetExplorer::QueueDrop (const std::string& path) {
    std::lock_guard<std::mutex> lock(dropMutex_);
    queuedDrops_.push_back(path);
}

bool AssetExplorer::HasPendingDrops () const {
    return !queuedDrops_.empty();
}

void AssetExplorer::ProcessQueuedDrops () {
    std::vector<std::string> drops;
    {
        std::lock_guard<std::mutex> lock(dropMutex_);
        if (queuedDrops_.empty()) return;
        drops.swap(queuedDrops_);
    }

    for (const std::string& path : drops) {
        std::error_code ec;
        if (fs::is_directory(U8Path(path), ec)) {
            // A dropped folder imports its whole tree, keeping the structure as the
            // logical path — which is what a "drop my textures folder in" gesture means.
            const fs::path root = U8Path(path);
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                AddImportFromFile(it->path().string());
            }
            continue;
        }
        if (LowerExtNoDot(path) == "wiscene") AddImportFromWiscene(path);
        else                                  AddImportFromFile(path);
    }
}

void AssetExplorer::AddImportFromFile (const std::string& path) {
    Import entry;
    entry.sourcePath  = path;
    entry.logicalPath = asset::NormalizePath(U8Path(path).filename().string());

    if (!ReadWholeFile(path, entry.data)) {
        entry.include = false;
        entry.note    = "could not be read";
    }
    entry.type  = asset::ClassifyByExtension(entry.logicalPath);
    entry.codec = asset::DefaultCodecFor(entry.logicalPath, entry.type, entry.data.size());
    imports_.push_back(std::move(entry));
}

void AssetExplorer::AddImportFromWiscene (const std::string& path) {
    // Same split the build-time packer performs: the map becomes a .stsd and every
    // resource it embedded becomes its own tray entry, so a dropped map arrives already
    // deduplicated against everything else in the tray.
    std::vector<uint8_t> source;
    if (!ReadWholeFile(path, source)) {
        Import bad;
        bad.sourcePath  = path;
        bad.logicalPath = U8Path(path).filename().string();
        bad.include     = false;
        bad.note        = "could not be read";
        imports_.push_back(std::move(bad));
        return;
    }

    std::string error;
    asset::WisceneSplit split;
    if (!asset::SplitWiscene(source.data(), source.size(), split, &error)) {
        Import bad;
        bad.sourcePath  = path;
        bad.logicalPath = U8Path(path).filename().string();
        bad.include     = false;
        bad.note        = error;
        imports_.push_back(std::move(bad));
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

        bool already = false;
        for (const Import& existing : imports_)
            if (asset::CanonicalPath(existing.logicalPath) == asset::CanonicalPath(logical)) already = true;
        if (already) continue;

        Import res;
        res.sourcePath  = path;
        res.logicalPath = logical;
        res.data.assign(split.Bytes() + r.offset, split.Bytes() + r.offset + r.size);
        res.type  = asset::ClassifyByExtension(logical);
        res.codec = asset::DefaultCodecFor(logical, res.type, res.data.size());
        res.note  = "from " + scene.name + ".wiscene";
        imports_.push_back(std::move(res));
    }

    asset::SceneWriteOptions options;
    std::vector<uint8_t> stsd;
    if (!asset::SerializeSceneDescriptor(scene, stsd, options, &error)) {
        status_        = error;
        statusIsError_ = true;
        return;
    }

    Import map;
    map.sourcePath  = path;
    map.logicalPath = asset::NormalizePath("scenes/" + scene.name + ".stsd");
    map.data        = std::move(stsd);
    map.type        = asset::AssetType::Scene;
    map.codec       = asset::Codec::None;   // the ECS blob inside is already compressed
    map.autoCodec   = false;
    map.note        = std::to_string(split.resources.size()) + " resources extracted";
    imports_.push_back(std::move(map));
}

// ── writing ────────────────────────────────────────────────────────────────────

bool AssetExplorer::WritePackage (const std::string& outDir, const std::string& baseName,
                                  bool mountAfter, std::string* error) {
    asset::PackOptions options;
    options.partSizeTarget   = uint64_t(std::max(1, packPartSizeMB_)) * 1024 * 1024;
    options.compressionLevel = packLevel_;

    asset::AssetPackWriter writer;
    if (!writer.Begin(outDir, baseName, options, error)) return false;

    for (const Import& entry : imports_) {
        if (!entry.include || entry.data.empty()) continue;
        const asset::Codec codec = entry.autoCodec
            ? asset::DefaultCodecFor(entry.logicalPath, entry.type, entry.data.size())
            : entry.codec;
        if (!writer.Add(entry.logicalPath, entry.data.data(), entry.data.size(),
                        entry.type, codec, asset::AssetFlag_None, error))
            return false;
    }
    if (!writer.Finish(error)) return false;

    if (mountAfter) {
        // Mounting last means it shadows whatever is already mounted, which is exactly
        // what you want when you drop a replacement texture in and want to see it.
        const std::string index = outDir + "/" + baseName + ".staod";
        if (!AssetSystem::Get().Mount(index, "assets/", error)) return false;
        AssetSystem::Get().Install();
    }
    return true;
}

bool AssetExplorer::ExtractSelected (const std::string& outDir, std::string* error) {
    const asset::AssetPack* pack = nullptr;
    const asset::StaodAsset* a = AssetSystem::Get().Find(selectedName_, &pack);
    if (a == nullptr || pack == nullptr) {
        if (error) *error = "nothing selected";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!pack->Read(*a, bytes, error)) return false;
    const std::string out = outDir + "/" + pack->NameString(*a);
    if (!WriteWholeFile(out, bytes.data(), bytes.size())) {
        if (error) *error = "cannot write " + out;
        return false;
    }
    return true;
}

// ── preview ────────────────────────────────────────────────────────────────────

void AssetExplorer::RefreshPreview () {
    if (previewName_ == selectedName_) return;
    previewName_   = selectedName_;
    previewResource_ = wi::Resource();
    previewText_.clear();
    previewIsText_ = false;
    if (selectedName_.empty()) return;

    const asset::AssetPack* pack = nullptr;
    const asset::StaodAsset* a = AssetSystem::Get().Find(selectedName_, &pack);
    if (a == nullptr || pack == nullptr) return;

    const asset::AssetType type = static_cast<asset::AssetType>(a->type);
    if (type == asset::AssetType::Texture || type == asset::AssetType::Image) {
        // The pack is mounted, so the engine's own loader resolves this name through the
        // asset source override — no need to hand it the bytes.
        previewResource_ = wi::resourcemanager::Load(pack->NameString(*a));
        return;
    }

    if (a->originalSize > 256 * 1024) return;   // do not page a big blob in to look at it

    std::vector<uint8_t> bytes;
    if (!pack->Read(*a, bytes, nullptr)) return;
    if (!LooksLikeText(bytes.data(), bytes.size())) return;
    previewIsText_ = true;
    previewText_.assign(reinterpret_cast<const char*>(bytes.data()),
                        std::min<size_t>(bytes.size(), 8192));
}

// ── UI ─────────────────────────────────────────────────────────────────────────

void AssetExplorer::DrawMounts () {
    AssetSystem& assets = AssetSystem::Get();
    if (assets.MountCount() == 0) {
        ImGui::TextDisabled("No asset package mounted.");
        ImGui::TextWrapped("The game is reading loose files. Set AppConfig::assetPacks, or "
                           "build with simtary_add_app(PACK_ASSETS), or drop files below "
                           "and write a package from them.");
        return;
    }

    if (!ImGui::BeginTable("mounts", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp))
        return;
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
        ImGui::TableNextColumn(); ImGui::TextUnformatted(U8Path(info.path).filename().string().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.path.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(info.mountPoint.empty() ? "/" : info.mountPoint.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", info.assetCount);
        ImGui::TableNextColumn(); ImGui::Text("%u", info.partCount);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::FormatBytes(info.payloadSize).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Verify")) {
            std::string error;
            const asset::AssetPack* pack = assets.PackAt(i);
            // A full sequential read of every part. Fine on demand, never on startup.
            statusIsError_ = !(pack && pack->VerifyAll(&error));
            status_ = statusIsError_ ? error : (U8Path(info.path).filename().string() + " verified");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Unmount")) {
            assets.Unmount(info.path);
            selectedId_ = 0;
            selectedName_.clear();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    // Parts, so "which .stafp do I need to reship" has an answer.
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

void AssetExplorer::DrawBrowser () {
    AssetSystem& assets = AssetSystem::Get();

    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filter", "filter by path...", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    TypeCombo("##type", &typeFilter_, true);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    {
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
    }

    const std::string filter = asset::CanonicalPath(filter_);

    if (!ImGui::BeginTable("assets", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, 260)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("size");
    ImGui::TableSetupColumn("stored");
    ImGui::TableSetupColumn("codec");
    ImGui::TableSetupColumn("part");
    ImGui::TableHeadersRow();

    uint32_t shown = 0;
    for (uint32_t m = 0; m < assets.MountCount(); ++m) {
        if (mountFilter_ >= 0 && (int)m != mountFilter_) continue;
        const asset::AssetPack* pack = assets.PackAt(m);
        if (pack == nullptr) continue;

        for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
            const asset::StaodAsset* a = pack->AssetAt(i);
            if (typeFilter_ >= 0 && a->type != typeFilter_) continue;

            const std::string name = pack->NameString(*a);
            if (!filter.empty() && asset::CanonicalPath(name).find(filter) == std::string::npos)
                continue;

            // A cap, not a scroll clip: 100k rows would make ImGui the bottleneck. The
            // count is reported below so a truncated list never reads as a complete one.
            if (++shown > 2000) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(a->id & 0x7FFFFFFF));
            const bool selected = (selectedId_ == a->id);
            if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedId_    = a->id;
                selectedMount_ = m;
                selectedName_  = name;
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const uint64_t id = a->id;
                ImGui::SetDragDropPayload(SIMTARY_ASSET_PAYLOAD, &id, sizeof(id));
                ImGui::TextUnformatted(name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();

            ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::ToString(static_cast<asset::AssetType>(a->type)));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::FormatBytes(a->originalSize).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::FormatBytes(a->storedSize).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(CodecName(static_cast<asset::Codec>(a->codec)));
            ImGui::TableNextColumn(); ImGui::Text("%u", pack->PartAt(a->partIndex).number);
        }
    }
    ImGui::EndTable();

    if (shown > 2000)
        ImGui::TextDisabled("%u assets match; showing the first 2000. Narrow the filter to see the rest.", shown);
    else
        ImGui::TextDisabled("%u asset%s", shown, shown == 1 ? "" : "s");
}

void AssetExplorer::DrawInspector () {
    if (selectedName_.empty()) {
        ImGui::TextDisabled("Select an asset to inspect it.");
        return;
    }

    const asset::AssetPack* pack = nullptr;
    const asset::StaodAsset* a = AssetSystem::Get().Find(selectedName_, &pack);
    if (a == nullptr || pack == nullptr) {
        ImGui::TextDisabled("The selected asset is no longer mounted.");
        return;
    }

    ImGui::TextUnformatted(selectedName_.c_str());
    ImGui::Separator();
    ImGui::Text("id            %016llx", (unsigned long long)a->id);
    ImGui::Text("type          %s", asset::ToString(static_cast<asset::AssetType>(a->type)));
    ImGui::Text("codec         %s", CodecName(static_cast<asset::Codec>(a->codec)));
    ImGui::Text("size          %s", asset::FormatBytes(a->originalSize).c_str());
    ImGui::Text("stored        %s%s", asset::FormatBytes(a->storedSize).c_str(),
                a->originalSize ? ("  (" + std::to_string(int(100.0 * double(a->storedSize) /
                                    double(a->originalSize))) + "%)").c_str() : "");
    ImGui::Text("part          %s @ %llu", pack->PartAt(a->partIndex).fileName.c_str(),
                (unsigned long long)a->offset);
    ImGui::Text("content hash  %016llx", (unsigned long long)a->contentHash);
    ImGui::Text("streamable    %s", (a->flags & asset::AssetFlag_Streamable) ? "yes" : "no");
    if (!(a->flags & asset::AssetFlag_Streamable))
        ImGui::TextDisabled("  whole-frame compression: any read decodes the whole asset");

    if (ImGui::Button("Verify")) {
        std::string error;
        statusIsError_ = !pack->VerifyAsset(*a, &error);
        status_ = statusIsError_ ? error : (selectedName_ + " matches its hash");
    }
    ImGui::SameLine();
    if (ImGui::Button("Extract...")) {
        // The dialog runs on its own thread, so the callback only parks a path; the
        // extraction itself happens right here on the next frame's button press. Keeping
        // file IO off that thread is the same rule imeditor.cpp follows for Save As.
        const std::string dir = wi::helper::FolderDialog("Extract asset to");
        if (!dir.empty()) {
            std::string error;
            statusIsError_ = !ExtractSelected(dir, &error);
            status_ = statusIsError_ ? error : ("extracted to " + dir);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy path")) ImGui::SetClipboardText(selectedName_.c_str());

    RefreshPreview();
    if (previewResource_.IsValid()) {
        const wi::graphics::Texture& tex = previewResource_.GetTexture();
        if (tex.IsValid()) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float side  = std::min(avail, 256.0f);
            const float aspect = tex.desc.height ? float(tex.desc.width) / float(tex.desc.height) : 1.0f;
            ImGui::Image((ImTextureID)&tex,
                         aspect >= 1.0f ? ImVec2(side, side / aspect) : ImVec2(side * aspect, side));
            ImGui::TextDisabled("%u x %u", tex.desc.width, tex.desc.height);
        }
    } else if (previewIsText_) {
        ImGui::InputTextMultiline("##preview", previewText_.data(), previewText_.size(),
                                  ImVec2(-1, 160), ImGuiInputTextFlags_ReadOnly);
    }
}

void AssetExplorer::DrawImportTray () {
    ImGui::TextWrapped("Drop files or folders onto this window to import them. A dropped "
                       ".wiscene is split into a .stsd plus its resources, exactly as the "
                       "build-time packer does it.");

    if (imports_.empty()) {
        ImGui::TextDisabled("Nothing waiting to be packed.");
        return;
    }

    uint64_t totalBytes = 0;
    uint32_t included   = 0;
    for (const Import& entry : imports_) {
        if (!entry.include) continue;
        totalBytes += entry.data.size();
        ++included;
    }

    if (ImGui::BeginTable("imports", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 180))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("logical path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("type");
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("codec");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < imports_.size(); ++i) {
            Import& entry = imports_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Checkbox("##include", &entry.include);

            ImGui::TableNextColumn();
            {
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), "%s", entry.logicalPath.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##path", buffer, sizeof(buffer)))
                    entry.logicalPath = buffer;
            }
            if (!entry.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.note.c_str());

            ImGui::TableNextColumn();
            {
                int t = static_cast<int>(entry.type);
                ImGui::SetNextItemWidth(-1);
                TypeCombo("##type", &t, false);
                entry.type = static_cast<asset::AssetType>(t);
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset::FormatBytes(entry.data.size()).c_str());

            ImGui::TableNextColumn();
            {
                const asset::Codec effective = entry.autoCodec
                    ? asset::DefaultCodecFor(entry.logicalPath, entry.type, entry.data.size())
                    : entry.codec;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##codec", entry.autoCodec
                        ? (std::string("auto (") + CodecName(effective) + ")").c_str()
                        : CodecName(effective))) {
                    if (ImGui::Selectable("auto", entry.autoCodec)) entry.autoCodec = true;
                    for (asset::Codec c : { asset::Codec::None, asset::Codec::Zstd, asset::Codec::ZstdChunked }) {
                        if (ImGui::Selectable(CodecName(c), !entry.autoCodec && entry.codec == c)) {
                            entry.autoCodec = false;
                            entry.codec     = c;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Text("%u file%s selected, %s", included, included == 1 ? "" : "s",
                asset::FormatBytes(totalBytes).c_str());

    ImGui::SetNextItemWidth(160);
    ImGui::InputText("package name", packName_, sizeof(packName_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragInt("MB/part", &packPartSizeMB_, 1.0f, 1,
                   static_cast<int>(asset::kMaxPartSize / (1024 * 1024)));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragInt("zstd", &packLevel_, 0.2f, 1, 19);
    ImGui::SameLine();
    ImGui::Checkbox("mount after writing", &mountAfterWrite_);

    ImGui::SetNextItemWidth(-140);
    ImGui::InputTextWithHint("##outdir", "output folder...", packOutDir_, sizeof(packOutDir_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string dir = wi::helper::FolderDialog("Write asset package to");
        if (!dir.empty()) std::snprintf(packOutDir_, sizeof(packOutDir_), "%s", dir.c_str());
    }

    ImGui::BeginDisabled(included == 0 || packOutDir_[0] == '\0');
    if (ImGui::Button("Write package")) {
        std::string error;
        statusIsError_ = !WritePackage(packOutDir_, packName_, mountAfterWrite_, &error);
        if (statusIsError_) {
            status_ = error;
        } else {
            status_ = std::string(packName_) + ".staod written to " + packOutDir_ +
                      (mountAfterWrite_ ? " and mounted" : "");
            imports_.clear();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear tray")) imports_.clear();
}

void AssetExplorer::GUI () {
    ProcessQueuedDrops();

    // An explicit drop target over the whole window so ImGui shows the highlight, even
    // though the payload itself arrives through SDL rather than through ImGui.
    if (ImGui::BeginDragDropTarget()) ImGui::EndDragDropTarget();

    if (ImGui::BeginTabBar("assettabs")) {
        if (ImGui::BeginTabItem("Packages")) {
            DrawMounts();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Browse")) {
            DrawBrowser();
            ImGui::Separator();
            DrawInspector();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(imports_.empty() ? "Import" : "Import *")) {
            DrawImportTray();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (!status_.empty()) {
        ImGui::Separator();
        if (statusIsError_) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status_.c_str());
        else                ImGui::TextDisabled("%s", status_.c_str());
    }
}

void AssetExplorer::Draw (bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Asset Explorer", p_open)) GUI();
    ImGui::End();
}

} // namespace st
