#include "imterrain.h"

#include "terrain/TerrainSystem.h"
#include "io/asset/AssetPack.h"
#include "io/asset/AssetSystem.h"
#include "io/model/ModelImporter.h"

#include "wiScene.h"
#include "wiTerrain.h"
#include "wiRenderer.h"
#include "wiRenderPath3D.h"
#include "wiHelper.h"
#include "wiBacklog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
namespace tr = st::terrain;

namespace {

constexpr float kPi = 3.14159265358979323846f;

const char* ToolLabel (st::TerrainEditor::Tool tool) {
    using Tool = st::TerrainEditor::Tool;
    switch (tool) {
        case Tool::Raise:   return "Raise";
        case Tool::Lower:   return "Lower";
        case Tool::Smooth:  return "Smooth";
        case Tool::Flatten: return "Flatten";
        case Tool::Paint:   return "Paint";
        case Tool::Replace: return "Replace";
        case Tool::Noise:   return "Noise";
        default:            return "None";
    }
}

const char* ToolHelp (st::TerrainEditor::Tool tool) {
    using Tool = st::TerrainEditor::Tool;
    switch (tool) {
        case Tool::Raise:   return "Pull the ground up under the brush. Shift lowers.";
        case Tool::Lower:   return "Push the ground down under the brush. Shift raises.";
        case Tool::Smooth:  return "Blur the height under the brush. Ctrl smooths with any tool.";
        case Tool::Flatten: return "Level the ground to the height where the stroke started (or a fixed height). Alt+click picks it.";
        case Tool::Paint:   return "Paint a layer. Painting takes weight from the other layers.";
        case Tool::Replace: return "Turn one layer into another, only where the first one is.";
        case Tool::Noise:   return "Roughen the ground with noise.";
        default:            return "";
    }
}

// World point -> pixel in the viewport image. False when the point is behind the camera.
bool Project (const wi::scene::CameraComponent& cam, const XMFLOAT3& p, ImVec2 imagePos,
              ImVec2 imageSize, ImVec2& out) {
    const XMVECTOR v = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.0f), cam.GetViewProjection());
    const float w = XMVectorGetW(v);
    if (w <= 1e-4f) return false;
    const float x = XMVectorGetX(v) / w, y = XMVectorGetY(v) / w;
    out = ImVec2(imagePos.x + (x * 0.5f + 0.5f) * imageSize.x,
                 imagePos.y + (0.5f - y * 0.5f) * imageSize.y);
    return true;
}

// A polyline draped over the terrain data, broken wherever a point goes behind the camera.
void DrawDraped (ImDrawList* dl, const tr::TerrainData& data, const wi::scene::CameraComponent& cam,
                 ImVec2 imagePos, ImVec2 imageSize, const XMFLOAT2* pts, int count, bool closed,
                 ImU32 color, float thickness) {
    ImVec2 prev;
    bool havePrev = false;
    const int n = closed ? count + 1 : count;
    for (int k = 0; k < n; ++k) {
        const XMFLOAT2& q = pts[k % count];
        const XMFLOAT3 p(q.x, data.SampleHeight(q.x, q.y) + 0.05f, q.y);
        ImVec2 s;
        const bool ok = Project(cam, p, imagePos, imageSize, s);
        if (ok && havePrev) dl->AddLine(prev, s, color, thickness);
        prev = s;
        havePrev = ok;
    }
}

void DrawBrushOutline (ImDrawList* dl, const tr::TerrainData& data, const wi::scene::CameraComponent& cam,
                       ImVec2 imagePos, ImVec2 imageSize, const tr::Brush& brush, float x, float z,
                       ImU32 color) {
    std::vector<XMFLOAT2> pts;
    if (brush.shape == tr::BrushShape::Circle) {
        const int seg = 64;
        for (int k = 0; k < seg; ++k) {
            const float a = float(k) / float(seg) * 2.0f * kPi;
            pts.emplace_back(x + std::cos(a) * brush.radius, z + std::sin(a) * brush.radius);
        }
    } else {
        const int per = 16;
        const float r = brush.radius;
        for (int k = 0; k < per; ++k) pts.emplace_back(x - r + 2 * r * k / per, z - r);
        for (int k = 0; k < per; ++k) pts.emplace_back(x + r, z - r + 2 * r * k / per);
        for (int k = 0; k < per; ++k) pts.emplace_back(x + r - 2 * r * k / per, z + r);
        for (int k = 0; k < per; ++k) pts.emplace_back(x - r, z + r - 2 * r * k / per);
    }
    DrawDraped(dl, data, cam, imagePos, imageSize, pts.data(), int(pts.size()), true, color, 2.0f);

    // The hardness ring: inside it the brush works at full strength.
    if (brush.hardness > 0.02f && brush.hardness < 0.98f) {
        std::vector<XMFLOAT2> in;
        for (const XMFLOAT2& p : pts)
            in.emplace_back(x + (p.x - x) * brush.hardness, z + (p.y - z) * brush.hardness);
        DrawDraped(dl, data, cam, imagePos, imageSize, in.data(), int(in.size()), true,
                   (color & 0x00FFFFFF) | 0x60000000, 1.0f);
    }
}

