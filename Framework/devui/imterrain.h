#pragma once
// Terrain Editor - the editor panel for st::terrain (Framework/terrain), laid out the way
// Roblox Studio's terrain editor is:
//
//   Create  region box + Generate (biomes, seed, blending), Import (heightmap image or raw),
//           Clear, Fill, Sea Level
//   Edit    brush tools: Raise, Lower, Smooth, Flatten, Paint, Replace, Noise; brush shape,
//           size, strength, hardness
//   Layers  the paint layers: colour, name, grass, and the SOIL each one is made of - the
//           numbers tyres and soft-ground physics read, which is what makes painted mud mud
//
// Brushes work in the Editor Viewport: left-drag to apply, Shift inverts Raise/Lower, Ctrl
// smooths whatever tool is up, [ and ] change the size, Alt+click picks the Flatten height.
// One drag is one undo step. Edits go through TerrainSystem's queue, so they land between
// generator runs and the chunks under the brush rebuild - mesh, physics and paint - a frame
// or two later.

#include "imeditorhistory.h"

#include "wiECS.h"
#include "imgui.h"
#include "terrain/TerrainData.h"
#include "terrain/TerrainGenerate.h"
#include "terrain/TerrainSystem.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace wi::scene { struct Scene; struct CameraComponent; }
namespace wi { class RenderPath3D; }

namespace st {

class TerrainEditor {
public:
    enum class Tool { None, Raise, Lower, Smooth, Flatten, Paint, Replace, Noise };
    enum class Tab  { Create, Edit, Layers };

    // The docked panel.
    void Draw (wi::scene::Scene& scene, EditorHistory& history, wi::ecs::Entity& selected);

    // Called by the Editor Viewport every frame it is drawn. Draws the brush (or the Create
    // region) over the image and applies strokes. Returns true while the terrain tool owns
    // the mouse, so the viewport skips its own click-to-select and gizmo.
    bool ViewportInput (wi::scene::Scene& scene, EditorHistory& history, wi::RenderPath3D& path,
                        const wi::scene::CameraComponent& cam, ImVec2 imagePos, ImVec2 imageSize,
                        bool hovered, float dt);

    bool IsToolActive () const { return tool_ != Tool::None && open_; }
    void SetOpen (bool open) { open_ = open; }

private:
    struct Stroke;

    wi::ecs::Entity CurrentTerrain (wi::scene::Scene& scene);
    void DrawCreateTerrain (wi::scene::Scene& scene, EditorHistory& history, wi::ecs::Entity& selected);
    void DrawCreateTab (wi::scene::Scene& scene, EditorHistory& history, wi::ecs::Entity terrain);
    void DrawEditTab (wi::scene::Scene& scene, wi::ecs::Entity terrain);
    void DrawLayersTab (wi::scene::Scene& scene, wi::ecs::Entity terrain, wi::ecs::Entity& selected);
    void DrawLayerPicker (const char* label, const terrain::TerrainData& data, uint32_t& layer);

    // Queue a region edit (generate, import, clear, fill) as ONE undo step.
    void RegionEdit (wi::ecs::Entity terrain, EditorHistory& history, const char* label,
                     terrain::SampleRect rect,
                     std::function<terrain::SampleRect(terrain::TerrainData&)> op);
    void BeginStroke (wi::ecs::Entity terrain, Tool tool, float x, float y, float z);
    void Dab (wi::ecs::Entity terrain, Tool tool, float x, float z, float dt);
    void EndStroke (EditorHistory& history);

    bool open_ = true;
    Tab  tab_  = Tab::Edit;
    Tool tool_ = Tool::None;
    wi::ecs::Entity terrain_ = wi::ecs::INVALID_ENTITY;

    // brush
    terrain::Brush brush_;
    uint32_t paintLayer_   = 0;
    uint32_t replaceFrom_  = 0;
    uint32_t replaceTo_    = 1;
    bool     flattenFixed_ = false;
    float    flattenHeight_ = 0.0f;
    float    noiseScale_   = 12.0f;
    uint32_t noiseSeed_    = 7;

    // stroke in progress
    std::shared_ptr<Stroke> stroke_;
    Tool     strokeTool_ = Tool::None;
    bool     haveLast_ = false;
    XMFLOAT3 lastHit_ = XMFLOAT3(0, 0, 0);
    bool     haveHover_ = false;
    XMFLOAT3 hover_ = XMFLOAT3(0, 0, 0);

    // create
    terrain::CreateParams      create_;
    terrain::GenerateParams    generate_;
    terrain::HeightmapImport   import_;
    float    fillHeight_ = 0.0f;
    int      fillLayer_  = 0;
    bool     fillPaint_  = true;
    float    seaLevel_   = 0.0f;
    std::string status_;
    // The file dialog answers on its own thread; the path is parked here for the next Draw.
    std::mutex  dialogMutex_;
    std::string dialogPath_;
    bool     regionFromImport_ = false; // the region box shows the import rectangle

    // layers
    std::vector<terrain::TextureSet> textureSets_;
    float    tileEdit_ = 2.0f;          // tile size while its slider is held (applied on release)
    int      tileEditLayer_ = -1;
    terrain::GrassSettings grassEdit_;  // what the Grass section shows while a control is held
    bool     grassEditing_ = false;
    std::vector<std::string> propModels_;   // models in the packages, for the prop picker
    std::string propModel_;
};

} // namespace st
