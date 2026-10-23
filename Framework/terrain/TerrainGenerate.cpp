#include "terrain/TerrainGenerate.h"

#include "wiHelper.h"
#include "wiJobSystem.h"
#include "wiNoise.h"
#include "Utility/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace st::terrain {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float Saturate (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep (float lo, float hi, float x) {
    if (hi <= lo) return x >= hi ? 1.0f : 0.0f;
    const float t = Saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

float Fract (float v) { return v - std::floor(v); }

// One sample's biome shape, in units of GenerateParams::height. Each is shaped to read as
// its namesake from a vehicle's point of view, not to be geologically right.
float BiomeHeight (Biome b, const wi::noise::Perlin& n, float x, float z, float duneAngle) {
    auto fbm = [&](float freq, int octaves, float offset) {
        return n.compute(x * freq + offset, 0.37f, z * freq - offset, octaves);
    };
    switch (b) {
        case Biome::Plains:
            return 0.03f * fbm(1.0f / 300.0f, 4, 11.0f) + 0.01f * fbm(1.0f / 40.0f, 3, 13.0f);
        case Biome::Hills: {
            // Billowed noise rounds the tops the way weathered hills are.
            const float billow = std::fabs(fbm(1.0f / 260.0f, 6, 23.0f));
            return 0.22f * billow + 0.03f * fbm(1.0f / 70.0f, 4, 29.0f);
        }
        case Biome::Mountains: {
            // Ridged multifractal (Musgrave): each octave is weighted by the one before it,
            // so detail piles up on the ridges and the valleys stay smooth.
            float sum = 0.0f, freq = 1.0f / 650.0f, amp = 1.0f, weight = 1.0f;
            for (int o = 0; o < 7; ++o) {
                float r = 1.0f - std::fabs(n.compute(x * freq + 37.0f * o, 0.37f, z * freq - 19.0f * o));
                r *= r;
                r *= weight;
                weight = Saturate(r * 2.0f);
                sum += r * amp;
                freq *= 2.0f;
                amp *= 0.5f;
            }
            return std::pow(Saturate(sum * 0.55f), 1.6f);
        }
        case Biome::Dunes: {
            // Long crests across the wind, bent by noise so they do not look ploughed.
            const float u = (x * std::cos(duneAngle) + z * std::sin(duneAngle)) / 38.0f
                          + 2.5f * fbm(1.0f / 160.0f, 3, 53.0f);
            const float crest = std::pow(0.5f + 0.5f * std::sin(u), 3.0f);
            return 0.06f * crest + 0.02f * fbm(1.0f / 400.0f, 2, 59.0f);
        }
        case Biome::Canyons: {
            // A mesa top cut by winding channels, then terraced so the walls step down.
            const float channel = std::fabs(fbm(1.0f / 360.0f, 4, 67.0f));
            float h = 0.28f * SmoothStep(0.02f, 0.13f, channel);
            const float t = h * 9.0f;
            h = (std::floor(t) + SmoothStep(0.65f, 1.0f, Fract(t))) / 9.0f;
            return h;
        }
        case Biome::Marsh:
            return -0.004f + 0.006f * fbm(1.0f / 70.0f, 3, 71.0f);
        case Biome::Lakes:
            return -0.07f * SmoothStep(0.05f, 0.45f, fbm(1.0f / 260.0f, 3, 79.0f) * 0.5f + 0.5f);
        default:
            return 0.0f;
    }
}

// Push a sample's paint towards one layer by `amount`, taking from the others evenly.
void Tint (TerrainData& data, int i, int j, uint32_t layer, float amount) {
    if (amount <= 0.0f || layer >= data.LayerCount()) return;
    float w[kMaxLayers];
    data.WeightsAt(i, j, w);
    const float target = Saturate(w[layer] + amount * (1.0f - w[layer]));
    const float rest = 1.0f - w[layer];
    const float scale = rest > 1e-5f ? (1.0f - target) / rest : 0.0f;
    for (uint32_t l = 0; l < data.LayerCount(); ++l)
        w[l] = (l == layer) ? target : w[l] * scale;
    data.SetWeights(i, j, w);
}

// Make sure every tile under `r` exists, so the parallel loops below only ever FIND tiles.
// The tile map is not safe to grow from several threads.
void Preallocate (TerrainData& data, const SampleRect& r) {
    for (int j = r.j0; j <= r.j1 + kTileSize; j += kTileSize)
        for (int i = r.i0; i <= r.i1 + kTileSize; i += kTileSize)
            data.HeightPtr(std::min(i, r.i1), std::min(j, r.j1));
}

