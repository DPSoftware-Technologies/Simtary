#include "terrain/TerrainData.h"

#include "wiHelper.h"
#include "wiNoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st::terrain {

namespace {

constexpr uint32_t kMagic   = 0x44545453; // "STTD"
constexpr uint32_t kVersion = 1;
constexpr int      kTileSamples = kTileSize * kTileSize;

float Saturate (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep (float lo, float hi, float x) {
    if (hi <= lo) return x >= hi ? 1.0f : 0.0f;
    const float t = Saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

int FloorToInt (float v) { return (int)std::floor(v); }
int CeilToInt (float v)  { return (int)std::ceil(v); }

// Soil numbers are Bekker-Wong parameters as tabulated in Wong, "Theory of Ground
// Vehicles" (kc in kN/m^(n+1), kphi in kN/m^(n+2), cohesion in kPa). Friction is the
// peak tyre coefficient a tyre model would start from on that surface.
struct NamedSoil { const char* name; SoilParams soil; };

SoilParams Soil (float friction, float rolling, float kc, float kphi, float n,
                 float cohesion, float angle, float absorption, bool deformable) {
    SoilParams s;
    s.friction = friction;
    s.rollingResistance = rolling;
    s.kc = kc;
    s.kphi = kphi;
    s.n = n;
    s.cohesion = cohesion;
    s.frictionAngle = angle;
    s.absorption = absorption;
    s.deformable = deformable;
    return s;
}

const NamedSoil kSoils[] = {
    { "Asphalt",    Soil(1.00f, 0.012f,  0.0f,    0.0f, 1.0f,  0.0f,  0.0f, 0.00f, false) },
    { "Concrete",   Soil(0.95f, 0.012f,  0.0f,    0.0f, 1.0f,  0.0f,  0.0f, 0.01f, false) },
    { "Rock",       Soil(0.90f, 0.015f,  0.0f,    0.0f, 1.0f,  0.0f,  0.0f, 0.00f, false) },
    { "Gravel",     Soil(0.70f, 0.020f,  0.0f,    0.0f, 1.0f,  0.0f, 35.0f, 0.30f, false) },
    { "Grass",      Soil(0.75f, 0.030f,  5.3f, 1515.0f, 0.7f,  1.7f, 29.0f, 0.40f, true)  },
    { "Sandy loam", Soil(0.70f, 0.040f,  5.3f, 1515.0f, 0.7f,  1.7f, 29.0f, 0.50f, true)  },
    { "Dry sand",   Soil(0.60f, 0.080f,  0.99f, 1528.0f, 1.1f, 1.04f, 28.0f, 0.90f, true)  },
    { "Clay / mud", Soil(0.45f, 0.100f, 13.2f,  692.0f, 0.5f,  4.14f, 13.0f, 0.10f, true)  },
    { "Snow",       Soil(0.30f, 0.050f,  4.37f, 196.7f, 1.6f,  1.03f, 19.7f, 0.20f, true)  },
};

const char* kSoilNames[] = {
    "Asphalt", "Concrete", "Rock", "Gravel", "Grass", "Sandy loam", "Dry sand", "Clay / mud", "Snow",
};

// byte stream

struct Writer {
    std::vector<uint8_t>& out;
    void Bytes (const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    }
    template <typename T> void Put (const T& v) { Bytes(&v, sizeof(T)); }
    void String (const std::string& s) {
        Put(uint32_t(s.size()));
        Bytes(s.data(), s.size());
    }
};

struct Reader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;
    bool ok = true;
    bool Bytes (void* dst, size_t n) {
        if (!ok || pos + n > size) { ok = false; return false; }
        std::memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool Get (T& v) { return Bytes(&v, sizeof(T)); }
    bool String (std::string& s) {
        uint32_t n = 0;
        if (!Get(n) || n > 4096 || pos + n > size) { ok = false; return false; }
        s.assign((const char*)p + pos, n);
        pos += n;
        return true;
    }
};

// Change the layer count of a tile's interleaved weights. New layers start at zero; dropped
// layers hand their weight to layer 0 so every sample still sums to 255.
void ConvertTileLayers (Tile& tile, uint32_t from, uint32_t to) {
    if (from == to || from == 0) return;
    std::vector<uint8_t> converted(size_t(kTileSamples) * to, 0);
    for (int s = 0; s < kTileSamples; ++s) {
        const uint8_t* src = &tile.weights[size_t(s) * from];
        uint8_t* dst = &converted[size_t(s) * to];
        uint32_t spill = 0;
        for (uint32_t l = 0; l < from; ++l) {
            if (l < to) dst[l] = src[l];
            else spill += src[l];
        }
        dst[0] = uint8_t(std::min(255u, dst[0] + spill));
    }
    tile.weights.swap(converted);
}

} // namespace

// ---------------------------------------------------------------- layer presets ---

std::vector<Layer> DefaultLayers () {
    std::vector<Layer> layers(4);
    layers[0].name = "Grass";
    layers[0].color = XMFLOAT4(0.29f, 0.42f, 0.17f, 1.0f);
    layers[0].grass = true;
    SoilPreset("Grass", layers[0].soil);

    layers[1].name = "Dirt";
    layers[1].color = XMFLOAT4(0.40f, 0.30f, 0.21f, 1.0f);
    SoilPreset("Sandy loam", layers[1].soil);

    layers[2].name = "Rock";
    layers[2].color = XMFLOAT4(0.44f, 0.43f, 0.41f, 1.0f);
    SoilPreset("Rock", layers[2].soil);

    layers[3].name = "Sand";
    layers[3].color = XMFLOAT4(0.76f, 0.68f, 0.50f, 1.0f);
    SoilPreset("Dry sand", layers[3].soil);
    return layers;
}

bool SoilPreset (const char* name, SoilParams& out) {
    for (const NamedSoil& s : kSoils) {
        if (std::strcmp(s.name, name) == 0) {
            out = s.soil;
            return true;
        }
    }
    return false;
}

const char* const* SoilPresetNames (size_t& count) {
    count = sizeof(kSoilNames) / sizeof(kSoilNames[0]);
    return kSoilNames;
}

void SampleRect::Merge (const SampleRect& o) {
    if (o.Empty()) return;
    if (Empty()) { *this = o; return; }
    i0 = std::min(i0, o.i0);
    j0 = std::min(j0, o.j0);
    i1 = std::max(i1, o.i1);
    j1 = std::max(j1, o.j1);
}

// ------------------------------------------------------------------ addressing ---

uint64_t TerrainData::Key (int ti, int tj) {
    return (uint64_t(uint32_t(ti)) << 32) | uint64_t(uint32_t(tj));
}

void TerrainData::SplitKey (uint64_t key, int& ti, int& tj) {
    ti = int(uint32_t(key >> 32));
    tj = int(uint32_t(key & 0xFFFFFFFFu));
}

int TerrainData::TileIndex (int sample) {
    return sample >= 0 ? sample / kTileSize : -((-sample - 1) / kTileSize) - 1;
}

const Tile* TerrainData::FindTile (int ti, int tj) const {
    auto it = tiles_.find(Key(ti, tj));
    return it == tiles_.end() ? nullptr : &it->second;
}

Tile& TerrainData::GetOrCreateTile (int ti, int tj) {
    Tile& tile = tiles_[Key(ti, tj)];
    if (tile.heights.empty()) {
        const uint32_t L = LayerCount();
        tile.heights.assign(kTileSamples, baseHeight);
        tile.weights.assign(size_t(kTileSamples) * L, 0);
        for (int s = 0; s < kTileSamples; ++s)
            tile.weights[size_t(s) * L] = 255;
    }
    return tile;
}

int TerrainData::ToSample (float world) const {
    return FloorToInt(world / spacing + 0.5f);
}

SampleRect TerrainData::RectAround (float x, float z, float radius) const {
    SampleRect r;
    r.i0 = FloorToInt((x - radius) / spacing);
    r.i1 = CeilToInt((x + radius) / spacing);
    r.j0 = FloorToInt((z - radius) / spacing);
    r.j1 = CeilToInt((z + radius) / spacing);
    return r;
}

SampleRect TerrainData::RectFromWorld (float x0, float z0, float x1, float z1) const {
    SampleRect r;
    r.i0 = FloorToInt(std::min(x0, x1) / spacing);
    r.i1 = CeilToInt(std::max(x0, x1) / spacing);
    r.j0 = FloorToInt(std::min(z0, z1) / spacing);
    r.j1 = CeilToInt(std::max(z0, z1) / spacing);
    return r;
}

void TerrainData::WorldBounds (const SampleRect& r, XMFLOAT2& min, XMFLOAT2& max) const {
    min = XMFLOAT2(ToWorld(r.i0), ToWorld(r.j0));
    max = XMFLOAT2(ToWorld(r.i1), ToWorld(r.j1));
}

// --------------------------------------------------------------------- reading ---

float TerrainData::HeightAt (int i, int j) const {
    const int ti = TileIndex(i), tj = TileIndex(j);
    const Tile* tile = FindTile(ti, tj);
    if (tile == nullptr) return baseHeight;
    return tile->heights[size_t(j - tj * kTileSize) * kTileSize + size_t(i - ti * kTileSize)];
}

void TerrainData::WeightsAt (int i, int j, float* out) const {
    const uint32_t L = LayerCount();
    const int ti = TileIndex(i), tj = TileIndex(j);
    const Tile* tile = FindTile(ti, tj);
    if (tile == nullptr) {
        out[0] = 1.0f;
        for (uint32_t l = 1; l < L; ++l) out[l] = 0.0f;
        return;
    }
    const size_t s = size_t(j - tj * kTileSize) * kTileSize + size_t(i - ti * kTileSize);
    const uint8_t* w = &tile->weights[s * L];
    for (uint32_t l = 0; l < L; ++l) out[l] = float(w[l]) * (1.0f / 255.0f);
}

float TerrainData::SampleHeight (float x, float z) const {
    const float fx = x / spacing, fz = z / spacing;
    const int i = FloorToInt(fx), j = FloorToInt(fz);
    const float tx = fx - float(i), tz = fz - float(j);
    const float h00 = HeightAt(i, j),     h10 = HeightAt(i + 1, j);
    const float h01 = HeightAt(i, j + 1), h11 = HeightAt(i + 1, j + 1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

void TerrainData::SampleWeights (float x, float z, float* out) const {
    const uint32_t L = LayerCount();
    const float fx = x / spacing, fz = z / spacing;
    const int i = FloorToInt(fx), j = FloorToInt(fz);
    const float tx = fx - float(i), tz = fz - float(j);
    float w00[kMaxLayers], w10[kMaxLayers], w01[kMaxLayers], w11[kMaxLayers];
    WeightsAt(i, j, w00);
    WeightsAt(i + 1, j, w10);
    WeightsAt(i, j + 1, w01);
    WeightsAt(i + 1, j + 1, w11);
    for (uint32_t l = 0; l < L; ++l) {
        const float a = w00[l] + (w10[l] - w00[l]) * tx;
        const float b = w01[l] + (w11[l] - w01[l]) * tx;
        out[l] = a + (b - a) * tz;
    }
}

XMFLOAT3 TerrainData::SampleNormal (float x, float z) const {
    const float s = spacing;
    const float dx = SampleHeight(x + s, z) - SampleHeight(x - s, z);
    const float dz = SampleHeight(x, z + s) - SampleHeight(x, z - s);
    XMFLOAT3 n(-dx, 2.0f * s, -dz);
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    n.x /= len; n.y /= len; n.z /= len;
    return n;
}

uint32_t TerrainData::DominantLayer (float x, float z) const {
    float w[kMaxLayers];
    SampleWeights(x, z, w);
    uint32_t best = 0;
    for (uint32_t l = 1; l < LayerCount(); ++l)
        if (w[l] > w[best]) best = l;
    return best;
}

void TerrainData::StackedWeights (const float* normalised, float* stacked, uint32_t count) {
    // The engine composites layers from the top index down, each covering what is below
    // it by its own weight. For layer i to end up contributing exactly normalised[i], its
    // stacked weight has to be its share of everything at or below it.
    float prefix = 0.0f;
    for (uint32_t l = 0; l < count; ++l) {
        const float a = std::max(0.0f, normalised[l]);
        prefix += a;
        stacked[l] = prefix > 1e-5f ? a / prefix : 0.0f;
    }
}

bool TerrainData::Raycast (const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                           XMFLOAT3& hit) const {
    auto above = [&](float t) {
        const float x = origin.x + direction.x * t;
        const float y = origin.y + direction.y * t;
        const float z = origin.z + direction.z * t;
        return y - SampleHeight(x, z);
    };

    float prevT = 0.0f;
    float prevD = above(0.0f);
    if (prevD < 0.0f) return false; // starting underground: nothing sensible to hit

    const float minStep = spacing * 0.25f;
    float t = 0.0f;
    while (t < maxDistance) {
        // March faster while high above the ground. Halving the clearance keeps a steep
        // slope from being stepped over when the ray runs nearly flat.
        const float step = std::min(50.0f, std::max(minStep, prevD * 0.5f));
        t = std::min(maxDistance, t + step);
        const float d = above(t);
        if (d <= 0.0f) {
            float lo = prevT, hi = t;
            for (int k = 0; k < 16; ++k) {
                const float mid = 0.5f * (lo + hi);
                if (above(mid) > 0.0f) lo = mid; else hi = mid;
            }
            hit = XMFLOAT3(origin.x + direction.x * hi, 0.0f, origin.z + direction.z * hi);
            hit.y = SampleHeight(hit.x, hit.z);
            return true;
        }
        prevT = t;
        prevD = d;
    }
    return false;
}

// --------------------------------------------------------------------- writing ---

float* TerrainData::HeightPtr (int i, int j) {
    const int ti = TileIndex(i), tj = TileIndex(j);
    Tile& tile = GetOrCreateTile(ti, tj);
    return &tile.heights[size_t(j - tj * kTileSize) * kTileSize + size_t(i - ti * kTileSize)];
}

float* TerrainData::FindHeightPtr (int i, int j) {
    const int ti = TileIndex(i), tj = TileIndex(j);
    auto it = tiles_.find(Key(ti, tj));
    if (it == tiles_.end()) return nullptr;
    return &it->second.heights[size_t(j - tj * kTileSize) * kTileSize + size_t(i - ti * kTileSize)];
}

uint8_t* TerrainData::WeightPtr (int i, int j) {
    const int ti = TileIndex(i), tj = TileIndex(j);
    Tile& tile = GetOrCreateTile(ti, tj);
    const size_t s = size_t(j - tj * kTileSize) * kTileSize + size_t(i - ti * kTileSize);
    return &tile.weights[s * LayerCount()];
}

void TerrainData::SetWeights (int i, int j, const float* weights) {
    const uint32_t L = LayerCount();
    float sum = 0.0f;
    for (uint32_t l = 0; l < L; ++l) sum += std::max(0.0f, weights[l]);

    uint8_t* out = WeightPtr(i, j);
    if (sum <= 1e-6f) {
        out[0] = 255;
        for (uint32_t l = 1; l < L; ++l) out[l] = 0;
        return;
    }

    // Quantise, then hand the rounding remainder to the strongest layer so the sample
    // still sums to exactly 255.
    uint32_t total = 0, strongest = 0;
    for (uint32_t l = 0; l < L; ++l) {
        const float w = std::max(0.0f, weights[l]) / sum;
        out[l] = uint8_t(w * 255.0f);
        total += out[l];
        if (weights[l] > weights[strongest]) strongest = l;
    }
    out[strongest] = uint8_t(out[strongest] + (255 - total));
}

bool TerrainData::AddLayer (const Layer& layer) {
    const uint32_t L = LayerCount();
    if (L >= kMaxLayers) return false;
    for (auto& kv : tiles_) ConvertTileLayers(kv.second, L, L + 1);
    layers.push_back(layer);
    return true;
}

bool TerrainData::RemoveLayer (uint32_t index) {
    const uint32_t L = LayerCount();
    if (L <= kMinLayers || index >= L) return false;
    // Whoever inherits the weight: layer 0, or layer 1 when layer 0 itself is going.
    const uint32_t heir = index == 0 ? 1 : 0;
    for (auto& kv : tiles_) {
        Tile& tile = kv.second;
        std::vector<uint8_t> converted(size_t(kTileSamples) * (L - 1));
        for (int s = 0; s < kTileSamples; ++s) {
            const uint8_t* src = &tile.weights[size_t(s) * L];
            uint8_t* dst = &converted[size_t(s) * (L - 1)];
            uint32_t d = 0;
            for (uint32_t l = 0; l < L; ++l) {
                if (l == index) continue;
                uint32_t v = src[l];
                if (l == heir) v += src[index];
                dst[d++] = uint8_t(std::min(255u, v));
            }
        }
        tile.weights.swap(converted);
    }
    layers.erase(layers.begin() + index);
    return true;
}

// ------------------------------------------------------------------- brushes ---

float TerrainData::BrushWeight (const Brush& brush, float dx, float dz) const {
    const float r = std::max(brush.radius, 1e-3f);
    const float d = brush.shape == BrushShape::Circle
        ? std::sqrt(dx * dx + dz * dz) / r
        : std::max(std::fabs(dx), std::fabs(dz)) / r;
    if (d >= 1.0f) return 0.0f;
    const float inner = Saturate(brush.hardness) * 0.999f;
    if (d <= inner) return 1.0f;
    return 1.0f - SmoothStep(inner, 1.0f, d);
}

SampleRect TerrainData::Raise (float x, float z, const Brush& brush, float metres) {
    SampleRect r = RectAround(x, z, brush.radius);
    bool any = false;
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float w = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (w <= 0.0f) continue;
            *HeightPtr(i, j) += metres * w;
            any = true;
        }
    }
    return any ? r : SampleRect();
}

SampleRect TerrainData::Smooth (float x, float z, const Brush& brush, float amount) {
    SampleRect r = RectAround(x, z, brush.radius);
    // Read the neighbourhood first: smoothing in place would let the samples already
    // written this dab bias the ones after them, and the brush would drift sideways.
    const int w = r.i1 - r.i0 + 3, h = r.j1 - r.j0 + 3;
    std::vector<float> src(size_t(w) * h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            src[size_t(j) * w + i] = HeightAt(r.i0 - 1 + i, r.j0 - 1 + j);

    bool any = false;
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float bw = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (bw <= 0.0f) continue;
            const int li = i - r.i0 + 1, lj = j - r.j0 + 1;
            float avg = 0.0f;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di)
                    avg += src[size_t(lj + dj) * w + (li + di)];
            avg *= 1.0f / 9.0f;
            float* p = HeightPtr(i, j);
            *p += (avg - *p) * Saturate(amount * bw);
            any = true;
        }
    }
    return any ? r : SampleRect();
}

