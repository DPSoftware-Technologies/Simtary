// Standalone test for the authored terrain data (Framework/terrain): sample addressing across
// negative tiles, brushes, paint normalisation, the stacked-weight conversion the engine's
// blend shader needs, undo snapshots, layer add/remove, the save blob round trip, raycasts,
// the biome generator and heightmap import. Built as `terrain_data_test` under the same
// /EHsc- /GR- flags as a game; returns the number of failed checks (0 = success).
#include "terrain/TerrainData.h"
#include "terrain/TerrainGenerate.h"

#include "wiJobSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace st::terrain;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } } while (0)

static bool Near (float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static float WeightSum (const TerrainData& d, int i, int j) {
    float w[kMaxLayers];
    d.WeightsAt(i, j, w);
    float s = 0.0f;
    for (uint32_t l = 0; l < d.LayerCount(); ++l) s += w[l];
    return s;
}

int main () {
    wi::jobsystem::Initialize();

    // empty data
    {
        TerrainData d;
        d.baseHeight = 3.0f;
        CHECK(d.LayerCount() == 4, "default layer count");
        CHECK(Near(d.HeightAt(0, 0), 3.0f), "base height where no tile exists");
        CHECK(Near(d.SampleHeight(-1234.5f, 987.25f), 3.0f), "sampled base height");
        float w[kMaxLayers];
        d.WeightsAt(-5, 7, w);
        CHECK(Near(w[0], 1.0f) && Near(w[1], 0.0f), "default paint is layer 0");
        CHECK(d.TileCount() == 0, "reading creates no tiles");
    }

    // brushes, including across the negative tile boundary
    {
        TerrainData d;
        Brush b;
        b.radius = 5.0f;
        b.hardness = 0.5f;
        d.Raise(0.0f, 0.0f, b, 2.0f);
        CHECK(Near(d.HeightAt(0, 0), 2.0f), "raise centre gets full amount");
        CHECK(Near(d.HeightAt(10, 0), 0.0f), "raise outside radius untouched");
        CHECK(d.HeightAt(4, 0) > 0.0f && d.HeightAt(4, 0) < 2.0f, "raise falls off");
        CHECK(Near(d.SampleHeight(0.5f, 0.0f), 0.5f * (d.HeightAt(0, 0) + d.HeightAt(1, 0))), "bilinear midpoint");

        d.Raise(-130.0f, -130.0f, b, 1.0f);
        CHECK(Near(d.HeightAt(-130, -130), 1.0f), "negative coordinates address the right tile");
        CHECK(Near(d.HeightAt(-129, -130), d.HeightAt(-131, -130), 0.05f), "negative tile symmetric");

        d.Flatten(0.0f, 0.0f, b, 0.5f, 1.0f);
        CHECK(Near(d.HeightAt(0, 0), 0.5f), "flatten at full amount reaches target");

        TerrainData s;
        Brush big; big.radius = 3.0f; big.hardness = 1.0f;
        s.Raise(0.0f, 0.0f, big, 4.0f);
        const float before = s.HeightAt(3, 0) - s.HeightAt(2, 0);
        s.Smooth(0.0f, 0.0f, big, 1.0f);
        s.Smooth(0.0f, 0.0f, big, 1.0f);
        const float after = s.HeightAt(3, 0) - s.HeightAt(2, 0);
        CHECK(std::fabs(after) <= std::fabs(before) + 1e-4f, "smooth reduces a step");
    }

    // paint
    {
        TerrainData d;
        Brush b; b.radius = 4.0f; b.hardness = 1.0f;
        d.Paint(0.0f, 0.0f, b, 2, 1.0f);
        float w[kMaxLayers];
        d.WeightsAt(0, 0, w);
        CHECK(w[2] > 0.99f, "paint reaches the layer");
        CHECK(Near(WeightSum(d, 0, 0), 1.0f, 0.005f), "painted weights still sum to 1");
        d.Paint(0.0f, 0.0f, b, 1, 0.3f);
        CHECK(Near(WeightSum(d, 0, 0), 1.0f, 0.005f), "second paint still sums to 1");
        d.Replace(0.0f, 0.0f, b, 2, 3, 1.0f);
        d.WeightsAt(0, 0, w);
        CHECK(w[2] < 0.01f && w[3] > 0.6f, "replace moves weight between layers");
        CHECK(d.DominantLayer(0.0f, 0.0f) == 3, "dominant layer after replace");
    }

    // stacked weights reproduce the normalised mix under top-down decal blending
    {
        const float a[4] = { 0.25f, 0.25f, 0.5f, 0.0f };
        float s[4];
        TerrainData::StackedWeights(a, s, 4);
        float contribution[4], cover = 1.0f;
        for (int l = 3; l >= 0; --l) {
            contribution[l] = cover * s[l];
            cover *= (1.0f - s[l]);
        }
        bool ok = true;
        for (int l = 0; l < 4; ++l) ok &= Near(contribution[l], a[l]);
        CHECK(ok, "stacked weights composite back to the normalised mix");
    }

    // undo snapshots
    {
        TerrainData d;
        Brush b; b.radius = 6.0f;
        TileSnapshot before;
        d.Capture(d.RectAround(0.0f, 0.0f, b.radius + 1.0f), before);
        d.Raise(0.0f, 0.0f, b, 3.0f);
        TileSnapshot after = d.CaptureSame(before);
        CHECK(d.TileCount() > 0, "raise created tiles");
        d.Restore(before);
        CHECK(d.TileCount() == 0 && Near(d.HeightAt(0, 0), 0.0f), "undo removes tiles that did not exist");
        d.Restore(after);
        CHECK(Near(d.HeightAt(0, 0), 3.0f), "redo puts the edit back");
    }

    // layers
    {
        TerrainData d;
        Brush b; b.radius = 3.0f; b.hardness = 1.0f;
        d.Paint(0.0f, 0.0f, b, 3, 1.0f);
        Layer extra; extra.name = "Mud";
        CHECK(d.AddLayer(extra) && d.LayerCount() == 5, "add layer");
        CHECK(Near(WeightSum(d, 0, 0), 1.0f, 0.005f), "add layer keeps sums");
        d.Paint(0.0f, 0.0f, b, 4, 1.0f);
        CHECK(d.RemoveLayer(4) && d.LayerCount() == 4, "remove layer");
        float w[kMaxLayers];
        d.WeightsAt(0, 0, w);
        CHECK(w[0] > 0.99f, "removed layer's weight goes to layer 0");
        CHECK(!d.RemoveLayer(0), "never below the minimum layer count");
    }

    // save blob
    {
        TerrainData d;
        d.spacing = 0.5f;
        d.baseHeight = -2.0f;
        d.layers[1].name = "Clay";
        d.layers[1].soil.kc = 13.2f;
        Brush b; b.radius = 8.0f;
        d.Raise(10.0f, -70.0f, b, 5.0f);
        d.Paint(10.0f, -70.0f, b, 2, 0.7f);

        std::vector<uint8_t> blob;
        d.Encode(blob);
        CHECK(TerrainData::LooksLikeTerrainData(blob.data(), blob.size()), "blob has the magic");
        std::string error;
        auto r = TerrainData::Decode(blob.data(), blob.size(), error);
        CHECK(r != nullptr, "decode succeeds");
        if (r) {
            CHECK(Near(r->spacing, 0.5f) && Near(r->baseHeight, -2.0f), "header round trip");
            CHECK(r->layers[1].name == "Clay" && Near(r->layers[1].soil.kc, 13.2f), "layer round trip");
            CHECK(r->TileCount() == d.TileCount(), "tile count round trip");
            bool same = true;
            for (int j = -160; j <= -120; ++j)
                for (int i = 0; i <= 40; ++i) {
                    same &= d.HeightAt(i, j) == r->HeightAt(i, j);
                    float a[kMaxLayers], c[kMaxLayers];
                    d.WeightsAt(i, j, a);
                    r->WeightsAt(i, j, c);
                    for (uint32_t l = 0; l < 4; ++l) same &= a[l] == c[l];
                }
            CHECK(same, "samples round trip exactly");
        }
        blob.resize(blob.size() / 2);
        CHECK(TerrainData::Decode(blob.data(), blob.size(), error) == nullptr, "truncated blob is rejected");
    }

    // raycast
    {
        TerrainData d;
        Brush b; b.radius = 20.0f; b.hardness = 1.0f;
        d.Raise(0.0f, 0.0f, b, 10.0f);
        XMFLOAT3 hit;
        const float k = 1.0f / std::sqrt(2.0f);
        const bool ok = d.Raycast(XMFLOAT3(0, 60, -50), XMFLOAT3(0, -k, k), 500.0f, hit);
        CHECK(ok, "ray hits the raised plateau");
        if (ok) CHECK(Near(hit.y, d.SampleHeight(hit.x, hit.z), 0.01f) && Near(hit.y, 10.0f, 0.2f), "hit sits on the surface");
        CHECK(!d.Raycast(XMFLOAT3(0, 60, 0), XMFLOAT3(0, 1, 0), 500.0f, hit), "ray pointing up misses");
    }

    // generator
    {
        TerrainData d;
        GenerateParams p;
        p.x = -64; p.z = -64; p.sizeX = 128; p.sizeZ = 128;
        p.height = 100.0f;
        for (bool& on : p.biomes) on = true;
        const SampleRect r = Generate(d, p);
        CHECK(!r.Empty(), "generate wrote a region");
        float lo, hi;
        d.HeightRange(lo, hi);
        CHECK(std::isfinite(lo) && std::isfinite(hi), "generated heights are finite");
        CHECK(lo > -20.0f && hi < 160.0f, "generated heights stay in range");
        CHECK(Near(WeightSum(d, 0, 0), 1.0f, 0.005f), "auto paint sums to 1");
        CHECK(Near(d.HeightAt(-64, 0), 0.0f, 0.01f), "edge fades back to what was there");
    }

    // heightmap import (.r16, 4x4 ramp)
    {
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "terrain_data_test.r16";
        {
            std::ofstream out(file, std::ios::binary);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const uint16_t v = uint16_t(x * 65535 / 3);
                    out.write((const char*)&v, sizeof(v));
                }
        }
        TerrainData d;
        HeightmapImport p;
        p.path = file.string();
        p.x = 0; p.z = 0; p.sizeX = 30; p.sizeZ = 30;
        p.minHeight = 10.0f; p.maxHeight = 40.0f;
        p.autoPaint = false;
        SampleRect written;
        std::string error;
        const bool ok = ImportHeightmap(d, p, written, error);
        CHECK(ok, error.c_str());
        if (ok) {
            CHECK(Near(d.HeightAt(0, 15), 10.0f, 0.01f), "import: black edge");
            CHECK(Near(d.HeightAt(30, 15), 40.0f, 0.01f), "import: white edge");
            CHECK(Near(d.HeightAt(15, 15), 25.0f, 0.05f), "import: interpolated middle");
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }

    wi::jobsystem::ShutDown();
    if (failures == 0) std::printf("terrain_data_test: all checks passed\n");
    return failures;
}