// A region's heights as one dense row-major grid, in whatever unit the caller chose.
struct Grid {
    int w = 0, h_ = 0;
    std::vector<float> h;
    float& At (int x, int y) { return h[size_t(y) * w + x]; }
};

// Bilinear height and gradient at a fractional grid position (x, y in [0, w-1) x [0, h-1)).
void HeightAndGradient (const Grid& g, float x, float y, float& height, float& gx, float& gy) {
    const int ix = int(x), iy = int(y);
    const float u = x - float(ix), v = y - float(iy);
    const size_t k = size_t(iy) * g.w + ix;
    const float nw = g.h[k], ne = g.h[k + 1], sw = g.h[k + g.w], se = g.h[k + g.w + 1];
    gx = (ne - nw) * (1 - v) + (se - sw) * v;
    gy = (sw - nw) * (1 - u) + (se - ne) * u;
    height = nw * (1 - u) * (1 - v) + ne * u * (1 - v) + sw * (1 - u) * v + se * u * v;
}

// Droplet hydraulic erosion (after Hans Beyer's "Implementation of a method for hydraulic
// erosion", the form popularised by Sebastian Lague). Each droplet rolls downhill with some
// inertia, picks up sediment while it speeds up and has capacity, and drops it where it slows
// - which carves gullies on the slopes and fills the valley floors. `deposit` collects where
// sediment came to rest, for the paint pass.
void HydraulicErosion (Grid& g, std::vector<float>& deposit, size_t droplets, uint32_t seed) {
    if (g.w < 8 || g.h_ < 8 || droplets == 0) return;

    constexpr float inertia      = 0.05f;
    constexpr float capacityK    = 4.0f;
    constexpr float minCapacity  = 0.01f;
    constexpr float depositSpeed = 0.3f;
    constexpr float erodeSpeed   = 0.3f;
    constexpr float evaporate    = 0.015f;
    constexpr float gravity      = 4.0f;
    constexpr int   lifetime     = 40;
    constexpr int   radius       = 3;

    // The erosion brush: every cell within `radius`, weighted by closeness, summing to 1.
    std::vector<int> brushOffX, brushOffY;
    std::vector<float> brushW;
    float wsum = 0.0f;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const float d = std::sqrt(float(dx * dx + dy * dy));
            if (d > radius) continue;
            brushOffX.push_back(dx);
            brushOffY.push_back(dy);
            brushW.push_back(1.0f - d / radius);
            wsum += brushW.back();
        }
    }
    for (float& w : brushW) w /= wsum;

    uint32_t state = seed * 747796405u + 2891336453u;
    auto rnd = [&]() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return float(state & 0xFFFFFF) / float(0x1000000);
    };

    // A droplet erodes with a brush around the cell it STARTS the step in, so it has to start a
    //	brush radius inside the edge; the bounds test below only guards where it moves to.
    const float spanX = float(g.w - 1 - 2 * radius), spanY = float(g.h_ - 1 - 2 * radius);
    if (spanX <= 1.0f || spanY <= 1.0f) return;
    for (size_t n = 0; n < droplets; ++n) {
        float x = float(radius) + rnd() * spanX, y = float(radius) + rnd() * spanY;
        float dx = 0, dy = 0, speed = 1, water = 1, sediment = 0;
        for (int life = 0; life < lifetime; ++life) {
            const int cx = int(x), cy = int(y);
            const float u = x - float(cx), v = y - float(cy);
            float height, gx, gy;
            HeightAndGradient(g, x, y, height, gx, gy);

            dx = dx * inertia - gx * (1 - inertia);
            dy = dy * inertia - gy * (1 - inertia);
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-6f) break;
            dx /= len; dy /= len;
            x += dx; y += dy;
            if (x < radius || y < radius || x >= float(g.w - 1 - radius) || y >= float(g.h_ - 1 - radius)) break;

            float newHeight, ngx, ngy;
            HeightAndGradient(g, x, y, newHeight, ngx, ngy);
            const float dh = newHeight - height;
            const float capacity = std::max(-dh * speed * water * capacityK, minCapacity);

            if (sediment > capacity || dh > 0) {
                // Going uphill, fill the pit behind; otherwise drop the excess.
                const float amount = dh > 0 ? std::min(dh, sediment) : (sediment - capacity) * depositSpeed;
                sediment -= amount;
                const size_t k = size_t(cy) * g.w + cx;
                g.h[k]           += amount * (1 - u) * (1 - v);
                g.h[k + 1]       += amount * u * (1 - v);
                g.h[k + g.w]     += amount * (1 - u) * v;
                g.h[k + g.w + 1] += amount * u * v;
                deposit[k] += amount;
            } else {
                const float amount = std::min((capacity - sediment) * erodeSpeed, -dh);
                for (size_t b = 0; b < brushW.size(); ++b) {
                    const int bx = cx + brushOffX[b], by = cy + brushOffY[b];
                    const float take = amount * brushW[b];
                    g.At(bx, by) -= take;
                    sediment += take;
                }
            }
            speed = std::sqrt(std::max(0.0f, speed * speed + dh * gravity));
            water *= (1 - evaporate);
        }
    }
}