SampleRect TerrainData::Flatten (float x, float z, const Brush& brush, float target, float amount) {
    SampleRect r = RectAround(x, z, brush.radius);
    bool any = false;
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float bw = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (bw <= 0.0f) continue;
            float* p = HeightPtr(i, j);
            *p += (target - *p) * Saturate(amount * bw);
            any = true;
        }
    }
    return any ? r : SampleRect();
}

SampleRect TerrainData::Paint (float x, float z, const Brush& brush, uint32_t layer, float amount) {
    const uint32_t L = LayerCount();
    if (layer >= L) return SampleRect();
    SampleRect r = RectAround(x, z, brush.radius);
    bool any = false;
    float w[kMaxLayers];
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float bw = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (bw <= 0.0f) continue;
            WeightsAt(i, j, w);
            const float target = Saturate(w[layer] + amount * bw);
            const float rest = 1.0f - w[layer];
            const float scale = rest > 1e-5f ? (1.0f - target) / rest : 0.0f;
            for (uint32_t l = 0; l < L; ++l)
                w[l] = (l == layer) ? target : w[l] * scale;
            SetWeights(i, j, w);
            any = true;
        }
    }
    return any ? r : SampleRect();
}

SampleRect TerrainData::Replace (float x, float z, const Brush& brush, uint32_t from, uint32_t to,
                                 float amount) {
    const uint32_t L = LayerCount();
    if (from >= L || to >= L || from == to) return SampleRect();
    SampleRect r = RectAround(x, z, brush.radius);
    bool any = false;
    float w[kMaxLayers];
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float bw = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (bw <= 0.0f) continue;
            WeightsAt(i, j, w);
            if (w[from] <= 0.0f) continue;
            const float moved = w[from] * Saturate(amount * bw);
            w[from] -= moved;
            w[to] += moved;
            SetWeights(i, j, w);
            any = true;
        }
    }
    return any ? r : SampleRect();
}

