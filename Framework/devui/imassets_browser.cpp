// imassets_browser.cpp - the Resource Explorer's Browser view (see imassets.h).
//
// The Unity-Project-window shape: a folder tree on the left with type shortcuts above it,
// breadcrumbs and a search box on top, and a grid of tiles - folders, files, thumbnails
// for images - with a size slider in the bottom corner. Folders are not stored anywhere:
// a package is a flat list of logical paths, and the tree is those paths split on '/'.
//
// It shares everything else with the Table view: the rows (CollectRows), the selection,
// drag-and-drop (SIMTARY_ASSET_PAYLOAD) and the inspector under it.
//
// Icons are drawn with ImDrawList rather than loaded, so no icon font or atlas has to
// ship with the framework and they scale with the slider for free.
#include "imassets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "devui/imscripteditor.h"
#include "imgui.h"
#include "io/asset/StHash.h"
#include "wiHelper.h"

namespace st {

namespace {

ImU32 Col (float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

// The band colour a file tile carries, and the label used when it has no extension.
void TypeBadge (asset::AssetType type, ImU32& colour, const char*& fallback) {
    using T = asset::AssetType;
    switch (type) {
    case T::Script:    colour = Col(0.95f, 0.58f, 0.20f); fallback = "SCRIPT"; break;
    case T::Image:
    case T::Texture:   colour = Col(0.36f, 0.72f, 0.38f); fallback = "IMG";    break;
    case T::Sound:
    case T::Music:     colour = Col(0.62f, 0.42f, 0.86f); fallback = "AUDIO";  break;
    case T::Video:     colour = Col(0.86f, 0.32f, 0.32f); fallback = "VIDEO";  break;
    case T::Shader:    colour = Col(0.24f, 0.70f, 0.72f); fallback = "SHADER"; break;
    case T::Animation: colour = Col(0.90f, 0.42f, 0.64f); fallback = "ANIM";   break;
    case T::Font:      colour = Col(0.66f, 0.52f, 0.36f); fallback = "FONT";   break;
    case T::Json:
    case T::Text:      colour = Col(0.50f, 0.62f, 0.78f); fallback = "TEXT";   break;
    default:           colour = Col(0.55f, 0.55f, 0.58f); fallback = "FILE";   break;
    }
}

bool IsModelLike (asset::AssetType type) {
    return type == asset::AssetType::Model || type == asset::AssetType::Mesh ||
           type == asset::AssetType::Scene || type == asset::AssetType::Material;
}

void DrawFolderIcon (ImDrawList* dl, ImVec2 p, float s) {
    const ImU32 back  = Col(0.42f, 0.50f, 0.60f);
    const ImU32 front = Col(0.56f, 0.64f, 0.74f);
    const float r = std::max(1.0f, s * 0.06f);
    dl->AddRectFilled(ImVec2(p.x + s * 0.08f, p.y + s * 0.18f), ImVec2(p.x + s * 0.46f, p.y + s * 0.34f), back, r,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p.x + s * 0.06f, p.y + s * 0.26f), ImVec2(p.x + s * 0.94f, p.y + s * 0.84f), back, r);
    dl->AddRectFilled(ImVec2(p.x + s * 0.06f, p.y + s * 0.36f), ImVec2(p.x + s * 0.94f, p.y + s * 0.84f), front, r);
}

void DrawFileIcon (ImDrawList* dl, ImVec2 p, float s, asset::AssetType type, const std::string& ext) {
    ImU32 band;
    const char* fallback;
    TypeBadge(type, band, fallback);
    const float l = p.x + s * 0.20f, r = p.x + s * 0.80f, t = p.y + s * 0.06f, b = p.y + s * 0.94f;
    const float fold = s * 0.18f;
    // the page, with its top-right corner folded over
    const ImVec2 page[5] = { ImVec2(l, t), ImVec2(r - fold, t), ImVec2(r, t + fold), ImVec2(r, b), ImVec2(l, b) };
    dl->AddConvexPolyFilled(page, 5, Col(0.88f, 0.89f, 0.91f));
    dl->AddTriangleFilled(ImVec2(r - fold, t), ImVec2(r - fold, t + fold), ImVec2(r, t + fold), Col(0.68f, 0.70f, 0.74f));
    // a few "lines of text" so it reads as a document
    if (s >= 32.0f) {
        for (int i = 0; i < 3; ++i) {
            const float y = t + s * (0.26f + 0.10f * i);
            dl->AddLine(ImVec2(l + s * 0.08f, y), ImVec2(r - s * (i == 0 ? 0.22f : 0.08f), y),
                        Col(0.70f, 0.72f, 0.76f), std::max(1.0f, s * 0.025f));
        }
    }
    // the type band, with the extension on it once there is room
    const float bandTop = b - s * 0.30f, bandBottom = b - s * 0.08f;
    dl->AddRectFilled(ImVec2(l - s * 0.04f, bandTop), ImVec2(r + s * 0.04f, bandBottom), band, s * 0.03f);
    if (s >= 40.0f) {
        std::string label = ext.empty() ? std::string(fallback) : ext;
        for (char& c : label) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (label.size() > 6) label.resize(6);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2((l + r) * 0.5f - ts.x * 0.5f, (bandTop + bandBottom) * 0.5f - ts.y * 0.5f),
                    Col(1, 1, 1), label.c_str());
    }
}

// An isometric cube: models, meshes, materials, scenes (bluer).
void DrawModelIcon (ImDrawList* dl, ImVec2 p, float s, bool scene) {
    const ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
    const float w = s * 0.36f, h = s * 0.20f, d = s * 0.38f;
    const ImVec2 top(c.x, c.y - h - d * 0.5f), left(c.x - w, c.y - d * 0.5f), right(c.x + w, c.y - d * 0.5f);
    const ImVec2 mid(c.x, c.y + h - d * 0.5f), bl(c.x - w, c.y + d * 0.5f), br(c.x + w, c.y + d * 0.5f);
    const ImVec2 bottom(c.x, c.y + h + d * 0.5f);
    const ImVec4 k = scene ? ImVec4(0.40f, 0.62f, 0.90f, 1) : ImVec4(0.55f, 0.66f, 0.82f, 1);
    const ImVec2 topFace[4]   = { top, right, mid, left };
    const ImVec2 leftFace[4]  = { left, mid, bottom, bl };
    const ImVec2 rightFace[4] = { mid, right, br, bottom };
    dl->AddConvexPolyFilled(topFace, 4, Col(std::min(1.0f, k.x * 1.15f), std::min(1.0f, k.y * 1.15f), std::min(1.0f, k.z * 1.1f)));
    dl->AddConvexPolyFilled(leftFace, 4, Col(k.x * 0.80f, k.y * 0.80f, k.z * 0.85f));
    dl->AddConvexPolyFilled(rightFace, 4, Col(k.x * 0.60f, k.y * 0.60f, k.z * 0.68f));
}

void DrawStar (ImDrawList* dl, ImVec2 c, float radius, ImU32 colour) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + i * 0.6283185f;
        const float rr = (i % 2 == 0) ? radius : radius * 0.45f;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    // concave, so a fan of triangles from the centre
    for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], colour);
}

