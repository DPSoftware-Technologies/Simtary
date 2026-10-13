#pragma once
// TerrainData - the authored terrain itself: a height and a set of paint layers per sample.
//
// The engine's wi::terrain::Terrain is a chunk GENERATOR: give it noise and it builds meshes,
// grass, physics and a virtual texture around the camera, but what it generated is output,
// not data, so there is nothing for a brush to change. This is the data it was missing. The
// engine reads it back through wi::terrain::Source (installed by st::terrain::TerrainSystem),
// so every chunk it builds - render mesh, Jolt heightfield, blend layers - comes from here.
//
// Layout: a sparse grid of samples, `spacing` metres apart, sample (i, j) sitting at world
// (i * spacing, j * spacing). Samples are grouped into kTileSize x kTileSize tiles that only
// exist where something was authored; every other sample reads `baseHeight` and the first
// layer. That is what lets a 100 km world cost nothing until someone paints on it.
//
// Per sample:
//   height   float metres
//   weights  one byte per layer, normalised so they sum to 255 (the way Unity's alphamaps
//            work: painting a layer takes weight from the others)
//
// The engine's blend shader stacks layers top-down like decals instead, so the normalised
// weights are converted on the way out (StackedWeights) - nothing here needs to know that.
//
// Thread safety: the engine's generator reads this from job threads. Writers must only run
// while that generator is idle, which is what TerrainSystem's edit queue guarantees.

#include "wiMath.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::terrain {

constexpr int      kTileSize  = 128;  // samples per tile edge
constexpr uint32_t kMinLayers = 4;    // the engine's prop placement indexes four regions
constexpr uint32_t kMaxLayers = 16;

// What physics reads about the ground under a wheel. One set per layer, so painting mud
// makes ground that behaves like mud, not just ground that looks like it. Nothing reads the
// Bekker-Wong half yet; it is here so the soil model has a contract to build against.
struct SoilParams {
    float friction          = 0.9f;   // peak tyre friction coefficient
    float rollingResistance = 0.015f; // rolling resistance coefficient
    // Bekker-Wong pressure-sinkage, p = (kc / b + kphi) * z^n
    float kc            = 0.0f;  // cohesive modulus,   kN/m^(n+1)
    float kphi          = 0.0f;  // frictional modulus, kN/m^(n+2)
    float n             = 1.0f;  // sinkage exponent
    float cohesion      = 0.0f;  // kPa
    float frictionAngle = 30.0f; // degrees
    float absorption    = 0.0f;  // how fast standing liquid soaks in, 0..1 per second
    bool  deformable    = false; // wheels leave ruts in it
};

struct Layer {
    std::string name;
    XMFLOAT4    color = XMFLOAT4(1, 1, 1, 1); // the base colour a new layer's material gets
    bool        grass = false;                // engine grass grows where this layer is painted
    SoilParams  soil;
};

// The default four, in order: Grass, Dirt, Rock, Sand. Generate's auto-paint assumes them.
std::vector<Layer> DefaultLayers ();
// A soil preset by name, for the layer panel ("Asphalt", "Mud", ...). False if unknown.
bool SoilPreset (const char* name, SoilParams& out);
const char* const* SoilPresetNames (size_t& count);

// Inclusive rectangle in SAMPLE coordinates.
struct SampleRect {
    int i0 = 0, j0 = 0, i1 = -1, j1 = -1;
    bool Empty () const { return i1 < i0 || j1 < j0; }
    void Merge (const SampleRect& o);
};

enum class BrushShape { Circle, Square };

struct Brush {
    BrushShape shape    = BrushShape::Circle;
    float      radius   = 8.0f; // metres
    float      strength = 0.5f; // 0..1
    float      hardness = 0.3f; // 0 = soft all the way in, 1 = hard edge
};

// What one tile holds. `weights` is interleaved: sample s, layer l at [s * layerCount + l].
struct Tile {
    std::vector<float>   heights;
    std::vector<uint8_t> weights;
};

// Tiles as they were before an edit, for undo. A tile that did not exist yet is recorded as
// an empty optional, so undoing it removes the tile instead of writing a flat one.
struct TileSnapshot {
    std::unordered_map<uint64_t, std::optional<Tile>> tiles;
    uint32_t layerCount = 0;
};

class TerrainData {
public:
    float              spacing    = 1.0f; // metres between samples
    float              baseHeight = 0.0f; // height wherever no tile exists
    std::vector<Layer> layers = DefaultLayers();

