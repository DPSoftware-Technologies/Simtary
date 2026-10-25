#pragma once
// TerrainSystem - binds st::terrain::TerrainData to the engine's wi::terrain::Terrain.
//
// The engine terrain keeps doing what it is good at: it builds render meshes, LODs, the
// blended virtual texture, grass, props and Jolt heightfields chunk by chunk around the
// camera. This system makes it build them from AUTHORED data instead of from noise:
//
//   - every terrain whose authored_data holds a TerrainData blob gets a wi::terrain::Source
//     that answers height and paint from that data (installed again after every load)
//   - edits are queued and run only while the engine's generator is idle, because the
//     generator reads the data from its job threads; the chunks an edit touched are then
//     invalidated so they regenerate - mesh, physics and texture together
//   - Flush() writes the data back into authored_data, which is what makes it save with the
//     scene. The editor calls it before every save.
//
// It is also the GROUND QUERY every physics feature uses (tyres, soil, liquid): Sample()
// gives height, normal and paint at a point, Soil() the physical parameters of a layer.
// Those read the data directly, so they are right even where no chunk has been built.
//
// st::App calls Update() once a frame, BEFORE the scene update, so an edit made this frame
// is in the data before the generator is kicked off again.

#include "terrain/TerrainData.h"

#include "wiECS.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wi::scene { struct Scene; }
namespace wi::terrain { struct Terrain; struct Source; }

namespace st::terrain {

// What the ground is at one point.
struct GroundSample {
    float    height = 0.0f;
    XMFLOAT3 normal = XMFLOAT3(0, 1, 0);
    uint32_t dominantLayer = 0;
    uint32_t layerCount = 0;
    float    weights[kMaxLayers] = {}; // normalised paint, sums to 1
};

// The textures that cover one layer, by logical asset path ("thirdparty/x/rock_diff.jpg").
// They live on the layer's material, so they save with the map like any material.
struct LayerTextures {
    std::string baseColor;        // colour
    std::string normal;           // normal map, +Y up (OpenGL / glTF)
    std::string surface;          // R occlusion, G roughness, B metalness (glTF "ARM")
    float       tileSize = 2.0f;  // metres one repeat covers; 0 = the engine's own rule
};

// A texture set found in the mounted packages: "<dir>/<name>_diff.<ext>" plus whichever of
// "_nor_gl" / "_arm" sit beside it (Poly Haven's names).
struct TextureSet {
    std::string   name;           // "<name>"
    LayerTextures textures;
};

// The engine's grass: camera-facing cards on every chunk near the camera, where a layer marked
// `grass` is painted. Saved with the map (the terrain's grass entity).
struct GrassSettings {
    bool        enabled = false;
    std::string texture;              // card atlas, transparent background, root at the bottom
    int         columns = 2;          // frames in the atlas: columns x rows, picked at random
    int         rows = 2;
    float       height = 0.5f;        // metres, before random variation
    float       randomness = 0.4f;    // 0 = every card the same height
    float       density = 1.0f;       // cards per vertex, relative
    float       viewDistance = 60.0f; // metres; beyond it no grass is drawn
    int         crossCards = 2;       // cards per tuft, crossed (1 = flat cards)
    float       stiffness = 3.0f;     // spring back against the weather wind (the engine's 0.5 lays grass flat in gusts)
};

// A model the engine scatters over the terrain - trees, bushes, rocks - chunk by chunk around
// the camera, where one paint layer is. Every copy shares the model's mesh (with its LODs).
// Saved with the map.
struct PropSettings {
    std::string name;             // the model it came from (set by AddProp)
    uint32_t    layer = 0;        // grows where this paint layer is
    int         minPerChunk = 0;  // a chunk (66 samples square) tries this many..
    int         maxPerChunk = 4;  // ..up to this many
    float       threshold = 0.4f; // 0..1: higher = fewer, only where paint and noise are strongest
    float       clumping = 0.03f; // noise frequency: low = big groves and clearings, high = even
    float       minSize = 0.8f;   // random scale range
    float       maxSize = 1.2f;
    float       sink = 0.0f;      // metres pushed into the ground at most (roots on a slope)
};

struct CreateParams {
    std::string name    = "Terrain";
    float       spacing = 1.0f;   // metres between samples; also the engine's chunk scale
    float       baseHeight = 0.0f;
    bool        grass   = false;  // engine grass (needs a grass material to look right)
    int         generationRadius = 8; // chunks around the camera
};

class TerrainSystem {
public:
    static TerrainSystem& Get ();

    // Once a frame, before the scene update. `sceneDt` is the delta the scene is about to
    // get: at 0 (paused) the engine skips terrain generation entirely, so while a focus is
    // set this pumps it instead - sculpting a paused world still shows the result.
    void Update (wi::scene::Scene& scene, float sceneDt);

    // Install the authored source on every terrain that came out of a file. Update() does
    // this too, but it runs BEFORE the scene update, and a scene that loads also updates in
    // that same frame - so st::App calls this straight after a load, or the engine would
    // build the first chunks from its own noise.
    void BindAll (wi::scene::Scene& scene);

    // Build chunks around `position` instead of the scene camera (the editor's free camera).
    void SetFocus (bool enabled, const XMFLOAT3& position = XMFLOAT3(0, 0, 0));

    // A new authored terrain with the default four layers, flat at baseHeight.
    wi::ecs::Entity CreateTerrain (wi::scene::Scene& scene, const CreateParams& params);
    // The first authored terrain in the scene, or INVALID_ENTITY.
    wi::ecs::Entity FindTerrain (const wi::scene::Scene& scene) const;
    bool IsAuthored (wi::ecs::Entity terrain) const;