// Thermal erosion: wherever a cell stands higher above a neighbour than `talus` allows,
// half the excess slides down. Knocks the spikes and razor ridges off raw noise.
void ThermalErosion (Grid& g, float talus, int passes) {
    const int dxs[4] = { 1, -1, 0, 0 }, dys[4] = { 0, 0, 1, -1 };
    for (int pass = 0; pass < passes; ++pass) {
        for (int y = 1; y < g.h_ - 1; ++y) {
            for (int x = 1; x < g.w - 1; ++x) {
                float& c = g.At(x, y);
                for (int k = 0; k < 4; ++k) {
                    float& nb = g.At(x + dxs[k], y + dys[k]);
                    const float diff = c - nb;
                    if (diff > talus) {
                        const float move = (diff - talus) * 0.25f;
                        c -= move;
                        nb += move;
                    }
                }
            }
        }
    }
}

} // namespace

const char* ToString (Biome biome) {
    switch (biome) {
        case Biome::Plains:    return "Plains";
        case Biome::Hills:     return "Hills";
        case Biome::Mountains: return "Mountains";
        case Biome::Dunes:     return "Dunes";
        case Biome::Canyons:   return "Canyons";
        case Biome::Marsh:     return "Marsh";
        case Biome::Lakes:     return "Lakes";
        default:               return "?";
    }
}