SampleRect TerrainData::Noise (float x, float z, const Brush& brush, float metres, float scale,
                               uint32_t seed) {
    wi::noise::Perlin perlin;
    perlin.init(seed);
    const float inv = 1.0f / std::max(scale, 0.01f);
    SampleRect r = RectAround(x, z, brush.radius);
    bool any = false;
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float bw = BrushWeight(brush, ToWorld(i) - x, ToWorld(j) - z);
            if (bw <= 0.0f) continue;
            const float n = perlin.compute(ToWorld(i) * inv, 0.0f, ToWorld(j) * inv, 4);
            *HeightPtr(i, j) += metres * bw * n;
            any = true;
        }
    }
    return any ? r : SampleRect();
}

// ------------------------------------------------------------------- regions ---

SampleRect TerrainData::FillRegion (const SampleRect& r, float height, int layer) {
    if (r.Empty()) return r;
    const uint32_t L = LayerCount();
    float w[kMaxLayers] = {};
    if (layer >= 0 && uint32_t(layer) < L) w[layer] = 1.0f;
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            *HeightPtr(i, j) = height;
            if (layer >= 0) SetWeights(i, j, w);
        }
    }
    return r;
}

SampleRect TerrainData::ClearRegion (const SampleRect& r) {
    if (r.Empty()) return r;
    // Tiles wholly inside the rectangle are dropped outright - that is memory back, not
    // just flat ground. Tiles it only clips are reset sample by sample.
    const int ti0 = TileIndex(r.i0), ti1 = TileIndex(r.i1);
    const int tj0 = TileIndex(r.j0), tj1 = TileIndex(r.j1);
    for (int tj = tj0; tj <= tj1; ++tj) {
        for (int ti = ti0; ti <= ti1; ++ti) {
            const int si0 = ti * kTileSize, sj0 = tj * kTileSize;
            const int si1 = si0 + kTileSize - 1, sj1 = sj0 + kTileSize - 1;
            if (si0 >= r.i0 && si1 <= r.i1 && sj0 >= r.j0 && sj1 <= r.j1) {
                tiles_.erase(Key(ti, tj));
                continue;
            }
            if (FindTile(ti, tj) == nullptr) continue;
            SampleRect part;
            part.i0 = std::max(si0, r.i0); part.i1 = std::min(si1, r.i1);
            part.j0 = std::max(sj0, r.j0); part.j1 = std::min(sj1, r.j1);
            FillRegion(part, baseHeight, 0);
        }
    }
    return r;
}

