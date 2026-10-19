#include "render/MeshLOD.h"
#include "Utility/meshoptimizer/meshoptimizer.h"
#include "wiBacklog.h"

#include <algorithm>
#include <vector>

using namespace wi::scene;

namespace st::render {

namespace {

uint64_t TriangleCount (const MeshComponent& mesh, uint32_t firstSubset, uint32_t lastSubset) {
    uint64_t indices = 0;
    for (uint32_t i = firstSubset; i < lastSubset && i < mesh.subsets.size(); ++i)
        indices += mesh.subsets[i].indexCount;
    return indices / 3;
}

} // namespace

void RemoveMeshLODs (MeshComponent& mesh) {
    if (mesh.subsets_per_lod == 0 || mesh.GetLODCount() <= 1) {
        mesh.subsets_per_lod = 0;
        return;
    }
    // LOD0's subsets, with their indices compacted to the front of a fresh buffer: the
    // other levels' indices may sit anywhere in the old one.
    const uint32_t count = mesh.subsets_per_lod;
    wi::vector<uint32_t> indices;
    wi::vector<MeshComponent::MeshSubset> subsets(mesh.subsets.begin(), mesh.subsets.begin() + count);
    for (MeshComponent::MeshSubset& s : subsets) {
        const uint32_t offset = (uint32_t)indices.size();
        indices.insert(indices.end(), mesh.indices.begin() + s.indexOffset,
                       mesh.indices.begin() + s.indexOffset + s.indexCount);
        s.indexOffset = offset;
    }
    mesh.indices = std::move(indices);
    mesh.subsets = std::move(subsets);
    mesh.subsets_per_lod = 0;
}

bool GenerateMeshLODs (MeshComponent& mesh, const MeshLODSettings& settingsIn, MeshLODReport* report) {
    MeshLODReport local;
    MeshLODReport& r = report ? *report : local;
    r = MeshLODReport{};

    MeshLODSettings settings = settingsIn;
    settings.lodCount  = std::clamp(settings.lodCount, 2u, 8u);
    settings.reduction = std::clamp(settings.reduction, 0.05f, 0.95f);

    if (mesh.indices.empty() || mesh.vertex_positions.empty() || mesh.subsets.empty()) {
        r.skipped = true;
        r.reason = "no CPU-side geometry";
        return false;
    }

    RemoveMeshLODs(mesh);
    const uint32_t perLod = (uint32_t)mesh.subsets.size();
    r.lod0Triangles = TriangleCount(mesh, 0, perLod);
    r.lastTriangles = r.lod0Triangles;

    const float* positions = &mesh.vertex_positions[0].x;
    const size_t vertexCount = mesh.vertex_positions.size();
    const unsigned int options = settings.lockBorder ? meshopt_SimplifyLockBorder : 0u;

    // Each level simplifies the PREVIOUS one, not LOD0: cheaper, and the error then
    // accumulates the way the screen-size rule expects.
    std::vector<std::vector<uint32_t>> previous(perLod);
    for (uint32_t s = 0; s < perLod; ++s) {
        const MeshComponent::MeshSubset& subset = mesh.subsets[s];
        previous[s].assign(mesh.indices.begin() + subset.indexOffset,
                           mesh.indices.begin() + subset.indexOffset + subset.indexCount);
    }

    std::vector<uint32_t> scratch;
    for (uint32_t lod = 1; lod < settings.lodCount; ++lod) {
        std::vector<std::vector<uint32_t>> level(perLod);
        uint64_t before = 0, after = 0;
        // Each level may deviate more: the object is half the size on screen.
        const float error = settings.targetError * (float)(1u << (lod - 1));
        for (uint32_t s = 0; s < perLod; ++s) {
            const std::vector<uint32_t>& src = previous[s];
            before += src.size() / 3;
            if (src.size() < 3) {
                level[s] = src;
                after += src.size() / 3;
                continue;
            }
            size_t target = (size_t)(src.size() * settings.reduction) / 3 * 3;
            target = std::max<size_t>(target, 3);
            scratch.resize(src.size());
            size_t written = meshopt_simplify(scratch.data(), src.data(), src.size(), positions, vertexCount,
                                              sizeof(XMFLOAT3), target, error, options, nullptr);
            if (settings.sloppyFallback && written > src.size() * settings.stopRatio && src.size() > 3 * 64) {
                // Topology the edge collapser will not touch (disconnected pieces, seams on
                // every edge). The sloppy simplifier clusters vertices instead; fine at the
                // distance a lower level is seen from.
                written = meshopt_simplifySloppy(scratch.data(), src.data(), src.size(), positions, vertexCount,
                                                 sizeof(XMFLOAT3), nullptr, target, error, nullptr);
            }
            if (written == 0) {
                // Collapsed to nothing: keep one level's worth of the previous geometry
                // rather than make the subset vanish at a distance.
                level[s] = src;
            } else {
                level[s].assign(scratch.begin(), scratch.begin() + written);
            }
            after += level[s].size() / 3;
        }
        if (before == 0 || (double)after > (double)before * settings.stopRatio)
            break; // no meaningful saving left
        for (uint32_t s = 0; s < perLod; ++s) {
            MeshComponent::MeshSubset subset = mesh.subsets[s];
            subset.indexOffset = (uint32_t)mesh.indices.size();
            subset.indexCount  = (uint32_t)level[s].size();
            mesh.indices.insert(mesh.indices.end(), level[s].begin(), level[s].end());
            mesh.subsets.push_back(subset);
        }
        r.lastTriangles = after;
        previous = std::move(level);
    }

    mesh.subsets_per_lod = (mesh.subsets.size() > perLod) ? perLod : 0;
    r.lods = mesh.GetLODCount();
    if (r.lods <= 1) {
        r.skipped = true;
        r.reason = "simplifier found nothing to remove";
    }
    mesh.CreateRenderData();
    return true;
}

uint32_t GenerateSceneLODs (Scene& scene, const MeshLODSettings& settings, uint32_t minTriangles) {
    uint32_t withLods = 0;
    uint64_t lod0 = 0, last = 0;
    for (size_t i = 0; i < scene.meshes.GetCount(); ++i) {
        MeshComponent& mesh = scene.meshes[i];
        uint32_t first = 0, end = 0;
        mesh.GetLODSubsetRange(0, first, end);
        if (TriangleCount(mesh, first, end) < minTriangles) continue;
        MeshLODReport report;
        if (GenerateMeshLODs(mesh, settings, &report) && report.lods > 1) {
            ++withLods;
            lod0 += report.lod0Triangles;
            last += report.lastTriangles;
        }
    }
    wi::backlog::post("MeshLOD: " + std::to_string(withLods) + " mesh(es) now have LODs (" +
                      std::to_string(lod0) + " triangles at LOD0 -> " + std::to_string(last) +
                      " at their lowest level)");
    return withLods;
}

} // namespace st::render