void DrawRegion (ImDrawList* dl, const tr::TerrainData& data, const wi::scene::CameraComponent& cam,
                 ImVec2 imagePos, ImVec2 imageSize, float x, float z, float sx, float sz) {
    std::vector<XMFLOAT2> pts;
    const int per = 32;
    for (int k = 0; k < per; ++k) pts.emplace_back(x + sx * k / per, z);
    for (int k = 0; k < per; ++k) pts.emplace_back(x + sx, z + sz * k / per);
    for (int k = 0; k < per; ++k) pts.emplace_back(x + sx - sx * k / per, z + sz);
    for (int k = 0; k < per; ++k) pts.emplace_back(x, z + sz - sz * k / per);
    DrawDraped(dl, data, cam, imagePos, imageSize, pts.data(), int(pts.size()), true,
               IM_COL32(90, 200, 255, 230), 2.0f);
}

bool ToolButton (const char* label, bool active, float width) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button(label, ImVec2(width, 0));
    if (active) ImGui::PopStyleColor(2);
    return clicked;
}

ImVec4 ToImVec4 (const XMFLOAT4& c) { return ImVec4(c.x, c.y, c.z, c.w); }

} // namespace

namespace st {

struct TerrainEditor::Stroke {
    Entity terrain = INVALID_ENTITY;
    std::string label;
    tr::TileSnapshot before;
    tr::TileSnapshot after;
    float flattenTarget = 0.0f;
};

// -------------------------------------------------------------------- terrain ---

Entity TerrainEditor::CurrentTerrain (wi::scene::Scene& scene) {
    tr::TerrainSystem& sys = tr::TerrainSystem::Get();
    if (terrain_ != INVALID_ENTITY && scene.terrains.Contains(terrain_) && sys.IsAuthored(terrain_))
        return terrain_;
    terrain_ = sys.FindTerrain(scene);
    return terrain_;
}

// ----------------------------------------------------------------------- panel ---

void TerrainEditor::Draw (wi::scene::Scene& scene, EditorHistory& history, Entity& selected) {
    // The heightmap file dialog answered on its own thread.
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (!dialogPath_.empty()) {
            import_.path.swap(dialogPath_);
            dialogPath_.clear();
        }
    }

    // A selected terrain becomes the one being edited.
    if (selected != INVALID_ENTITY && scene.terrains.Contains(selected)
        && tr::TerrainSystem::Get().IsAuthored(selected))
        terrain_ = selected;

    const Entity terrain = CurrentTerrain(scene);
    if (terrain == INVALID_ENTITY) {
        tool_ = Tool::None;
        DrawCreateTerrain(scene, history, selected);
        return;
    }