enum class Glyph { Folder, Star, File };

// A text-height icon in front of a label (tree rows, breadcrumbs). Reserves its space.
void InlineIcon (Glyph glyph, asset::AssetType type = asset::AssetType::Unknown) {
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (glyph == Glyph::Folder)      DrawFolderIcon(dl, p, s);
    else if (glyph == Glyph::Star)   DrawStar(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.48f, Col(1.0f, 0.82f, 0.25f));
    else if (IsModelLike(type))      DrawModelIcon(dl, p, s, type == asset::AssetType::Scene);
    else                             DrawFileIcon(dl, p, s, type, "");
    ImGui::Dummy(ImVec2(s, s));
    ImGui::SameLine(0, 4);
}

std::string ExtensionOf (const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    return path.substr(dot + 1);
}

std::string FolderOf (const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string LeafOf (const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The label under a tile, cut to fit with an ellipsis.
std::string FitLabel (const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > width) out.pop_back();
    return out + "...";
}

// The type shortcuts above the tree - Unity's Favorites.
struct Shortcut {
    const char*      label;
    asset::AssetType type;
};
const Shortcut kShortcuts[] = {
    { "All Scripts",    asset::AssetType::Script },
    { "All Images",     asset::AssetType::Image },
    { "All Textures",   asset::AssetType::Texture },
    { "All Models",     asset::AssetType::Model },
    { "All Scenes",     asset::AssetType::Scene },
    { "All Sounds",     asset::AssetType::Sound },
    { "All Animations", asset::AssetType::Animation },
};

// A vertical divider the user drags. `value` is the width it controls; `sign` is +1 when
//	dragging right should grow it (a pane on the left), -1 for a pane on the right.
void Splitter (const char* id, float& value, float minValue, float maxValue, float height, float sign) {
    ImGui::InvisibleButton(id, ImVec2(6.0f, std::max(1.0f, height)));
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active) value += sign * ImGui::GetIO().MouseDelta.x;
    value = std::clamp(value, minValue, std::max(minValue, maxValue));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float x = (a.x + b.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y),
        ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                  : (ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator)),
        active ? 2.0f : 1.0f);
}

} // namespace

