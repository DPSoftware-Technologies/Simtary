#pragma once
// Region generators for the Terrain Editor's Create tab: procedural biomes and heightmap
// import. Both WRITE INTO TerrainData once and are done - the result is ordinary authored
// terrain that every brush can keep editing, which is the difference from the engine's own
// generator, whose noise stack is re-evaluated (and so cannot be edited) chunk by chunk.

#include "terrain/TerrainData.h"

#include <cstdint>
#include <string>

namespace st::terrain {

enum class Biome : uint32_t {
    Plains,
    Hills,
    Mountains,
    Dunes,
    Canyons,
    Marsh,
    Lakes,
    Count
};

const char* ToString (Biome biome);

struct GenerateParams {
    // Region, world metres. Samples outside it are not touched.
    float x = -256.0f, z = -256.0f;
    float sizeX = 512.0f, sizeZ = 512.0f;

    uint32_t seed      = 1337;
    bool     biomes[uint32_t(Biome::Count)] = { true, true, true, false, false, false, false };
    float    biomeSize = 500.0f; // metres across one biome cell
    float    blending  = 0.25f;  // 0 = hard borders, 1 = everything mixed
    float    height    = 200.0f; // metres a full-height mountain reaches above `base`
    float    warp      = 0.5f;   // domain warp: 0 = raw noise, 1 = strongly folded shapes
    // Hydraulic erosion: water droplets run downhill, carve channels and drop sediment in
    // the valleys. This is what turns noise into ground that looks like it has had weather.
    // 0 = off, 1 = one droplet per sample (slow on big regions: ~1-2 s per 512 x 512).
    float    erosion   = 0.6f;
    bool     thermal   = true;   // collapse slopes steeper than ~38 degrees (scree)
    float    base      = 0.0f;   // ground level the biomes sit on
    float    edgeFade  = 0.1f;   // fraction of the region over which it fades into what was there
    bool     autoPaint = true;   // grass / dirt / rock / sand by slope and height
    float    waterLevel = 0.0f;  // sand below this (only when autoPaint)
};

// Fill the region. Returns the samples written.
SampleRect Generate (TerrainData& data, const GenerateParams& params);

struct HeightmapImport {
    std::string path;          // .png / .jpg / .tga / .bmp (8 or 16 bit), .r16 / .raw (16-bit LE), .r32 (float)
    // World rectangle the image covers: (x, z) is its minimum corner. The image's top row
    // is the +Z (north) edge, the way a map is read; flipY turns that around.
    float x = -512.0f, z = -512.0f;
    float sizeX = 1024.0f, sizeZ = 1024.0f;
    float minHeight = 0.0f;    // metres black maps to
    float maxHeight = 200.0f;  // metres white maps to (ignored for .r32, which is metres already)
    bool  flipY = false;
    bool  autoPaint = true;
    float waterLevel = 0.0f;
};

// Read the image and resample it into the region. `error` says why on failure.
bool ImportHeightmap (TerrainData& data, const HeightmapImport& params, SampleRect& written,
                      std::string& error);

} // namespace st::terrain