SampleRect TerrainData::AutoPaint (const SampleRect& r, float sandBelow, float rockSlopeDegrees) {
    if (r.Empty() || LayerCount() < 4) return SampleRect();
    const float s = spacing;
    float w[kMaxLayers];
    for (int j = r.j0; j <= r.j1; ++j) {
        for (int i = r.i0; i <= r.i1; ++i) {
            const float h  = HeightAt(i, j);
            const float dx = HeightAt(i + 1, j) - HeightAt(i - 1, j);
            const float dz = HeightAt(i, j + 1) - HeightAt(i, j - 1);
            const float slope = std::atan(std::sqrt(dx * dx + dz * dz) / (2.0f * s)) * (180.0f / 3.14159265f);

            const float rock = SmoothStep(rockSlopeDegrees - 6.0f, rockSlopeDegrees + 4.0f, slope);
            const float dirt = SmoothStep(rockSlopeDegrees - 20.0f, rockSlopeDegrees - 6.0f, slope) * (1.0f - rock) * 0.7f;
            const float sand = (1.0f - SmoothStep(sandBelow - 0.5f, sandBelow + 1.5f, h)) * (1.0f - rock);
            for (uint32_t l = 0; l < LayerCount(); ++l) w[l] = 0.0f;
            w[1] = dirt;
            w[2] = rock;
            w[3] = sand;
            w[0] = std::max(0.0f, 1.0f - rock - dirt - sand);
            SetWeights(i, j, w);
        }
    }
    return r;
}