void AssetExplorer::DrawBrowserLayout () {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Leave the footer (status line, working-set totals) its rows under the panes.
    float footer = 0.0f;
    if (editing_) footer += ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    if (!status_.empty()) footer += ImGui::GetTextLineHeightWithSpacing() * 2.0f + style.ItemSpacing.y;
    const float height = std::max(120.0f, avail.y - footer);

    // Wide panel: inspector beside the browser, full height, like Unity's Inspector next to
    //	its Project window. Narrow (docked along the bottom of a small window): stacked,
    //	and the inspector shrinks to one line while nothing is selected.
    const bool sideBySide = avail.x >= 640.0f;
    if (sideBySide) {
        const float browserWidth = avail.x - inspectorWidth_ - 6.0f - style.ItemSpacing.x * 2.0f;
        ImGui::BeginChild("##browserpane", ImVec2(browserWidth, height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawBrowser();
        ImGui::EndChild();
        ImGui::SameLine();
        Splitter("##inspsplit", inspectorWidth_, 200.0f, avail.x * 0.5f, height, -1.0f);
        ImGui::SameLine();
        ImGui::BeginChild("##inspector", ImVec2(0, height), ImGuiChildFlags_Borders);
        DrawInspector();
        ImGui::EndChild();
    } else {
        const float inspector = selection_.empty() ? ImGui::GetFrameHeightWithSpacing() + style.WindowPadding.y * 2.0f
                                                   : std::clamp(height * 0.30f, 90.0f, 220.0f);
        ImGui::BeginChild("##browserpane", ImVec2(0, std::max(100.0f, height - inspector - style.ItemSpacing.y)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawBrowser();
        ImGui::EndChild();
        ImGui::BeginChild("##inspector", ImVec2(0, inspector), ImGuiChildFlags_Borders);
        DrawInspector();
        ImGui::EndChild();
    }
}

void AssetExplorer::SelectRow (uint64_t id) {
    // Ctrl adds to the selection, a plain click replaces it.
    if (ImGui::GetIO().KeyCtrl) {
        auto it = std::find(selection_.begin(), selection_.end(), id);
        if (it != selection_.end()) selection_.erase(it);
        else                        selection_.push_back(id);
    } else {
        selection_.assign(1, id);
    }
    lastClicked_ = id;
}

void AssetExplorer::RowDragSource (const Row& row) {
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) return;
    AssetPayload payload;
    payload.id   = row.id;
    payload.type = row.type;
    std::snprintf(payload.path, sizeof(payload.path), "%s", row.path.c_str());
    ImGui::SetDragDropPayload(SIMTARY_ASSET_PAYLOAD, &payload, sizeof(payload));
    ImGui::TextUnformatted(row.path.c_str());
    if (payload.IsModel()) ImGui::TextDisabled("drop on the viewport to place it in the scene");
    else                   ImGui::TextDisabled("drop on an asset field in Properties");
    ImGui::EndDragDropSource();
}

void AssetExplorer::OpenRow (const Row& row) {
    if (row.type == asset::AssetType::Script && asset::CanonicalPath(ExtensionOf(row.path)) == "tp") {
        // The LOGICAL path, mount point included: that is what stScript names and what
        //	Taipan resolves imports against.
        st::devui::OpenScriptEditor(asset::NormalizePath(row.mount + row.path));
        return;
    }
    SetStatus(row.path + ": drag it onto the viewport or an asset field to use it.", false);
}

void AssetExplorer::RowContextMenu (const Row& row) {
    if (!ImGui::BeginPopupContextItem("##rowmenu")) return;
    if (std::find(selection_.begin(), selection_.end(), row.id) == selection_.end()) selection_.assign(1, row.id);
    ImGui::TextDisabled("%s", row.path.c_str());
    ImGui::Separator();
    if (row.type == asset::AssetType::Script && ImGui::MenuItem("Edit script")) OpenRow(row);
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(row.path.c_str());
    if (ImGui::MenuItem("Copy logical path"))
        ImGui::SetClipboardText(asset::NormalizePath(row.mount + row.path).c_str());
    if (ImGui::MenuItem("Extract...")) {
        const std::string dir = wi::helper::FolderDialog("Extract selected assets to");
        if (!dir.empty()) {
            std::string error;
            if (!ExtractEntries(selection_, dir, &error)) SetStatus(error, true);
        }
    }
    if (editing_) {
        ImGui::Separator();
        if (ImGui::MenuItem(row.removed ? "Restore" : "Remove")) {
            for (uint64_t id : selection_)
                if (Entry* e = FindEntry(id)) e->removed = !row.removed;
        }
    }
    ImGui::EndPopup();
}

const wi::graphics::Texture* AssetExplorer::Thumbnail (const Row& row) {
    if (row.type != asset::AssetType::Image && row.type != asset::AssetType::Texture) return nullptr;
    auto it = thumbs_.find(row.id);
    if (it == thumbs_.end()) {
        // A couple of loads per frame: decoding a folder of PNGs in one frame would stall
        //	the editor for seconds. The rest fill in over the following frames.
        if (thumbBudget_ <= 0) return nullptr;
        --thumbBudget_;
        Thumb thumb;
        const Entry* entry = editing_ ? FindEntry(row.id) : nullptr;
        if (entry && entry->origin == Entry::Origin::File)
            thumb.resource = wi::resourcemanager::Load(entry->filePath);
        else if (entry && entry->origin == Entry::Origin::Memory)
            thumb.resource = wi::resourcemanager::Load(entry->logicalPath, wi::resourcemanager::Flags::NONE,
                                                       entry->bytes.data(), entry->bytes.size());
        else
            thumb.resource = wi::resourcemanager::Load(row.path); // resolves through the mounted package
        thumb.failed = !thumb.resource.IsValid() || !thumb.resource.GetTexture().IsValid();
        it = thumbs_.emplace(row.id, std::move(thumb)).first;

        // Bounded: past this many, drop the one drawn least recently.
        constexpr size_t kMaxThumbs = 256;
        if (thumbs_.size() > kMaxThumbs) {
            auto oldest = thumbs_.end();
            for (auto t = thumbs_.begin(); t != thumbs_.end(); ++t)
                if (t->first != row.id && (oldest == thumbs_.end() || t->second.lastUsed < oldest->second.lastUsed))
                    oldest = t;
            if (oldest != thumbs_.end()) thumbs_.erase(oldest);
            it = thumbs_.find(row.id);
        }
    }
    it->second.lastUsed = frame_;
    if (it->second.failed) return nullptr;
    return &it->second.resource.GetTexture();
}

void AssetExplorer::RebuildTree (const std::vector<Row>& rows) {
    tree_ = FolderNode();
    for (const Row& row : rows) {
        FolderNode* node = &tree_;
        ++node->total;
        const std::string folder = FolderOf(row.path);
        size_t start = 0;
        while (start < folder.size()) {
            size_t end = folder.find('/', start);
            if (end == std::string::npos) end = folder.size();
            const std::string part = folder.substr(start, end - start);
            FolderNode& child = node->children[asset::CanonicalPath(part)];
            if (child.name.empty()) {
                child.name = part;
                child.path = folder.substr(0, end);
            }
            node = &child;
            ++node->total;
            start = end + 1;
        }
        ++node->files;
    }
}

void AssetExplorer::DrawFolderTree (const FolderNode& node, int depth) {
    const std::string current = asset::CanonicalPath(browseFolder_);
    for (const auto& kv : node.children) {
        const FolderNode& child = kv.second;
        const std::string childKey = asset::CanonicalPath(child.path);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (child.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (shortcutType_ < 0 && current == childKey) flags |= ImGuiTreeNodeFlags_Selected;
        // Keep the way down to the current folder unfolded (entered from the grid).
        if (!current.empty() && (current + "/").rfind(childKey + "/", 0) == 0) ImGui::SetNextItemOpen(true);

        const bool open = ImGui::TreeNodeEx(child.path.c_str(), flags, "%s", "");
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            browseFolder_ = child.path;
            shortcutType_ = -1;
        }
        ImGui::SameLine(0, 0);
        InlineIcon(Glyph::Folder);
        ImGui::TextUnformatted(child.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u", child.total);
        if (open) {
            DrawFolderTree(child, depth + 1);
            ImGui::TreePop();
        }
    }
}

void AssetExplorer::DrawTiles (const std::vector<Row>& rows) {
    // What the grid shows: the current folder's subfolders, then the files directly in
    //	it. A search or a type shortcut flattens that into every match instead.
    const bool flat = filter_[0] != 0 || shortcutType_ >= 0;
    std::vector<const FolderNode*> folders;
    std::vector<const Row*> files;

    if (!flat) {
        const FolderNode* node = &tree_;
        size_t start = 0;
        while (node && start < browseFolder_.size()) {
            size_t end = browseFolder_.find('/', start);
            if (end == std::string::npos) end = browseFolder_.size();
            auto it = node->children.find(asset::CanonicalPath(browseFolder_.substr(start, end - start)));
            node = it == node->children.end() ? nullptr : &it->second;
            start = end + 1;
        }
        if (node == nullptr) { // the folder went away (package edited or unmounted)
            browseFolder_.clear();
            node = &tree_;
        }
        for (const auto& kv : node->children) folders.push_back(&kv.second);
        const std::string here = asset::CanonicalPath(browseFolder_);
        for (const Row& row : rows)
            if (asset::CanonicalPath(FolderOf(row.path)) == here) files.push_back(&row);
    } else {
        const std::string filter = asset::CanonicalPath(filter_);
        for (const Row& row : rows) {
            if (shortcutType_ >= 0 && static_cast<int>(row.type) != shortcutType_) continue;
            if (!filter.empty() && asset::CanonicalPath(row.path).find(filter) == std::string::npos) continue;
            files.push_back(&row);
        }
    }
    std::sort(files.begin(), files.end(), [](const Row* a, const Row* b) {
        return asset::CanonicalPath(LeafOf(a->path)) < asset::CanonicalPath(LeafOf(b->path));
    });

    const int count = int(folders.size() + files.size());
    if (count == 0) {
        ImGui::TextDisabled(flat ? "Nothing matches." : "Empty folder.");
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float tile = tileSize_;
    // Small tiles drop the name and rely on the tooltip, as Unity's do.
    const float labelHeight = tile >= 48.0f ? ImGui::GetTextLineHeight() + 2.0f : 0.0f;
    const float cellW = tile + style.ItemSpacing.x;
    const float cellH = tile + labelHeight + style.ItemSpacing.y;
    const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
    const int lines = (count + columns - 1) / columns;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    std::string enterFolder;
    ImGuiListClipper clipper; // only the visible rows of tiles are built
    clipper.Begin(lines, cellH);
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            for (int col = 0; col < columns; ++col) {
                const int index = line * columns + col;
                if (index >= count) break;
                if (col > 0) ImGui::SameLine();

                const bool isFolder = index < (int)folders.size();
                const FolderNode* folder = isFolder ? folders[index] : nullptr;
                const Row* row = isFolder ? nullptr : files[index - folders.size()];
                const std::string name = isFolder ? folder->name : LeafOf(row->path);

                ImGui::PushID(isFolder ? ("d:" + folder->path).c_str() : ("f:" + row->path).c_str());
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##tile", ImVec2(tile, tile + labelHeight));
                const bool hovered = ImGui::IsItemHovered();
                const bool selected = !isFolder &&
                    std::find(selection_.begin(), selection_.end(), row->id) != selection_.end();

                if (selected || hovered) {
                    dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile + labelHeight),
                                      selected ? ImGui::GetColorU32(ImGuiCol_Header)
                                               : ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.45f),
                                      4.0f);
                }

                // icon, or the image itself
                const float pad = tile * 0.08f;
                const ImVec2 ip(p.x + pad, p.y + pad);
                const float is = tile - pad * 2.0f;
                if (isFolder) {
                    DrawFolderIcon(dl, ip, is);
                } else if (const wi::graphics::Texture* tex = Thumbnail(*row)) {
                    const float aspect = tex->desc.height ? float(tex->desc.width) / float(tex->desc.height) : 1.0f;
                    const ImVec2 sz = aspect >= 1.0f ? ImVec2(is, is / aspect) : ImVec2(is * aspect, is);
                    const ImVec2 a(ip.x + (is - sz.x) * 0.5f, ip.y + (is - sz.y) * 0.5f);
                    dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + sz.x + 1, a.y + sz.y + 1),
                                      Col(0.1f, 0.1f, 0.1f), 2.0f);
                    dl->AddImage((ImTextureID)tex, a, ImVec2(a.x + sz.x, a.y + sz.y));
                } else if (IsModelLike(row->type)) {
                    DrawModelIcon(dl, ip, is, row->type == asset::AssetType::Scene);
                } else {
                    DrawFileIcon(dl, ip, is, row->type, ExtensionOf(row->path));
                }
                // working-set state, as a mark on the tile
                if (!isFolder && row->removed) {
                    dl->AddLine(ip, ImVec2(ip.x + is, ip.y + is), Col(0.95f, 0.35f, 0.35f), 2.0f);
                } else if (!isFolder && (row->added || row->renamed)) {
                    dl->AddCircleFilled(ImVec2(p.x + tile - pad, p.y + pad), std::max(3.0f, tile * 0.05f),
                                        row->added ? Col(0.4f, 0.85f, 0.45f) : Col(1.0f, 0.8f, 0.3f));
                }

                // sounds: a play / stop button in the icon's corner, like Unity's
                bool overPlay = false;
                const bool isSound = !isFolder &&
                    (row->type == asset::AssetType::Sound || row->type == asset::AssetType::Music);
                if (isSound && (hovered || audio_.IsPlaying(row->id))) {
                    const float r = std::max(7.0f, tile * 0.14f);
                    const ImVec2 c(ip.x + is - r, ip.y + is - r);
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    overPlay = hovered && (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= r * r;
                    dl->AddCircleFilled(c, r, overPlay ? Col(0.25f, 0.55f, 0.95f) : Col(0.12f, 0.12f, 0.14f, 0.92f));
                    if (audio_.IsPlaying(row->id)) {
                        const float h = r * 0.42f;
                        dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), Col(1, 1, 1));
                    } else {
                        const float h = r * 0.48f;
                        dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - h), ImVec2(c.x - h * 0.7f, c.y + h),
                                              ImVec2(c.x + h, c.y), Col(1, 1, 1));
                    }
                }

                if (labelHeight > 0.0f) {
                    const std::string label = FitLabel(name, tile - 4.0f);
                    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    dl->AddText(ImVec2(p.x + (tile - ts.x) * 0.5f, p.y + tile), ImGui::GetColorU32(ImGuiCol_Text),
                                label.c_str());
                }

                if (isFolder) {
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) enterFolder = folder->path;
                    if (hovered) ImGui::SetTooltip("%s/\n%u item(s)", folder->path.c_str(), folder->total);
                } else {
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if (overPlay) {
                            // the button plays without disturbing a multi-selection's anchor
                            selection_.assign(1, row->id);
                            std::string sourceName = row->path;
                            const std::vector<uint8_t>* bytes = nullptr;
                            AudioSourceFor(row->id, sourceName, bytes);
                            audio_.Toggle(row->id, sourceName, bytes);
                        } else {
                            SelectRow(row->id);
                        }
                    }
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenRow(*row);
                    RowDragSource(*row);
                    RowContextMenu(*row);
                    if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                        ImGui::SetItemTooltip("%s\n%s, %s", row->path.c_str(), asset::ToString(row->type),
                                              asset::FormatBytes(row->size).c_str());
                }
                ImGui::PopID();
            }
        }
    }
    clipper.End();

    if (!enterFolder.empty()) {
        browseFolder_ = enterFolder;
        shortcutType_ = -1;
    }
}