    uint32_t LayerCount () const { return uint32_t(layers.size()); }

    // sample space
    int   ToSample (float world) const;     // nearest sample index
    float ToWorld (int sample) const { return float(sample) * spacing; }
    SampleRect RectAround (float x, float z, float radius) const;
    SampleRect RectFromWorld (float x0, float z0, float x1, float z1) const;
    void  WorldBounds (const SampleRect& r, XMFLOAT2& min, XMFLOAT2& max) const;

    // reading
    float HeightAt (int i, int j) const;
    // Normalised 0..1 weights for every layer at a sample.
    void  WeightsAt (int i, int j, float* out) const;
    float SampleHeight (float x, float z) const;           // bilinear
    void  SampleWeights (float x, float z, float* out) const;
    XMFLOAT3 SampleNormal (float x, float z) const;
    // Highest-weighted layer at a world position.
    uint32_t DominantLayer (float x, float z) const;
    // Convert normalised weights to the engine's top-down stacking (see the file header).
    static void StackedWeights (const float* normalised, float* stacked, uint32_t count);

    // Cast a ray against the heightfield. Returns false on a miss within maxDistance.
    bool Raycast (const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                  XMFLOAT3& hit) const;

    // writing - these create the tile when it does not exist
    float*   HeightPtr (int i, int j);
    // Same, but never creates: nullptr where no tile exists. The one form that is safe from
    // several threads at once, once the tiles have been made up front.
    float*   FindHeightPtr (int i, int j);
    uint8_t* WeightPtr (int i, int j);
    // Normalise a float weight set and store it as bytes summing to 255.
    void SetWeights (int i, int j, const float* weights);

    // layers
    // Appends a layer; existing samples get zero weight for it.
    bool AddLayer (const Layer& layer);
    // Removes a layer and hands its weight to layer 0. Never goes below kMinLayers.
    bool RemoveLayer (uint32_t index);

    // brush edits - each returns the samples it touched (empty when nothing changed)
    float BrushWeight (const Brush& brush, float dx, float dz) const;
    SampleRect Raise (float x, float z, const Brush& brush, float metres);
    SampleRect Smooth (float x, float z, const Brush& brush, float amount);
    SampleRect Flatten (float x, float z, const Brush& brush, float target, float amount);
    SampleRect Paint (float x, float z, const Brush& brush, uint32_t layer, float amount);
    SampleRect Replace (float x, float z, const Brush& brush, uint32_t from, uint32_t to, float amount);
    SampleRect Noise (float x, float z, const Brush& brush, float metres, float scale, uint32_t seed);

    // region edits
    SampleRect FillRegion (const SampleRect& r, float height, int layer);  // layer < 0 keeps paint
    SampleRect ClearRegion (const SampleRect& r);
    // Paint by slope and height using the default four layers (Grass, Dirt, Rock, Sand).
    SampleRect AutoPaint (const SampleRect& r, float sandBelow, float rockSlopeDegrees);

    // undo
    // Copy every tile overlapping `r` into `snap`, unless it is already there. Call before
    // the edit; the first capture of a tile within one stroke is the one that counts.
    void Capture (const SampleRect& r, TileSnapshot& snap) const;
    // Copy the CURRENT state of exactly the tiles `before` holds.
    TileSnapshot CaptureSame (const TileSnapshot& before) const;
    // Put the tiles in `snap` back. Returns the samples it touched.
    SampleRect Restore (const TileSnapshot& snap);

    // persistence - a versioned, zstd-compressed blob, stored in Terrain::authored_data
    void Encode (std::vector<uint8_t>& out) const;
    static std::shared_ptr<TerrainData> Decode (const uint8_t* data, size_t size, std::string& error);
    static bool LooksLikeTerrainData (const uint8_t* data, size_t size);

    size_t TileCount () const { return tiles_.size(); }
    // Height range of everything authored (baseHeight when nothing is).
    void HeightRange (float& lo, float& hi) const;

private:
    static uint64_t Key (int ti, int tj);
    static void SplitKey (uint64_t key, int& ti, int& tj);
    static int TileIndex (int sample); // floor division by kTileSize
    const Tile* FindTile (int ti, int tj) const;
    Tile& GetOrCreateTile (int ti, int tj);

    std::unordered_map<uint64_t, Tile> tiles_;
};

} // namespace st::terrain