// ---------------------------------------------------------------------- undo ---

void TerrainData::Capture (const SampleRect& r, TileSnapshot& snap) const {
    if (r.Empty()) return;
    snap.layerCount = LayerCount();
    const int ti0 = TileIndex(r.i0), ti1 = TileIndex(r.i1);
    const int tj0 = TileIndex(r.j0), tj1 = TileIndex(r.j1);
    for (int tj = tj0; tj <= tj1; ++tj) {
        for (int ti = ti0; ti <= ti1; ++ti) {
            const uint64_t key = Key(ti, tj);
            if (snap.tiles.count(key)) continue;
            auto it = tiles_.find(key);
            if (it == tiles_.end()) snap.tiles.emplace(key, std::nullopt);
            else snap.tiles.emplace(key, it->second);
        }
    }
}

TileSnapshot TerrainData::CaptureSame (const TileSnapshot& before) const {
    TileSnapshot snap;
    snap.layerCount = LayerCount();
    for (const auto& kv : before.tiles) {
        auto it = tiles_.find(kv.first);
        if (it == tiles_.end()) snap.tiles.emplace(kv.first, std::nullopt);
        else snap.tiles.emplace(kv.first, it->second);
    }
    return snap;
}

SampleRect TerrainData::Restore (const TileSnapshot& snap) {
    SampleRect touched;
    for (const auto& kv : snap.tiles) {
        int ti, tj;
        SplitKey(kv.first, ti, tj);
        if (!kv.second.has_value()) {
            tiles_.erase(kv.first);
        } else {
            Tile tile = *kv.second;
            ConvertTileLayers(tile, snap.layerCount, LayerCount());
            tiles_[kv.first] = std::move(tile);
        }
        SampleRect r;
        r.i0 = ti * kTileSize; r.i1 = r.i0 + kTileSize - 1;
        r.j0 = tj * kTileSize; r.j1 = r.j0 + kTileSize - 1;
        touched.Merge(r);
    }
    return touched;
}