void AssetExplorer::DrawBrowser () {
    ++frame_;
    thumbBudget_ = 2;

    std::vector<Row> rows;
    CollectRows(rows, false);

    // The tree only changes when the rows do. Cheap to rebuild, but not free on a package
    //	with tens of thousands of assets, so only when the set of ids moved.
    uint64_t signature = rows.size() * 0x9E3779B97F4A7C15ull;
    for (const Row& row : rows) signature = (signature ^ row.id) * 0x100000001B3ull;
    if (signature != treeSignature_) {
        RebuildTree(rows);
        treeSignature_ = signature;
    }

    // Everything below follows the space it is given, every frame: the panes fill the
    //	parent, the tree keeps the width the divider set (clamped to the panel), and the grid
    //	re-flows its columns to whatever is left.
    const float barHeight = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = std::max(80.0f, avail.y);
    treeWidth_ = std::clamp(treeWidth_, 120.0f, std::max(120.0f, avail.x * 0.45f));

    // left: shortcuts, then the folder tree
    ImGui::BeginChild("##tree", ImVec2(treeWidth_, height), ImGuiChildFlags_Borders);
    if (ImGui::TreeNodeEx("##favorites", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", "")) {
        ImGui::SameLine(0, 0);
        InlineIcon(Glyph::Star);
        ImGui::TextUnformatted("Favorites");
        for (const Shortcut& sc : kShortcuts) {
            const bool current = shortcutType_ == static_cast<int>(sc.type);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
            if (current) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx(sc.label, flags, "%s", "");
            if (ImGui::IsItemClicked()) shortcutType_ = current ? -1 : static_cast<int>(sc.type);
            ImGui::SameLine(0, 0);
            InlineIcon(Glyph::File, sc.type);
            ImGui::TextUnformatted(sc.label);
        }
        ImGui::TreePop();
    }
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
    if (browseFolder_.empty() && shortcutType_ < 0) rootFlags |= ImGuiTreeNodeFlags_Selected;
    const bool rootOpen = ImGui::TreeNodeEx("##assetsroot", rootFlags, "%s", "");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        browseFolder_.clear();
        shortcutType_ = -1;
    }
    ImGui::SameLine(0, 0);
    InlineIcon(Glyph::Folder);
    ImGui::TextUnformatted("Assets");
    ImGui::SameLine();
    ImGui::TextDisabled("%u", tree_.total);
    if (rootOpen) {
        DrawFolderTree(tree_, 0);
        ImGui::TreePop();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    Splitter("##treesplit", treeWidth_, 120.0f, avail.x * 0.45f, height, 1.0f);
    ImGui::SameLine(0, 0);

    // right: breadcrumbs + search, the grid, then the selection / size bar
    ImGui::BeginGroup();
    if (shortcutType_ >= 0) {
        for (const Shortcut& sc : kShortcuts) {
            if (static_cast<int>(sc.type) != shortcutType_) continue;
            InlineIcon(Glyph::Star);
            ImGui::TextUnformatted(sc.label);
        }
    } else {
        InlineIcon(Glyph::Folder);
        if (ImGui::SmallButton("Assets")) browseFolder_.clear();
        size_t start = 0;
        while (start < browseFolder_.size()) {
            size_t end = browseFolder_.find('/', start);
            if (end == std::string::npos) end = browseFolder_.size();
            ImGui::SameLine(0, 2);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0, 2);
            const std::string part = browseFolder_.substr(start, end - start);
            ImGui::PushID((int)start);
            const bool clicked = ImGui::SmallButton(part.c_str());
            ImGui::PopID();
            if (clicked) {
                browseFolder_ = browseFolder_.substr(0, end);
                break;
            }
            start = end + 1;
        }
    }
    {
        const float searchWidth = 200.0f;
        ImGui::SameLine();
        const float room = ImGui::GetContentRegionAvail().x - searchWidth;
        if (room > 0) ImGui::SameLine(ImGui::GetCursorPosX() + room);
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::InputTextWithHint("##search", "Search", filter_, sizeof(filter_));
    }

    ImGui::BeginChild("##grid", ImVec2(0, std::max(40.0f, height - barHeight * 2.0f)), ImGuiChildFlags_Borders);
    DrawTiles(rows);
    // A click on empty space clears the selection, like a file manager.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        selection_.clear();
    ImGui::EndChild();

    // the selected asset's logical path, and the tile-size slider in the corner
    const Row* sel = nullptr;
    if (!selection_.empty())
        for (const Row& r : rows)
            if (r.id == selection_.back()) { sel = &r; break; }
    if (sel)                     ImGui::TextDisabled("%s%s", sel->mount.c_str(), sel->path.c_str());
    else if (!selection_.empty()) ImGui::TextDisabled("%zu selected", selection_.size());
    else                          ImGui::TextDisabled("%zu assets", rows.size());
    {
        const float sliderWidth = 120.0f;
        ImGui::SameLine();
        const float room = ImGui::GetContentRegionAvail().x - sliderWidth;
        if (room > 0) ImGui::SameLine(ImGui::GetCursorPosX() + room);
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##tilesize", &tileSize_, 32.0f, 160.0f, "%.0f px");
    }
    ImGui::EndGroup();
}

} // namespace st