SampleRect Generate (TerrainData& data, const GenerateParams& p) {
    const SampleRect r = data.RectFromWorld(p.x, p.z, p.x + p.sizeX, p.z + p.sizeZ);
    if (r.Empty()) return r;

    std::vector<Biome> enabled;
    for (uint32_t b = 0; b < uint32_t(Biome::Count); ++b)
        if (p.biomes[b]) enabled.push_back(Biome(b));
    if (enabled.empty()) enabled.push_back(Biome::Plains);

    wi::noise::Perlin noise;
    noise.init(p.seed);
    wi::noise::Perlin selector;
    selector.init(p.seed * 2654435761u + 97u);

    const float duneAngle = float(p.seed % 360u) * (kPi / 180.0f);
    // Softmax sharpness: blending 0 gives near-hard borders, 1 a broad mix.
    const float sharpness = 1.5f + (1.0f - Saturate(p.blending)) * 14.0f;
    const float invBiome = 1.0f / std::max(p.biomeSize, 1.0f);
    const float fadeDist = std::max(p.edgeFade, 1e-4f) * std::min(p.sizeX, p.sizeZ);

    const int width = r.i1 - r.i0 + 1;
    const int rows  = r.j1 - r.j0 + 1;
    const size_t count = size_t(width) * rows;
    // The region is built in a local grid first, so erosion can run over it as a whole,
    // and only then written into the tiles.
    Grid grid{ width, rows, std::vector<float>(count) };
    std::vector<float> fadeShare(count, 0.0f);
    // Per-sample dune and marsh share, kept for the paint pass after the heights are final.
    std::vector<float> duneShare(count, 0.0f);
    std::vector<float> marshShare(count, 0.0f);

    Preallocate(data, r);

    const float warpAmp = Saturate(p.warp) * 0.3f * std::max(p.biomeSize, 1.0f);

    // One row per job. The job system stores its functions in a fixed 96-byte buffer, so the
    // job itself captures only the row function, which captures everything else.
    auto row = [&](int j) {
        float sel[uint32_t(Biome::Count)];
        for (int i = r.i0; i <= r.i1; ++i) {
            const float x = data.ToWorld(i), z = data.ToWorld(j);

            // Domain warp: bend the coordinates with low-frequency noise before sampling, so
            // ridges and biome borders wander instead of running in noise-grid straight lines.
            const float wx = x + warpAmp * noise.compute(x / 380.0f + 5.2f, 2.1f, z / 380.0f + 1.3f, 3);
            const float wz = z + warpAmp * noise.compute(x / 380.0f - 7.7f, 4.3f, z / 380.0f + 9.1f, 3);

            float total = 0.0f;
            for (size_t k = 0; k < enabled.size(); ++k) {
                const float off = 17.0f * float(uint32_t(enabled[k]) + 1);
                const float s = selector.compute(wx * invBiome + off, 1.7f, wz * invBiome - off, 2);
                sel[k] = std::exp(sharpness * s);
                total += sel[k];
            }

            float h = 0.0f, dunes = 0.0f, marsh = 0.0f;
            for (size_t k = 0; k < enabled.size(); ++k) {
                const float w = sel[k] / total;
                if (w < 1e-3f) continue;
                h += w * BiomeHeight(enabled[k], noise, wx, wz, duneAngle);
                if (enabled[k] == Biome::Dunes) dunes = w;
                if (enabled[k] == Biome::Marsh) marsh = w;
            }

            // Fade into whatever was there so the region has no cliff at its border.
            const float edge = std::min(std::min(x - p.x, p.x + p.sizeX - x),
                                        std::min(z - p.z, p.z + p.sizeZ - z));
            const float fade = SmoothStep(0.0f, 1.0f, edge / fadeDist);

            const float existing = data.HeightAt(i, j);
            const float generated = p.base + h * p.height;
            const size_t s = size_t(j - r.j0) * width + size_t(i - r.i0);
            grid.h[s] = existing + (generated - existing) * fade;
            fadeShare[s] = fade;
            duneShare[s] = dunes * fade;
            marshShare[s] = marsh * fade;
        }
    };
    wi::jobsystem::context ctx;
    const int firstRow = r.j0;
    wi::jobsystem::Dispatch(ctx, uint32_t(rows), 1, [&row, firstRow](wi::jobsystem::JobArgs args) {
        row(firstRow + int(args.jobIndex));
    });
    wi::jobsystem::Wait(ctx);

    // Weather. Erosion runs in "sample" units (heights divided by the spacing) so its
    // parameters mean the same thing at any resolution.
    std::vector<float> deposit(count, 0.0f);
    if (p.erosion > 0.0f || p.thermal) {
        const std::vector<float> before = grid.h;
        const float s = data.spacing;
        for (float& v : grid.h) v /= s;
        if (p.erosion > 0.0f)
            HydraulicErosion(grid, deposit, size_t(double(count) * Saturate(p.erosion)), p.seed);
        if (p.thermal)
            ThermalErosion(grid, std::tan(38.0f * kPi / 180.0f), 6);
        for (size_t k = 0; k < count; ++k) {
            // Erosion must not reach past the fade either, or it would cut the border it
            // was meant to blend.
            const float eroded = grid.h[k] * s;
            grid.h[k] = before[k] + (eroded - before[k]) * fadeShare[k];
            deposit[k] *= fadeShare[k];
        }
    }

    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i)
            if (float* dst = data.FindHeightPtr(i, j))
                *dst = grid.h[size_t(j - r.j0) * width + size_t(i - r.i0)];

    if (p.autoPaint) {
        data.AutoPaint(r, p.waterLevel + 0.5f, 34.0f);
        // Sediment settles in the channels and at the foot of slopes: paint it as dirt, the
        // detail that makes eroded ground read as eroded rather than as smoothed noise.
        float maxDeposit = 1e-6f;
        for (float d : deposit) maxDeposit = std::max(maxDeposit, d);
        const float invDeposit = 1.0f / (maxDeposit * 0.25f);
        for (int j = r.j0; j <= r.j1; ++j) {
            for (int i = r.i0; i <= r.i1; ++i) {
                const size_t s = size_t(j - r.j0) * width + size_t(i - r.i0);
                const float altitude = (data.HeightAt(i, j) - p.base) / std::max(p.height, 1.0f);
                Tint(data, i, j, 1, Saturate(deposit[s] * invDeposit) * 0.85f); // sediment
                Tint(data, i, j, 2, SmoothStep(0.55f, 0.85f, altitude) * fadeShare[s] * 0.8f); // bare peaks
                Tint(data, i, j, 3, duneShare[s]);         // dunes are sand
                Tint(data, i, j, 1, marshShare[s] * 0.8f); // marsh is mud
            }
        }
    }
    return r;
}