// ---------------------------------------------------------------- persistence ---

void TerrainData::Encode (std::vector<uint8_t>& out) const {
    std::vector<uint8_t> payload;
    payload.reserve(64 + tiles_.size() * (size_t(kTileSamples) * (4 + LayerCount()) + 8));
    Writer w{ payload };
    w.Put(spacing);
    w.Put(baseHeight);
    w.Put(LayerCount());
    for (const Layer& l : layers) {
        w.String(l.name);
        w.Put(l.color);
        w.Put(uint8_t(l.grass ? 1 : 0));
        w.Put(l.soil.friction);
        w.Put(l.soil.rollingResistance);
        w.Put(l.soil.kc);
        w.Put(l.soil.kphi);
        w.Put(l.soil.n);
        w.Put(l.soil.cohesion);
        w.Put(l.soil.frictionAngle);
        w.Put(l.soil.absorption);
        w.Put(uint8_t(l.soil.deformable ? 1 : 0));
    }
    w.Put(uint32_t(tiles_.size()));
    for (const auto& kv : tiles_) {
        int ti, tj;
        SplitKey(kv.first, ti, tj);
        w.Put(int32_t(ti));
        w.Put(int32_t(tj));
        w.Bytes(kv.second.heights.data(), kv.second.heights.size() * sizeof(float));
        w.Bytes(kv.second.weights.data(), kv.second.weights.size());
    }

    wi::vector<uint8_t> compressed;
    const bool packed = wi::helper::Compress(payload.data(), payload.size(), compressed);

    out.clear();
    Writer h{ out };
    h.Put(kMagic);
    h.Put(kVersion);
    h.Put(uint64_t(payload.size()));
    h.Put(uint8_t(packed ? 1 : 0));
    if (packed) h.Bytes(compressed.data(), compressed.size());
    else        h.Bytes(payload.data(), payload.size());
}