    // The data behind an authored terrain. Read it freely; WRITE it only from inside Edit().
    TerrainData* Data (wi::ecs::Entity terrain);

    // Run `op` on the terrain's data as soon as the generator is idle (this frame if it
    // already is). `op` returns the samples it changed. Those chunks are updated IN PLACE
    // (heights, normals, physics, the touched texture tiles), which is what a brush wants:
    // a full rebuild throws the chunk's texture away and it renders black until it comes
    // back. `rebuild` asks for the full path instead - for big region edits, where it is
    // also faster, and for anything that changes the layer count.
    using EditFn = std::function<SampleRect(TerrainData&)>;
    void Edit (wi::ecs::Entity terrain, EditFn op, bool rebuild = false);
    // Edits still waiting for the generator.
    size_t PendingEdits () const;

    // Layers. Each is a paint channel in the data AND a material entity in the engine's
    // list, so both change together - queued like any edit, since the generator reads both.
    void AddLayer (wi::scene::Scene& scene, wi::ecs::Entity terrain, const Layer& layer);
    void RemoveLayer (wi::scene::Scene& scene, wi::ecs::Entity terrain, uint32_t index);
    // The material that renders a layer, INVALID_ENTITY if it has none.
    wi::ecs::Entity LayerMaterial (const wi::scene::Scene& scene, wi::ecs::Entity terrain,
                                   uint32_t layer) const;
    // A layer's colour changed: push it into its material (which rebuilds the chunks).
    void SyncLayerMaterial (wi::scene::Scene& scene, wi::ecs::Entity terrain, uint32_t layer);
    // A layer's textures, read back from its material.
    LayerTextures GetLayerTextures (const wi::scene::Scene& scene, wi::ecs::Entity terrain,
                                    uint32_t layer) const;
    // Put textures on a layer's material (the chunks rebuild their texture). Setting a colour
    // texture turns the layer's tint white, or the tint would darken it.
    void SetLayerTextures (wi::scene::Scene& scene, wi::ecs::Entity terrain, uint32_t layer,
                           const LayerTextures& textures);
    GrassSettings GetGrass (const wi::scene::Scene& scene, wi::ecs::Entity terrain) const;
    // Apply grass settings; every chunk rebuilds its grass.
    void SetGrass (wi::scene::Scene& scene, wi::ecs::Entity terrain, const GrassSettings& grass);

    std::vector<PropSettings> GetProps (const wi::scene::Scene& scene, wi::ecs::Entity terrain) const;
    // Import `modelPath` (a logical path - .glb/.gltf/.fbx/.obj) and scatter it. A model with
    // several objects contributes its biggest one. LODs are generated for its meshes.
    bool AddProp (wi::scene::Scene& scene, wi::ecs::Entity terrain, const std::string& modelPath,
                  const PropSettings& settings, std::string* error = nullptr);
    void UpdateProp (wi::scene::Scene& scene, wi::ecs::Entity terrain, uint32_t index, const PropSettings& settings);
    void DeleteProp (wi::scene::Scene& scene, wi::ecs::Entity terrain, uint32_t index);

    // Every texture set in the mounted packages, sorted by name.
    static std::vector<TextureSet> FindTextureSets ();
    // Metres one repeat covers -> repeats per chunk (whole numbers, so chunks meet seamlessly).
    static float TilingFor (float tileSize, float spacing);

    // Mark the data changed outside Edit() (a layer's soil settings, say), so Flush saves it.
    void MarkDirty (wi::ecs::Entity terrain);

    // Write every authored terrain's data into its component, ready to serialize.
    void Flush (wi::scene::Scene& scene);

    // ground queries
    bool Sample (float x, float z, GroundSample& out) const;
    const SoilParams* Soil (uint32_t layer) const;
    bool Raycast (const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                  XMFLOAT3& hit) const;

    // Last load / decode problem, for the editor panel. Empty when there was none.
    const std::string& LastError () const { return lastError_; }

private:
    struct Binding;
    Binding* EnsureBound (wi::scene::Scene& scene, size_t index);
    Binding* Find (wi::ecs::Entity terrain);
    const Binding* Find (wi::ecs::Entity terrain) const;
    Binding* Bind (wi::scene::Scene& scene, wi::ecs::Entity entity, wi::terrain::Terrain& terrain,
                   std::shared_ptr<TerrainData> data);
    void Invalidate (wi::terrain::Terrain& terrain, const TerrainData& data, const SampleRect& r,
                     bool rebuild);
    void FitLevels (wi::terrain::Terrain& terrain, const TerrainData& data, const SampleRect& r);

    struct Binding {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        std::shared_ptr<TerrainData> data;
        std::shared_ptr<wi::terrain::Source> source;
        size_t flushedHash = 0;   // hash of the bytes last written into authored_data
        bool   dirty = false;     // data changed since the last Flush
        std::vector<EditFn> pending;
        bool   pendingRebuild = false; // any queued op asked for the full path
        wi::ecs::Entity chunkGroup = wi::ecs::INVALID_ENTITY; // the engine's "chunks" entity
    };
    std::vector<std::unique_ptr<Binding>> bindings_;
    std::string lastError_;
    bool     focus_ = false;
    XMFLOAT3 focusPosition_ = XMFLOAT3(0, 0, 0);
};

} // namespace st::terrain