bool ImportHeightmap (TerrainData& data, const HeightmapImport& p, SampleRect& written,
                      std::string& error) {
    written = SampleRect();
    wi::vector<uint8_t> file;
    if (!wi::helper::FileRead(p.path, file) || file.empty()) {
        error = "cannot read " + p.path;
        return false;
    }

    const std::string ext = wi::helper::toLower(wi::helper::GetExtensionFromFileName(p.path));
    int w = 0, h = 0;
    std::vector<float> pixels; // normalised 0..1, or metres for .r32
    bool metres = false;

    if (ext == "r16" || ext == "raw") {
        const size_t n = file.size() / 2;
        const int side = int(std::sqrt(double(n)) + 0.5);
        if (size_t(side) * side != n) {
            error = "a ." + ext + " heightmap must be square 16-bit samples";
            return false;
        }
        w = h = side;
        pixels.resize(n);
        const uint16_t* src = (const uint16_t*)file.data();
        for (size_t k = 0; k < n; ++k) pixels[k] = float(src[k]) / 65535.0f;
    } else if (ext == "r32") {
        const size_t n = file.size() / 4;
        const int side = int(std::sqrt(double(n)) + 0.5);
        if (size_t(side) * side != n) {
            error = "a .r32 heightmap must be square 32-bit float samples";
            return false;
        }
        w = h = side;
        pixels.resize(n);
        std::memcpy(pixels.data(), file.data(), n * sizeof(float));
        metres = true;
    } else {
        int comp = 0;
        stbi_us* img = stbi_load_16_from_memory(file.data(), int(file.size()), &w, &h, &comp, 1);
        if (img == nullptr) {
            error = std::string("cannot decode image: ") + stbi_failure_reason();
            return false;
        }
        pixels.resize(size_t(w) * h);
        for (size_t k = 0; k < pixels.size(); ++k) pixels[k] = float(img[k]) / 65535.0f;
        stbi_image_free(img);
    }
    if (w < 2 || h < 2) {
        error = "heightmap is too small";
        return false;
    }

    auto pixel = [&](int px, int py) {
        px = std::max(0, std::min(w - 1, px));
        py = std::max(0, std::min(h - 1, py));
        return pixels[size_t(py) * w + px];
    };

    const SampleRect r = data.RectFromWorld(p.x, p.z, p.x + p.sizeX, p.z + p.sizeZ);
    Preallocate(data, r);

    auto row = [&](int j) {
        const float z = data.ToWorld(j);
        float v = (z - p.z) / std::max(p.sizeZ, 1e-3f);
        if (!p.flipY) v = 1.0f - v; // image top row is the north (+Z) edge
        v = Saturate(v) * float(h - 1);
        const int py = int(std::floor(v));
        const float ty = v - float(py);
        for (int i = r.i0; i <= r.i1; ++i) {
            const float x = data.ToWorld(i);
            const float u = Saturate((x - p.x) / std::max(p.sizeX, 1e-3f)) * float(w - 1);
            const int px = int(std::floor(u));
            const float tx = u - float(px);
            const float a = pixel(px, py)     + (pixel(px + 1, py)     - pixel(px, py))     * tx;
            const float b = pixel(px, py + 1) + (pixel(px + 1, py + 1) - pixel(px, py + 1)) * tx;
            const float value = a + (b - a) * ty;
            float* dst = data.FindHeightPtr(i, j);
            if (dst != nullptr) *dst = metres ? value : p.minHeight + value * (p.maxHeight - p.minHeight);
        }
    };
    wi::jobsystem::context ctx;
    const int firstRow = r.j0;
    wi::jobsystem::Dispatch(ctx, uint32_t(r.j1 - r.j0 + 1), 1, [&row, firstRow](wi::jobsystem::JobArgs args) {
        row(firstRow + int(args.jobIndex));
    });
    wi::jobsystem::Wait(ctx);

    if (p.autoPaint) data.AutoPaint(r, p.waterLevel + 0.5f, 34.0f);
    written = r;
    return true;
}

} // namespace st::terrain