    tr::TerrainSystem& sys = tr::TerrainSystem::Get();
    const tr::TerrainData* data = sys.Data(terrain);
    const wi::scene::NameComponent* name = scene.names.GetComponent(terrain);
    ImGui::Text("%s", name ? name->name.c_str() : "Terrain");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu tiles, %.2f m samples%s", data->TileCount(), data->spacing,
                        sys.PendingEdits() > 0 ? ", applying..." : "");
    if (!sys.LastError().empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", sys.LastError().c_str());

    if (ImGui::BeginTabBar("##terraintabs")) {
        if (ImGui::BeginTabItem("Create")) {
            tab_ = Tab::Create;
            DrawCreateTab(scene, history, terrain);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Edit")) {
            tab_ = Tab::Edit;
            DrawEditTab(scene, terrain);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layers")) {
            tab_ = Tab::Layers;
            DrawLayersTab(scene, terrain, selected);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (!status_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", status_.c_str());
    }
}

void TerrainEditor::DrawCreateTerrain (wi::scene::Scene& scene, EditorHistory& history, Entity& selected) {
    ImGui::TextWrapped("No editable terrain in this scene. Create one, then sculpt and paint it "
                       "here. Physics, grass and the blended ground texture follow automatically.");
    ImGui::Spacing();
    static char nameBuf[64] = "Terrain";
    ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
    ImGui::DragFloat("Sample spacing (m)", &create_.spacing, 0.05f, 0.25f, 8.0f, "%.2f");
    ImGui::SetItemTooltip("Distance between height samples. 1 m is a good default; smaller is "
                          "more detail and more memory.");
    ImGui::DragFloat("Ground height (m)", &create_.baseHeight, 0.1f);
    ImGui::DragInt("Build radius (chunks)", &create_.generationRadius, 0.1f, 1, 32);
    ImGui::SetItemTooltip("How many chunks are built around the camera, each %.0f m across.",
                          66.0f * create_.spacing);
    ImGui::Checkbox("Engine grass", &create_.grass);
    ImGui::SetItemTooltip("Grows where a layer marked 'grass' is painted. Needs a grass material "
                          "to look right.");

    if (ImGui::Button("Create Terrain", ImVec2(-1, 0))) {
        create_.name = nameBuf;
        const Entity e = tr::TerrainSystem::Get().CreateTerrain(scene, create_);
        wi::vector<Entity> created;
        created.push_back(e);
        if (const wi::terrain::Terrain* t = scene.terrains.GetComponent(e))
            for (Entity m : t->materialEntities) created.push_back(m);
        history.RecordCreatedMany(scene, created, "Create Terrain");
        terrain_ = e;
        selected = e;
        tab_ = Tab::Create;
        status_ = "Terrain created. Generate or import ground in the Create tab, or pick a brush in Edit.";
    }
}

void TerrainEditor::DrawCreateTab (wi::scene::Scene& scene, EditorHistory& history, Entity terrain) {
    tr::TerrainData* data = tr::TerrainSystem::Get().Data(terrain);

    if (ImGui::CollapsingHeader("Region", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("Position (min X, Z)", &generate_.x, 1.0f);
        ImGui::DragFloat2("Size (X, Z)", &generate_.sizeX, 1.0f, 8.0f, 100000.0f, "%.0f m");
        if (ImGui::Button("Centre on brush / view")) {
            const XMFLOAT3 c = haveHover_ ? hover_ : scene.camera.Eye;
            generate_.x = c.x - generate_.sizeX * 0.5f;
            generate_.z = c.z - generate_.sizeZ * 0.5f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(blue box in the viewport)");
    }

    const tr::SampleRect region = data->RectFromWorld(generate_.x, generate_.z,
        generate_.x + generate_.sizeX, generate_.z + generate_.sizeZ);

    if (ImGui::CollapsingHeader("Generate", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Biomes");
        for (uint32_t b = 0; b < uint32_t(tr::Biome::Count); ++b) {
            if (b % 3 != 0) ImGui::SameLine(110.0f * float(b % 3));
            ImGui::Checkbox(tr::ToString(tr::Biome(b)), &generate_.biomes[b]);
        }
        ImGui::SliderFloat("Blending", &generate_.blending, 0.0f, 1.0f);
        ImGui::DragFloat("Biome size", &generate_.biomeSize, 2.0f, 20.0f, 20000.0f, "%.0f m");
        ImGui::DragFloat("Max height", &generate_.height, 0.5f, 0.0f, 5000.0f, "%.0f m");
        ImGui::DragFloat("Base height", &generate_.base, 0.1f);
        ImGui::SliderFloat("Warp", &generate_.warp, 0.0f, 1.0f);
        ImGui::SetItemTooltip("Bends the noise so ridges and valleys wander instead of lining up.");
        ImGui::SliderFloat("Erosion", &generate_.erosion, 0.0f, 1.0f);
        ImGui::SetItemTooltip("Water runs downhill, carves gullies and fills valleys with sediment.\n"
                              "This is what makes it look real. Higher is slower (~1-2 s per 512 m at 1).");
        ImGui::SameLine();
        ImGui::Checkbox("Scree", &generate_.thermal);
        ImGui::SetItemTooltip("Collapse slopes steeper than ~38 degrees.");
        ImGui::SliderFloat("Edge fade", &generate_.edgeFade, 0.0f, 0.5f);
        int seed = int(generate_.seed);
        if (ImGui::InputInt("Seed", &seed)) generate_.seed = uint32_t(seed);
        ImGui::SameLine();
        if (ImGui::SmallButton("Random"))
            generate_.seed = generate_.seed * 1664525u + 1013904223u;
        ImGui::Checkbox("Auto paint", &generate_.autoPaint);
        ImGui::SameLine();
        ImGui::BeginDisabled(!generate_.autoPaint);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Sand below", &generate_.waterLevel, 0.1f);
        ImGui::EndDisabled();

        if (ImGui::Button("Generate##run", ImVec2(-1, 0))) { // "##run": the header above is also "Generate"
            const tr::GenerateParams params = generate_;
            RegionEdit(terrain, history, "Generate Terrain", region,
                [params](tr::TerrainData& d) { return tr::Generate(d, params); });
            status_ = "Generating " + std::to_string(int(generate_.sizeX)) + " x "
                    + std::to_string(int(generate_.sizeZ)) + " m...";
        }
    }

    if (ImGui::CollapsingHeader("Import heightmap")) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s", import_.path.c_str());
        if (ImGui::InputText("File", path, sizeof(path))) import_.path = path;
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) {
            wi::helper::FileDialogParams params;
            params.type = wi::helper::FileDialogParams::OPEN;
            params.description = "Heightmap (*.png;*.r16;*.raw;*.r32;*.tga;*.bmp;*.jpg)";
            for (const char* ext : { "png", "r16", "raw", "r32", "tga", "bmp", "jpg" })
                params.extensions.push_back(ext);
            wi::helper::FileDialog(params, [this](std::string file) {
                std::lock_guard<std::mutex> lock(dialogMutex_);
                dialogPath_ = std::move(file);
            });
        }
        ImGui::TextDisabled("Placed over the Region above.");
        ImGui::DragFloat("Black = (m)", &import_.minHeight, 0.5f);
        ImGui::DragFloat("White = (m)", &import_.maxHeight, 0.5f);
        ImGui::Checkbox("Flip vertically", &import_.flipY);
        ImGui::SameLine();
        ImGui::Checkbox("Auto paint##import", &import_.autoPaint);
        ImGui::BeginDisabled(import_.path.empty());
        if (ImGui::Button("Import", ImVec2(-1, 0))) {
            tr::HeightmapImport params = import_;
            params.x = generate_.x;
            params.z = generate_.z;
            params.sizeX = generate_.sizeX;
            params.sizeZ = generate_.sizeZ;
            params.waterLevel = generate_.waterLevel;
            std::string* status = &status_;
            RegionEdit(terrain, history, "Import Heightmap", region,
                [params, status](tr::TerrainData& d) {
                    tr::SampleRect written;
                    std::string error;
                    if (!tr::ImportHeightmap(d, params, written, error)) {
                        *status = "Import failed: " + error;
                        wi::backlog::post("Terrain: " + *status, wi::backlog::LogLevel::Error);
                    } else {
                        *status = "Imported " + params.path;
                    }
                    return written;
                });
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Fill / Clear")) {
        ImGui::DragFloat("Fill height", &fillHeight_, 0.1f);
        ImGui::Checkbox("Paint too", &fillPaint_);
        if (fillPaint_) {
            uint32_t layer = uint32_t(fillLayer_);
            DrawLayerPicker("Fill layer", *data, layer);
            fillLayer_ = int(layer);
        }
        if (ImGui::Button("Fill region", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
            const float h = fillHeight_;
            const int layer = fillPaint_ ? fillLayer_ : -1;
            RegionEdit(terrain, history, "Fill Terrain", region,
                [region, h, layer](tr::TerrainData& d) { return d.FillRegion(region, h, layer); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear region", ImVec2(-1, 0))) {
            RegionEdit(terrain, history, "Clear Terrain", region,
                [region](tr::TerrainData& d) { return d.ClearRegion(region); });
        }
        ImGui::SetItemTooltip("Back to flat ground at the terrain's base height, first layer.");
    }

    if (ImGui::CollapsingHeader("Sea level")) {
        wi::scene::WeatherComponent* weather = scene.weathers.GetCount() > 0 ? &scene.weathers[0] : nullptr;
        if (weather == nullptr) {
            ImGui::TextDisabled("The scene has no weather yet; the terrain adds one on its first build.");
        } else {
            const Entity weatherEntity = scene.weathers.GetEntity(0);
            bool ocean = weather->IsOceanEnabled();
            seaLevel_ = weather->oceanParameters.waterHeight;
            if (ImGui::Checkbox("Ocean", &ocean)) {
                history.BeginEntity(scene, weatherEntity, "Toggle Ocean");
                weather->SetOceanEnabled(ocean);
                history.Commit(scene);
            }
            ImGui::BeginDisabled(!ocean);
            if (ImGui::DragFloat("Water height", &seaLevel_, 0.05f)) {
                weather->oceanParameters.waterHeight = seaLevel_;
            }
            if (ImGui::IsItemActivated()) history.BeginEntity(scene, weatherEntity, "Set Sea Level");
            if (ImGui::IsItemDeactivatedAfterEdit()) history.Commit(scene);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Water bodies and buoyancy use this level.");
        }
    }
}

void TerrainEditor::DrawLayerPicker (const char* label, const tr::TerrainData& data, uint32_t& layer) {
    ImGui::TextDisabled("%s", label);
    const float size = ImGui::GetFrameHeight() * 1.6f;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (uint32_t l = 0; l < data.LayerCount(); ++l) {
        if (l > 0) {
            const float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + size;
            if (next < right) ImGui::SameLine();
        }
        ImGui::PushID(int(l));
        const bool active = (layer == l);
        if (active) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
        if (ImGui::ColorButton("##layer", ToImVec4(data.layers[l].color),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(size, size)))
            layer = l;
        if (active) ImGui::PopStyleVar();
        ImGui::SetItemTooltip("%s", data.layers[l].name.c_str());
        ImGui::PopID();
    }
    if (layer < data.LayerCount())
        ImGui::Text("%s", data.layers[layer].name.c_str());
}

void TerrainEditor::DrawEditTab (wi::scene::Scene& scene, Entity terrain) {
    (void)scene;
    const tr::TerrainData* data = tr::TerrainSystem::Get().Data(terrain);

    const Tool tools[] = { Tool::Raise, Tool::Lower, Tool::Smooth, Tool::Flatten,
                           Tool::Paint, Tool::Replace, Tool::Noise };
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    for (int k = 0; k < int(sizeof(tools) / sizeof(tools[0])); ++k) {
        if (k % 3 != 0) ImGui::SameLine();
        if (ToolButton(ToolLabel(tools[k]), tool_ == tools[k], w))
            tool_ = (tool_ == tools[k]) ? Tool::None : tools[k];
        ImGui::SetItemTooltip("%s", ToolHelp(tools[k]));
    }
    if (tool_ == Tool::None) {
        ImGui::TextDisabled("Pick a tool, then left-drag in the Editor Viewport.");
        return;
    }
    ImGui::TextWrapped("%s", ToolHelp(tool_));

    if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        int shape = brush_.shape == tr::BrushShape::Circle ? 0 : 1;
        ImGui::RadioButton("Circle", &shape, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Square", &shape, 1);
        brush_.shape = shape == 0 ? tr::BrushShape::Circle : tr::BrushShape::Square;
        ImGui::DragFloat("Size", &brush_.radius, 0.1f, 0.5f, 500.0f, "%.1f m");
        ImGui::SetItemTooltip("Radius. [ and ] over the viewport change it too.");
        ImGui::SliderFloat("Strength", &brush_.strength, 0.01f, 1.0f);
        ImGui::SliderFloat("Hardness", &brush_.hardness, 0.0f, 1.0f);
        ImGui::SetItemTooltip("How much of the brush works at full strength before it fades out.");
    }

    switch (tool_) {
        case Tool::Flatten:
            ImGui::Checkbox("Fixed height", &flattenFixed_);
            ImGui::BeginDisabled(!flattenFixed_);
            ImGui::DragFloat("Height", &flattenHeight_, 0.05f);
            ImGui::EndDisabled();
            ImGui::TextDisabled(flattenFixed_ ? "Alt+click sets the height." :
                                                "Levels to wherever each stroke starts.");
            break;
        case Tool::Paint:
            DrawLayerPicker("Layer", *data, paintLayer_);
            break;
        case Tool::Replace:
            DrawLayerPicker("Replace", *data, replaceFrom_);
            DrawLayerPicker("With", *data, replaceTo_);
            break;
        case Tool::Noise: {
            ImGui::DragFloat("Noise scale", &noiseScale_, 0.1f, 0.5f, 500.0f, "%.1f m");
            int seed = int(noiseSeed_);
            if (ImGui::InputInt("Noise seed", &seed)) noiseSeed_ = uint32_t(seed);
            break;
        }
        default:
            break;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Shift inverts raise/lower. Ctrl smooths. One drag = one undo.");
}

void TerrainEditor::DrawLayersTab (wi::scene::Scene& scene, Entity terrain, Entity& selected) {
    tr::TerrainSystem& sys = tr::TerrainSystem::Get();
    tr::TerrainData* data = sys.Data(terrain);

    ImGui::TextWrapped("Each layer is a paint channel and a material. Open a layer to give it a "
                       "texture set.");

    static uint32_t open = 0;
    for (uint32_t l = 0; l < data->LayerCount(); ++l) {
        tr::Layer& layer = data->layers[l];
        ImGui::PushID(int(l));

        float color[3] = { layer.color.x, layer.color.y, layer.color.z };
        if (ImGui::ColorEdit3("##color", color, ImGuiColorEditFlags_NoInputs)) {
            layer.color = XMFLOAT4(color[0], color[1], color[2], 1.0f);
        }
        // Pushing the colour into the material rebuilds every chunk, so only on release.
        if (ImGui::IsItemDeactivatedAfterEdit()) sys.SyncLayerMaterial(scene, terrain, l);
        ImGui::SameLine();
        const bool expanded = ImGui::TreeNodeEx("##layer", open == l ? ImGuiTreeNodeFlags_DefaultOpen : 0,
                                                "%u  %s", l, layer.name.c_str());
        if (expanded) {
            char name[64];
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ImGui::InputText("Name", name, sizeof(name))) layer.name = name;
            if (ImGui::IsItemDeactivatedAfterEdit()) sys.SyncLayerMaterial(scene, terrain, l);

            bool grass = layer.grass;
            if (ImGui::Checkbox("Grass grows here", &grass)) {
                // The generator reads this flag from its threads, so it changes through the
                // queue like any edit. Grass is decided when a chunk is built: rebuild them all.
                wi::scene::Scene* s = &scene;
                sys.Edit(terrain, [s, terrain, l, grass](tr::TerrainData& d) {
                    if (l < d.LayerCount()) d.layers[l].grass = grass;
                    if (wi::terrain::Terrain* t = s->terrains.GetComponent(terrain)) t->Generation_Restart();
                    return tr::SampleRect();
                });
                sys.MarkDirty(terrain);
            }

            ImGui::SeparatorText("Textures");
            tr::LayerTextures tex = sys.GetLayerTextures(scene, terrain, l);
            std::string current = "None (plain colour)";
            if (!tex.baseColor.empty()) {
                const size_t slash = tex.baseColor.find_last_of('/');
                current = tex.baseColor.substr(slash == std::string::npos ? 0 : slash + 1);
                const size_t diff = current.rfind("_diff.");
                if (diff != std::string::npos) current.resize(diff);
            }
            if (ImGui::BeginCombo("Texture set", current.c_str(), ImGuiComboFlags_HeightLarge)) {
                // Scanned when the list opens: the packages only change between launches.
                if (ImGui::IsWindowAppearing()) textureSets_ = tr::TerrainSystem::FindTextureSets();
                if (ImGui::Selectable("None (plain colour)", tex.baseColor.empty())) {
                    tex.baseColor.clear();
                    tex.normal.clear();
                    tex.surface.clear();
                    sys.SetLayerTextures(scene, terrain, l, tex);
                }
                for (const tr::TextureSet& set : textureSets_) {
                    if (ImGui::Selectable(set.name.c_str(), set.textures.baseColor == tex.baseColor)) {
                        tr::LayerTextures pick = set.textures;
                        pick.tileSize = tex.tileSize > 0.0f ? tex.tileSize : 2.0f;
                        sys.SetLayerTextures(scene, terrain, l, pick);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s\n%s", set.textures.baseColor.c_str(),
                                          set.textures.normal.empty() ? "(no normal map)" : set.textures.normal.c_str(),
                                          set.textures.surface.empty() ? "(no ARM map)" : set.textures.surface.c_str());
                }
                if (textureSets_.empty())
                    ImGui::TextDisabled("No <name>_diff.jpg/.png in the packages.");
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Any texture named <name>_diff (Poly Haven's naming), with its "
                                  "<name>_nor_gl normal map and <name>_arm occlusion/roughness/metal "
                                  "map when they are beside it.");
            if (!tex.baseColor.empty()) {
                float tile = tileEditLayer_ == int(l) ? tileEdit_ : (tex.tileSize > 0.0f ? tex.tileSize : 2.0f);
                if (ImGui::SliderFloat("Tile size", &tile, 0.5f, 16.0f, "%.2f m", ImGuiSliderFlags_Logarithmic)) {
                    tileEdit_ = tile;
                    tileEditLayer_ = int(l);
                }
                // Retiling re-renders every chunk's texture, so only on release.
                if (ImGui::IsItemDeactivatedAfterEdit() && tileEditLayer_ == int(l)) {
                    tex.tileSize = tileEdit_;
                    sys.SetLayerTextures(scene, terrain, l, tex);
                    tileEditLayer_ = -1;
                }
                ImGui::SetItemTooltip("How many metres one repeat of the texture covers. Snapped so "
                                      "every chunk holds a whole number of repeats.");
            }

            const Entity material = sys.LayerMaterial(scene, terrain, l);
            ImGui::BeginDisabled(material == INVALID_ENTITY);
            if (ImGui::SmallButton("Select material")) selected = material;
            ImGui::EndDisabled();

            ImGui::SeparatorText("Soil");
            tr::SoilParams& soil = layer.soil;
            if (ImGui::BeginCombo("Preset", "Choose...")) {
                size_t count = 0;
                const char* const* names = tr::SoilPresetNames(count);
                for (size_t k = 0; k < count; ++k) {
                    if (ImGui::Selectable(names[k])) {
                        tr::SoilPreset(names[k], soil);
                        sys.MarkDirty(terrain);
                    }
                }
                ImGui::EndCombo();
            }
            bool changed = false;
            changed |= ImGui::SliderFloat("Tyre friction", &soil.friction, 0.0f, 1.5f);
            changed |= ImGui::SliderFloat("Rolling resistance", &soil.rollingResistance, 0.0f, 0.3f, "%.3f");
            changed |= ImGui::Checkbox("Deformable (ruts)", &soil.deformable);
            if (soil.deformable) {
                changed |= ImGui::DragFloat("kc", &soil.kc, 0.05f, 0.0f, 1000.0f);
                ImGui::SetItemTooltip("Bekker cohesive modulus, kN/m^(n+1)");
                changed |= ImGui::DragFloat("kphi", &soil.kphi, 1.0f, 0.0f, 100000.0f);
                ImGui::SetItemTooltip("Bekker frictional modulus, kN/m^(n+2)");
                changed |= ImGui::DragFloat("n", &soil.n, 0.01f, 0.1f, 3.0f);
                ImGui::SetItemTooltip("Sinkage exponent");
                changed |= ImGui::DragFloat("Cohesion", &soil.cohesion, 0.05f, 0.0f, 100.0f, "%.2f kPa");
                changed |= ImGui::DragFloat("Friction angle", &soil.frictionAngle, 0.1f, 0.0f, 60.0f, "%.1f deg");
            }
            changed |= ImGui::SliderFloat("Absorption", &soil.absorption, 0.0f, 1.0f);
            ImGui::SetItemTooltip("How fast standing water soaks in (surface liquid).");
            if (changed) sys.MarkDirty(terrain);

            ImGui::BeginDisabled(data->LayerCount() <= tr::kMinLayers);
            if (ImGui::SmallButton("Remove layer")) {
                sys.RemoveLayer(scene, terrain, l);
                status_ = "Removed layer '" + layer.name + "' (its paint went to the first layer).";
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Grass")) {
        // Edited on a copy and applied on release: every change rebuilds every chunk's grass.
        if (!grassEditing_) grassEdit_ = sys.GetGrass(scene, terrain);
        tr::GrassSettings& g = grassEdit_;
        bool apply = false;
        apply |= ImGui::Checkbox("Grass on", &g.enabled);
        ImGui::SetItemTooltip("Grows on every layer with 'Grass grows here' ticked.");
        char texture[256];
        std::snprintf(texture, sizeof(texture), "%s", g.texture.c_str());
        if (ImGui::InputText("Card texture", texture, sizeof(texture))) { g.texture = texture; grassEditing_ = true; }
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Logical path of the card atlas: grass on a transparent background, "
                              "root at the bottom edge of each frame.");
        int atlas[2] = { g.columns, g.rows };
        if (ImGui::SliderInt2("Frames (columns, rows)", atlas, 1, 4)) { g.columns = atlas[0]; g.rows = atlas[1]; grassEditing_ = true; }
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::SliderFloat("Height", &g.height, 0.05f, 2.0f, "%.2f m")) grassEditing_ = true;
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::SliderFloat("Height variation", &g.randomness, 0.0f, 1.0f)) grassEditing_ = true;
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::SliderFloat("Density", &g.density, 0.0f, 4.0f)) grassEditing_ = true;
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::SliderFloat("View distance", &g.viewDistance, 10.0f, 300.0f, "%.0f m")) grassEditing_ = true;
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Grass costs GPU time per card: keep this short in VR or on a weak GPU.");
        if (ImGui::SliderInt("Crossed cards", &g.crossCards, 1, 4)) grassEditing_ = true;
        if (ImGui::SliderFloat("Stiffness", &g.stiffness, 0.0f, 10.0f, "%.1f")) grassEditing_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How hard the grass springs back against the weather wind. Low = lies flat in gusts.");
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        if (apply) {
            sys.SetGrass(scene, terrain, g);
            grassEditing_ = false;
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Trees and props")) {
        ImGui::TextWrapped("Models scattered where a layer is painted, built around the camera "
                           "like the grass. Every copy shares one mesh.");
        std::vector<tr::PropSettings> props = sys.GetProps(scene, terrain);
        for (uint32_t i = 0; i < props.size(); ++i) {
            tr::PropSettings& p = props[i];
            ImGui::PushID(int(1000 + i));
            ImGui::SeparatorText(p.name.empty() ? "(prop)" : p.name.c_str());
            bool apply = false;
            int layer = int(p.layer);
            if (ImGui::SliderInt("On layer", &layer, 0, int(data->LayerCount()) - 1,
                                 layer < int(data->LayerCount()) ? data->layers[layer].name.c_str() : "?")) {
                p.layer = uint32_t(layer);
                apply = true;
            }
            int count[2] = { p.minPerChunk, p.maxPerChunk };
            if (ImGui::SliderInt2("Per chunk (min, max)", count, 0, 64)) { p.minPerChunk = count[0]; p.maxPerChunk = count[1]; }
            apply |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("Threshold", &p.threshold, 0.0f, 1.0f);
            apply |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("Higher = fewer: only where the layer is painted strongest and the noise is high.");
            ImGui::SliderFloat("Clumping", &p.clumping, 0.002f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
            apply |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("Low = big groves and clearings, high = spread evenly.");
            float size[2] = { p.minSize, p.maxSize };
            if (ImGui::SliderFloat2("Size (min, max)", size, 0.1f, 3.0f)) { p.minSize = size[0]; p.maxSize = size[1]; }
            apply |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("Sink", &p.sink, 0.0f, 2.0f, "%.2f m");
            apply |= ImGui::IsItemDeactivatedAfterEdit();
            if (apply) sys.UpdateProp(scene, terrain, i, p);
            if (ImGui::SmallButton("Remove")) sys.DeleteProp(scene, terrain, i);
            ImGui::PopID();
        }

        ImGui::SeparatorText("Add");
        if (ImGui::BeginCombo("Model", propModel_.empty() ? "Choose a model..." : propModel_.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                propModels_.clear();
                const st::AssetSystem& assets = st::AssetSystem::Get();
                for (uint32_t m = 0; m < assets.MountCount(); ++m) {
                    const st::asset::AssetPack* pack = assets.PackAt(m);
                    if (pack == nullptr) continue;
                    for (uint32_t k = 0; k < pack->AssetCount(); ++k) {
                        std::string path = pack->NameString(*pack->AssetAt(k));
                        if (st::model::CanImport(path) &&
                            std::find(propModels_.begin(), propModels_.end(), path) == propModels_.end())
                            propModels_.push_back(std::move(path));
                    }
                }
                std::sort(propModels_.begin(), propModels_.end());
            }
            for (const std::string& path : propModels_)
                if (ImGui::Selectable(path.c_str(), path == propModel_)) propModel_ = path;
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(propModel_.empty());
        if (ImGui::Button("Scatter it", ImVec2(-1, 0))) {
            tr::PropSettings p;
            std::string why;
            if (sys.AddProp(scene, terrain, propModel_, p, &why)) status_ = "Scattering " + propModel_ + " on layer 0.";
            else status_ = "Could not add " + propModel_ + ": " + why;
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(data->LayerCount() >= tr::kMaxLayers);
    if (ImGui::Button("Add layer", ImVec2(-1, 0))) {
        tr::Layer layer;
        layer.name = "Layer " + std::to_string(data->LayerCount());
        layer.color = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
        tr::SoilPreset("Gravel", layer.soil);
        sys.AddLayer(scene, terrain, layer);
        open = data->LayerCount();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Layers are not undoable. %u of %u used.", data->LayerCount(), tr::kMaxLayers);
}

// ------------------------------------------------------------------ edits ---

void TerrainEditor::RegionEdit (Entity terrain, EditorHistory& history, const char* label,
                                tr::SampleRect rect,
                                std::function<tr::SampleRect(tr::TerrainData&)> op) {
    if (rect.Empty()) return;
    auto snap = std::make_shared<Stroke>();
    snap->terrain = terrain;
    snap->label = label;
    EditorHistory* h = &history;
    // Region edits take the full rebuild path: they touch many chunks at once, where
    //	regenerating on the generator's threads beats refreshing them all on this one.
    tr::TerrainSystem::Get().Edit(terrain, [snap, rect, op, h](tr::TerrainData& d) {
        d.Capture(rect, snap->before);
        const tr::SampleRect written = op(d);
        snap->after = d.CaptureSame(snap->before);
        const Entity e = snap->terrain;
        h->PushCustom(snap->label.c_str(), e, [snap, e](bool redo) {
            tr::TerrainSystem::Get().Edit(e, [snap, redo](tr::TerrainData& d2) {
                return d2.Restore(redo ? snap->after : snap->before);
            }, true);
        });
        return written;
    }, true);
}

void TerrainEditor::BeginStroke (Entity terrain, Tool tool, float x, float y, float z) {
    stroke_ = std::make_shared<Stroke>();
    stroke_->terrain = terrain;
    stroke_->label = std::string("Terrain ") + ToolLabel(tool);
    stroke_->flattenTarget = flattenFixed_ ? flattenHeight_ : y;
    strokeTool_ = tool;
    lastHit_ = XMFLOAT3(x, y, z);
    haveLast_ = true;
}

void TerrainEditor::Dab (Entity terrain, Tool tool, float x, float z, float dt) {
    if (!stroke_) return;
    auto stroke = stroke_;
    const tr::Brush brush = brush_;
    const float s = brush.strength;
    const uint32_t paintLayer = paintLayer_, from = replaceFrom_, to = replaceTo_;
    const float noiseScale = noiseScale_;
    const uint32_t noiseSeed = noiseSeed_;

    tr::TerrainSystem::Get().Edit(terrain, [=](tr::TerrainData& d) {
        d.Capture(d.RectAround(x, z, brush.radius + d.spacing), stroke->before);
        const float raise = s * std::max(2.0f, brush.radius) * 0.5f * dt;
        switch (tool) {
            case Tool::Raise:   return d.Raise(x, z, brush, raise);
            case Tool::Lower:   return d.Raise(x, z, brush, -raise);
            case Tool::Smooth:  return d.Smooth(x, z, brush, std::min(1.0f, s * 10.0f * dt));
            case Tool::Flatten: return d.Flatten(x, z, brush, stroke->flattenTarget, std::min(1.0f, s * 8.0f * dt));
            case Tool::Paint:   return d.Paint(x, z, brush, paintLayer, s * 5.0f * dt);
            case Tool::Replace: return d.Replace(x, z, brush, from, to, s * 5.0f * dt);
            case Tool::Noise:   return d.Noise(x, z, brush, s * 3.0f * dt, noiseScale, noiseSeed);
            default:            return tr::SampleRect();
        }
    });
}

void TerrainEditor::EndStroke (EditorHistory& history) {
    if (!stroke_) return;
    auto stroke = stroke_;
    stroke_.reset();
    haveLast_ = false;
    EditorHistory* h = &history;
    // Queued behind the stroke's own dabs, so "after" is captured once they have all run.
    tr::TerrainSystem::Get().Edit(stroke->terrain, [stroke, h](tr::TerrainData& d) {
        if (stroke->before.tiles.empty()) return tr::SampleRect();
        stroke->after = d.CaptureSame(stroke->before);
        const Entity e = stroke->terrain;
        h->PushCustom(stroke->label.c_str(), e, [stroke, e](bool redo) {
            tr::TerrainSystem::Get().Edit(e, [stroke, redo](tr::TerrainData& d2) {
                return d2.Restore(redo ? stroke->after : stroke->before);
            });
        });
        return tr::SampleRect();
    });
}

// --------------------------------------------------------------- viewport ---

bool TerrainEditor::ViewportInput (wi::scene::Scene& scene, EditorHistory& history,
                                   wi::RenderPath3D& path, const wi::scene::CameraComponent& cam,
                                   ImVec2 imagePos, ImVec2 imageSize, bool hovered, float dt) {
    haveHover_ = false;
    if (!open_) {
        if (stroke_) EndStroke(history);
        return false;
    }
    const Entity terrain = CurrentTerrain(scene);
    tr::TerrainData* data = terrain != INVALID_ENTITY ? tr::TerrainSystem::Get().Data(terrain) : nullptr;
    if (data == nullptr) {
        if (stroke_) EndStroke(history);
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imagePos, ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y), true);

    if (tab_ == Tab::Create)
        DrawRegion(dl, *data, cam, imagePos, imageSize, generate_.x, generate_.z, generate_.sizeX, generate_.sizeZ);

    const bool brushing = tab_ == Tab::Edit && tool_ != Tool::None;
    if (!brushing) {
        dl->PopClipRect();
        if (stroke_) EndStroke(history);
        return false;
    }

    // Where the pointer meets the ground - the DATA, not the rendered chunks, so the brush
    // sits on the right spot even before the chunk under it has rebuilt.
    XMFLOAT3 hit;
    bool onGround = false;
    if (hovered) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float u = (m.x - imagePos.x) / std::max(1.0f, imageSize.x);
        const float v = (m.y - imagePos.y) / std::max(1.0f, imageSize.y);
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            const long cx = (long)(u * path.GetLogicalWidth());
            const long cy = (long)(v * path.GetLogicalHeight());
            const wi::primitive::Ray ray = wi::renderer::GetPickRay(cx, cy, path, cam);
            onGround = data->Raycast(ray.origin, ray.direction, 20000.0f, hit);
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  brush_.radius = std::max(0.5f, brush_.radius / 1.15f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) brush_.radius = std::min(500.0f, brush_.radius * 1.15f);
    }

    // The tool this frame: Ctrl smooths, Shift flips raise and lower.
    Tool tool = tool_;
    if (io.KeyCtrl) tool = Tool::Smooth;
    else if (io.KeyShift && tool == Tool::Raise) tool = Tool::Lower;
    else if (io.KeyShift && tool == Tool::Lower) tool = Tool::Raise;

    if (onGround) {
        haveHover_ = true;
        hover_ = hit;
        const ImU32 color = (tool == Tool::Lower) ? IM_COL32(255, 140, 90, 230)
                          : (tool == Tool::Paint) ? IM_COL32(120, 255, 140, 230)
                          : IM_COL32(255, 255, 255, 230);
        DrawBrushOutline(dl, *data, cam, imagePos, imageSize, brush_, hit.x, hit.z, color);
        ImVec2 centre;
        if (Project(cam, hit, imagePos, imageSize, centre))
            dl->AddCircleFilled(centre, 3.0f, color);
    }
    dl->PopClipRect();

    // Alt+click picks the flatten height and does not start a stroke.
    if (hovered && onGround && io.KeyAlt && tool_ == Tool::Flatten && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        flattenHeight_ = hit.y;
        flattenFixed_ = true;
        return true;
    }

    if (hovered && onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt)
        BeginStroke(terrain, tool, hit.x, hit.y, hit.z);

    if (stroke_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            EndStroke(history);
        } else if (onGround) {
            // Space dabs along the path the pointer travelled, so a fast drag leaves a line
            // and not a row of separate stamps.
            const float dx = hit.x - lastHit_.x, dz = hit.z - lastHit_.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float step = std::max(0.25f, brush_.radius * 0.25f);
            const int n = std::max(1, std::min(32, int(std::ceil(dist / step))));
            for (int k = 1; k <= n; ++k) {
                const float t = float(k) / float(n);
                Dab(terrain, tool, lastHit_.x + dx * t, lastHit_.z + dz * t, dt / float(n));
            }
            lastHit_ = hit;
        }
    }

    // While a tool is up the viewport's own left click (select) and the gizmo stay out of it.
    return hovered || stroke_ != nullptr;
}

} // namespace st
