#pragma once
// MeshLOD - generate the level-of-detail meshes the engine already knows how to draw.
//
// The engine has had LODs all along: a MeshComponent with `subsets_per_lod` set holds every
// level's subsets back to back (LOD0's, then LOD1's, ...), all indexing the same vertex
// buffer, and Scene::ComputeObjectLODForView picks one per view - main camera, every shadow
// cascade, every reflection - as log2(1 / screen fraction) + lod_bias. So each halving of an
// object's size on screen steps down one level. What was missing is anything that MAKES
// the levels: an imported model came in with LOD0 only and drew every triangle at any
// distance, including into four shadow cascades.
//
// GenerateMeshLODs builds them with meshoptimizer's simplifier (already compiled into the
// engine for meshlets): each level keeps `reduction` of the previous level's triangles,
// subset by subset, so materials stay exactly where they were. Vertices are shared - only
// the index buffer grows, by roughly reduction / (1 - reduction) of LOD0 (100% for 0.5),
// which is the price of the saving and far smaller than the vertex data.
//
// A level that the simplifier cannot shrink below `stopRatio` of the previous one stops the
// chain there: a 12-triangle crate gains nothing from four LODs.
//
// The result is ordinary mesh data - it saves with the map or the template, no runtime cost
// beyond the selection the engine already does.
//
// No exceptions, no RTTI.

#include "wiScene.h"

#include <cstdint>
#include <string>

namespace st::render {

struct MeshLODSettings {
    uint32_t lodCount    = 4;      // levels including LOD0 (2..8)
    float    reduction   = 0.5f;   // triangles kept per level, relative to the previous one
    float    targetError = 0.02f;  // max deviation, relative to the mesh extent (0.02 = 2%)
    float    stopRatio   = 0.85f;  // a level keeping more than this of the previous one ends the chain
    bool     lockBorder  = false;  // keep open edges in place (seams between separate meshes)
    bool     sloppyFallback = true; // when the precise simplifier stalls, try the cluster-collapse one
};

struct MeshLODReport {
    uint32_t lods          = 1;    // levels the mesh has now
    uint64_t lod0Triangles = 0;
    uint64_t lastTriangles = 0;    // triangles in the smallest level
    bool     skipped       = false; // nothing to do (no CPU data, too small)
    std::string reason;
};

// Rebuild `mesh`'s LOD chain from its LOD0 (existing extra levels are discarded first) and
// re-create its GPU data. False when the mesh has no CPU-side indices or positions.
bool GenerateMeshLODs (wi::scene::MeshComponent& mesh, const MeshLODSettings& settings = MeshLODSettings{},
                       MeshLODReport* report = nullptr);

// Drop every level but LOD0.
void RemoveMeshLODs (wi::scene::MeshComponent& mesh);

// Every mesh in the scene with at least `minTriangles` triangles in LOD0. Returns how many
// meshes ended up with more than one level.
uint32_t GenerateSceneLODs (wi::scene::Scene& scene, const MeshLODSettings& settings = MeshLODSettings{},
                            uint32_t minTriangles = 300);

} // namespace st::render