bool TerrainData::LooksLikeTerrainData (const uint8_t* data, size_t size) {
    if (data == nullptr || size < 17) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == kMagic;
}

std::shared_ptr<TerrainData> TerrainData::Decode (const uint8_t* data, size_t size, std::string& error) {
    if (!LooksLikeTerrainData(data, size)) {
        error = "not terrain data";
        return nullptr;
    }
    Reader h{ data, size };
    uint32_t magic = 0, version = 0;
    uint64_t rawSize = 0;
    uint8_t packed = 0;
    h.Get(magic);
    h.Get(version);
    h.Get(rawSize);
    h.Get(packed);
    if (!h.ok || version != kVersion) {
        error = "unsupported terrain data version " + std::to_string(version);
        return nullptr;
    }

    wi::vector<uint8_t> raw;
    const uint8_t* body = data + h.pos;
    size_t bodySize = size - h.pos;
    if (packed) {
        if (!wi::helper::Decompress(body, bodySize, raw) || raw.size() != rawSize) {
            error = "terrain data failed to decompress";
            return nullptr;
        }
        body = raw.data();
        bodySize = raw.size();
    }

    Reader r{ body, bodySize };
    auto out = std::make_shared<TerrainData>();
    uint32_t layerCount = 0;
    r.Get(out->spacing);
    r.Get(out->baseHeight);
    r.Get(layerCount);
    if (!r.ok || layerCount < kMinLayers || layerCount > kMaxLayers || !(out->spacing > 0.0f)) {
        error = "terrain data header is corrupt";
        return nullptr;
    }
    out->layers.assign(layerCount, Layer());
    for (Layer& l : out->layers) {
        uint8_t grass = 0, deformable = 0;
        r.String(l.name);
        r.Get(l.color);
        r.Get(grass);
        r.Get(l.soil.friction);
        r.Get(l.soil.rollingResistance);
        r.Get(l.soil.kc);
        r.Get(l.soil.kphi);
        r.Get(l.soil.n);
        r.Get(l.soil.cohesion);
        r.Get(l.soil.frictionAngle);
        r.Get(l.soil.absorption);
        r.Get(deformable);
        l.grass = grass != 0;
        l.soil.deformable = deformable != 0;
    }

    uint32_t tileCount = 0;
    r.Get(tileCount);
    for (uint32_t t = 0; t < tileCount && r.ok; ++t) {
        int32_t ti = 0, tj = 0;
        r.Get(ti);
        r.Get(tj);
        Tile tile;
        tile.heights.resize(kTileSamples);
        tile.weights.resize(size_t(kTileSamples) * layerCount);
        r.Bytes(tile.heights.data(), tile.heights.size() * sizeof(float));
        r.Bytes(tile.weights.data(), tile.weights.size());
        if (r.ok) out->tiles_.emplace(Key(ti, tj), std::move(tile));
    }
    if (!r.ok) {
        error = "terrain data is truncated";
        return nullptr;
    }
    return out;
}

void TerrainData::HeightRange (float& lo, float& hi) const {
    lo = hi = baseHeight;
    for (const auto& kv : tiles_) {
        for (float h : kv.second.heights) {
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
}

} // namespace st::terrain
