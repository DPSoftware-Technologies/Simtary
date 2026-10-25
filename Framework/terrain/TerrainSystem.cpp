#include "terrain/TerrainSystem.h"

#include "io/asset/AssetPack.h"
#include "io/asset/AssetSystem.h"
#include "io/model/ModelImporter.h"
#include "render/MeshLOD.h"

#include "wiScene.h"
#include "wiTerrain.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiArchive.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace st::terrain {

namespace {

// What the engine's generator calls, from its job threads, for every vertex it builds.
class AuthoredSource : public wi::terrain::Source {
public:
    explicit AuthoredSource (std::shared_ptr<TerrainData> data) : data_(std::move(data)) {}

    float Height (const XMFLOAT2& p) const override {
        return data_->SampleHeight(p.x, p.y);
    }

    float Paint (const XMFLOAT2& p, uint32_t layer) const override {
        if (layer >= data_->LayerCount()) return 0.0f;
        float normalised[kMaxLayers];
        data_->SampleWeights(p.x, p.y, normalised);
        return normalised[layer];
    }

    float Surface (const XMFLOAT2& p, float* weights, uint32_t count) const override {
        float normalised[kMaxLayers];
        data_->SampleWeights(p.x, p.y, normalised);
        const uint32_t n = std::min(count, data_->LayerCount());
        TerrainData::StackedWeights(normalised, weights, n);
        for (uint32_t l = n; l < count; ++l) weights[l] = 0.0f;

        float grass = 0.0f;
        for (uint32_t l = 0; l < data_->LayerCount(); ++l)
            if (data_->layers[l].grass) grass += normalised[l];
        return grass;
    }

private:
    std::shared_ptr<TerrainData> data_;
};

size_t HashBytes (const wi::vector<uint8_t>& bytes) {
    return bytes.empty() ? 0 : wi::helper::HashByteData(bytes.data(), bytes.size());
}

wi::ecs::Entity CreateLayerMaterial (wi::scene::Scene& scene, const Layer& layer) {
    const wi::ecs::Entity m = wi::ecs::CreateEntity();
    scene.names.Create(m) = "terrain_" + layer.name;
    wi::scene::MaterialComponent& mat = scene.materials.Create(m);
    mat.SetBaseColor(layer.color);
    mat.SetRoughness(1.0f);
    mat.SetMetalness(0.0f);
    return m;
}

} // namespace

TerrainSystem& TerrainSystem::Get () {
    static TerrainSystem instance;
    return instance;
}

TerrainSystem::Binding* TerrainSystem::Find (wi::ecs::Entity terrain) {
    for (auto& b : bindings_)
        if (b->entity == terrain) return b.get();
    return nullptr;
}

const TerrainSystem::Binding* TerrainSystem::Find (wi::ecs::Entity terrain) const {
    for (const auto& b : bindings_)
        if (b->entity == terrain) return b.get();
    return nullptr;
}

bool TerrainSystem::IsAuthored (wi::ecs::Entity terrain) const {
    return Find(terrain) != nullptr;
}

TerrainData* TerrainSystem::Data (wi::ecs::Entity terrain) {
    Binding* b = Find(terrain);
    return b ? b->data.get() : nullptr;
}

wi::ecs::Entity TerrainSystem::FindTerrain (const wi::scene::Scene& scene) const {
    for (size_t i = 0; i < scene.terrains.GetCount(); ++i) {
        const wi::ecs::Entity e = scene.terrains.GetEntity(i);
        if (Find(e) != nullptr) return e;
    }
    return wi::ecs::INVALID_ENTITY;
}

TerrainSystem::Binding* TerrainSystem::Bind (wi::scene::Scene& scene, wi::ecs::Entity entity,
                                             wi::terrain::Terrain& terrain,
                                             std::shared_ptr<TerrainData> data) {
    (void)scene;
    Binding* b = Find(entity);
    if (b == nullptr) {
        bindings_.push_back(std::make_unique<Binding>());
        b = bindings_.back().get();
    }
    b->entity = entity;
    b->data = std::move(data);
    b->source = std::make_shared<AuthoredSource>(b->data);
    b->pending.clear();
    b->dirty = false;
    terrain.source = b->source;
    return b;
}

// --------------------------------------------------------------------- update ---

void TerrainSystem::SetFocus (bool enabled, const XMFLOAT3& position) {
    focus_ = enabled;
    focusPosition_ = position;
}

TerrainSystem::Binding* TerrainSystem::EnsureBound (wi::scene::Scene& scene, size_t index) {
    const wi::ecs::Entity entity = scene.terrains.GetEntity(index);
    wi::terrain::Terrain& terrain = scene.terrains[index];

    // Every terrain follows the editor camera while it has one, authored or not.
    terrain.focus_override = focus_;
    terrain.focus_position = focusPosition_;

    Binding* b = Find(entity);
    if (terrain.source == nullptr) {
        // Fresh from a load, or from an undo that rewrote the component - either way the
        // engine dropped the source, and authored_data says what the terrain should be.
        if (!TerrainData::LooksLikeTerrainData(terrain.authored_data.data(), terrain.authored_data.size()))
            return nullptr; // a plain procedural terrain: not ours

        const size_t hash = HashBytes(terrain.authored_data);
        if (b != nullptr && b->flushedHash == hash) {
            // The bytes are the ones we last wrote, so the data in memory is the same or
            // NEWER (edits since the last save). Undoing an unrelated property edit must
            // not throw a sculpt away, so keep what we have.
            terrain.source = b->source;
        } else {
            std::string error;
            std::shared_ptr<TerrainData> data = TerrainData::Decode(
                terrain.authored_data.data(), terrain.authored_data.size(), error);
            if (data == nullptr) {
                lastError_ = "Terrain: " + error;
                wi::backlog::post(lastError_, wi::backlog::LogLevel::Error);
                return nullptr;
            }
            b = Bind(scene, entity, terrain, std::move(data));
            b->flushedHash = hash;
            lastError_.clear();
            // Anything the generator built before this point came from its own noise, not
            // from the data - a raised plateau with its own heightfield, which is what
            // pushed a vehicle spawned on it under the world. Start over from the data.
            if (terrain.IsGenerationStarted() || !terrain.chunks.empty()) {
                if (terrain.IsGenerationBusy())
                    terrain.Generation_Cancel();
                terrain.SetGenerationStarted(false);
            }
        }
    } else if (b == nullptr || terrain.source != b->source) {
        return nullptr; // somebody else's source
    }
    return b;
}

void TerrainSystem::BindAll (wi::scene::Scene& scene) {
    for (size_t i = 0; i < scene.terrains.GetCount(); ++i)
        EnsureBound(scene, i);
}

void TerrainSystem::Update (wi::scene::Scene& scene, float sceneDt) {
    std::vector<wi::ecs::Entity> seen;

    for (size_t i = 0; i < scene.terrains.GetCount(); ++i) {
        const wi::ecs::Entity entity = scene.terrains.GetEntity(i);
        wi::terrain::Terrain& terrain = scene.terrains[i];
        Binding* b = EnsureBound(scene, i);
        if (b == nullptr)
            continue;
        seen.push_back(entity);

        // The generator reads the data from its job threads, so the data only changes while
        // it is idle. Anything queued during a busy frame simply runs on the next idle one.
        if (b->pending.empty() || terrain.IsGenerationBusy())
            continue;

        SampleRect touched;
        std::vector<EditFn> ops;
        ops.swap(b->pending);
        const bool rebuild = b->pendingRebuild;
        b->pendingRebuild = false;
        for (EditFn& op : ops)
            touched.Merge(op(*b->data));
        if (!touched.Empty()) {
            b->dirty = true;
            FitLevels(terrain, *b->data, touched);
            Invalidate(terrain, *b->data, touched, rebuild);
        }
    }

    // Scene::Update only runs terrain generation for dt > 0. Its reason (generation spawns
    // render work, which background loading must not) does not apply here - this is the main
    // thread, between frames - so a paused editor still sees its edits rebuilt. After the
    // edits above, so this frame's edit is the one that gets built.
    if (focus_ && sceneDt <= 0.0f) {
        for (size_t i = 0; i < scene.terrains.GetCount(); ++i) {
            wi::terrain::Terrain& terrain = scene.terrains[i];
            terrain.terrainEntity = scene.terrains.GetEntity(i);
            terrain.scene = &scene; // what Scene::Update would have set
            terrain.Generation_Update(scene.camera);
        }
    }

    // A binding whose terrain is gone (deleted, undone, or a different scene) goes too. The
    // chunks the engine built hang off their own group entity, which removing the terrain
    // COMPONENT (an undone "Create Terrain") leaves behind as orphan geometry - so take it
    // with us. Entity ids are never reused, so this cannot hit something in a newer scene.
    for (auto& b : bindings_) {
        if (const wi::terrain::Terrain* t = scene.terrains.GetComponent(b->entity))
            b->chunkGroup = t->chunkGroupEntity;
    }
    for (size_t k = 0; k < bindings_.size();) {
        Binding& b = *bindings_[k];
        if (std::find(seen.begin(), seen.end(), b.entity) != seen.end()) {
            ++k;
            continue;
        }
        if (!scene.terrains.Contains(b.entity) && b.chunkGroup != wi::ecs::INVALID_ENTITY)
            scene.Entity_Remove(b.chunkGroup);
        bindings_.erase(bindings_.begin() + k);
    }
}

void TerrainSystem::Invalidate (wi::terrain::Terrain& terrain, const TerrainData& data,
                                const SampleRect& r, bool rebuild) {
    XMFLOAT2 min, max;
    data.WorldBounds(r, min, max);
    // One sample more each way: the normals of the samples next to an edit change too.
    min.x -= data.spacing; min.y -= data.spacing;
    max.x += data.spacing; max.y += data.spacing;
    if (rebuild || !terrain.RefreshChunksInRegion(min, max))
        terrain.InvalidateChunksInRegion(min, max);
}

void TerrainSystem::FitLevels (wi::terrain::Terrain& terrain, const TerrainData& data,
                               const SampleRect& r) {
    // bottomLevel/topLevel only normalise the per-chunk heightmap that objects blend into
    // the ground with, but a height outside them would clamp there. Grow them to fit.
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (int j = r.j0; j <= r.j1; j += std::max(1, (r.j1 - r.j0) / 64)) {
        for (int i = r.i0; i <= r.i1; i += std::max(1, (r.i1 - r.i0) / 64)) {
            const float h = data.HeightAt(i, j);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    if (lo < terrain.bottomLevel) terrain.bottomLevel = std::floor(lo - 50.0f);
    if (hi > terrain.topLevel)    terrain.topLevel    = std::ceil(hi + 50.0f);
}

// --------------------------------------------------------------------- create ---

wi::ecs::Entity TerrainSystem::CreateTerrain (wi::scene::Scene& scene, const CreateParams& params) {
    auto data = std::make_shared<TerrainData>();
    data->spacing = std::max(0.1f, params.spacing);
    data->baseHeight = params.baseHeight;

    const wi::ecs::Entity entity = wi::ecs::CreateEntity();
    scene.names.Create(entity) = params.name;

    std::vector<wi::ecs::Entity> materials;
    for (const Layer& layer : data->layers)
        materials.push_back(CreateLayerMaterial(scene, layer));

    wi::terrain::Terrain& terrain = scene.terrains.Create(entity);
    terrain._flags = wi::terrain::Terrain::CENTER_TO_CAM
                   | wi::terrain::Terrain::REMOVAL
                   | wi::terrain::Terrain::PHYSICS;
    terrain.SetGrassEnabled(params.grass);
    terrain.chunk_scale = data->spacing;
    terrain.generation = std::max(1, params.generationRadius);
    terrain.physics_generation = 3;
    terrain.prop_generation = std::min(terrain.generation, 4);
    terrain.bottomLevel = params.baseHeight - 500.0f;
    terrain.topLevel = params.baseHeight + 2500.0f;
    terrain.materialEntities.assign(materials.begin(), materials.end());
    for (wi::ecs::Entity m : materials)
        scene.Component_Attach(m, entity);

    Binding* b = Bind(scene, entity, terrain, data);
    b->dirty = true;
    Flush(scene); // so the very first undo snapshot already carries the data
    return entity;
}

// ---------------------------------------------------------------------- edits ---

void TerrainSystem::Edit (wi::ecs::Entity terrain, EditFn op, bool rebuild) {
    if (Binding* b = Find(terrain)) {
        b->pending.push_back(std::move(op));
        b->pendingRebuild |= rebuild;
    }
}

size_t TerrainSystem::PendingEdits () const {
    size_t n = 0;
    for (const auto& b : bindings_) n += b->pending.size();
    return n;
}

void TerrainSystem::MarkDirty (wi::ecs::Entity terrain) {
    if (Binding* b = Find(terrain))
        b->dirty = true;
}

void TerrainSystem::AddLayer (wi::scene::Scene& scene, wi::ecs::Entity entity, const Layer& layer) {
    wi::scene::Scene* s = &scene;
    Edit(entity, [s, entity, layer](TerrainData& data) {
        wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity);
        if (terrain == nullptr || !data.AddLayer(layer)) return SampleRect();
        const wi::ecs::Entity m = CreateLayerMaterial(*s, layer);
        s->Component_Attach(m, entity);
        terrain->materialEntities.push_back(m);
        // A new material arrives dirty, which makes the engine regenerate every chunk with
        // the new layer count. Nothing to invalidate by hand.
        return SampleRect();
    });
    MarkDirty(entity);
}

void TerrainSystem::RemoveLayer (wi::scene::Scene& scene, wi::ecs::Entity entity, uint32_t index) {
    wi::scene::Scene* s = &scene;
    Edit(entity, [s, entity, index](TerrainData& data) {
        wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity);
        if (terrain == nullptr || !data.RemoveLayer(index)) return SampleRect();
        if (index < terrain->materialEntities.size()) {
            const wi::ecs::Entity m = terrain->materialEntities[index];
            terrain->materialEntities.erase(terrain->materialEntities.begin() + index);
            if (m != wi::ecs::INVALID_ENTITY) s->Entity_Remove(m);
        }
        // Removing a material does not dirty anything the engine watches, so restart the
        // generation by hand - every chunk's blend layers just shifted down by one.
        terrain->Generation_Restart();
        return SampleRect();
    });
    MarkDirty(entity);
}

wi::ecs::Entity TerrainSystem::LayerMaterial (const wi::scene::Scene& scene, wi::ecs::Entity entity,
                                              uint32_t layer) const {
    const wi::terrain::Terrain* terrain = scene.terrains.GetComponent(entity);
    if (terrain == nullptr || layer >= terrain->materialEntities.size())
        return wi::ecs::INVALID_ENTITY;
    return terrain->materialEntities[layer];
}

void TerrainSystem::SyncLayerMaterial (wi::scene::Scene& scene, wi::ecs::Entity entity, uint32_t layer) {
    Binding* b = Find(entity);
    if (b == nullptr || layer >= b->data->LayerCount()) return;
    const wi::ecs::Entity m = LayerMaterial(scene, entity, layer);
    if (wi::scene::MaterialComponent* mat = scene.materials.GetComponent(m))
        mat->SetBaseColor(b->data->layers[layer].color);
    if (wi::scene::NameComponent* name = scene.names.GetComponent(m))
        name->name = "terrain_" + b->data->layers[layer].name;
    b->dirty = true;
}

float TerrainSystem::TilingFor (float tileSize, float spacing) {
    // A chunk's texture coordinates run 0..1 across (chunk_width - 1) quads. A whole number of
    // repeats per chunk keeps neighbouring chunks seamless; at least two, because exactly one
    // is the value that tells the shader "no tiling set, use the engine's rule".
    const float chunk = float(wi::terrain::chunk_width - 1) * spacing;
    return std::max(2.0f, std::round(chunk / std::max(tileSize, 0.01f)));
}

LayerTextures TerrainSystem::GetLayerTextures (const wi::scene::Scene& scene, wi::ecs::Entity entity,
                                               uint32_t layer) const {
    LayerTextures out;
    const Binding* b = Find(entity);
    const wi::scene::MaterialComponent* mat = scene.materials.GetComponent(LayerMaterial(scene, entity, layer));
    if (b == nullptr || mat == nullptr) return out;
    using M = wi::scene::MaterialComponent;
    out.baseColor = mat->textures[M::BASECOLORMAP].name;
    out.normal = mat->textures[M::NORMALMAP].name;
    out.surface = mat->textures[M::SURFACEMAP].name;
    const float repeats = mat->texMulAdd.x;
    out.tileSize = repeats == 1.0f ? 0.0f
                 : float(wi::terrain::chunk_width - 1) * b->data->spacing / std::max(repeats, 1e-3f);
    return out;
}

void TerrainSystem::SetLayerTextures (wi::scene::Scene& scene, wi::ecs::Entity entity, uint32_t layer,
                                      const LayerTextures& textures) {
    Binding* b = Find(entity);
    wi::scene::MaterialComponent* mat = scene.materials.GetComponent(LayerMaterial(scene, entity, layer));
    if (b == nullptr || mat == nullptr || layer >= b->data->LayerCount()) return;
    using M = wi::scene::MaterialComponent;
    const std::pair<int, const std::string*> slots[] = {
        { M::BASECOLORMAP, &textures.baseColor },
        { M::NORMALMAP, &textures.normal },
        { M::SURFACEMAP, &textures.surface },
    };
    for (const auto& [slot, path] : slots) {
        mat->textures[slot].name = *path;
        mat->textures[slot].resource = {};
    }
    if (textures.tileSize > 0.0f) {
        const float repeats = TilingFor(textures.tileSize, b->data->spacing);
        mat->texMulAdd = XMFLOAT4(repeats, repeats, 0, 0);
    } else {
        mat->texMulAdd = XMFLOAT4(1, 1, 0, 0);
    }
    if (!textures.baseColor.empty()) {
        b->data->layers[layer].color = XMFLOAT4(1, 1, 1, 1);
        mat->SetBaseColor(b->data->layers[layer].color);
    }
    mat->CreateRenderData(true);
    mat->SetDirty();   // the engine re-renders every chunk's texture from a dirty layer material
    b->dirty = true;
}

GrassSettings TerrainSystem::GetGrass (const wi::scene::Scene& scene, wi::ecs::Entity entity) const {
    GrassSettings out;
    const wi::terrain::Terrain* terrain = scene.terrains.GetComponent(entity);
    if (terrain == nullptr) return out;
    // The scene's grass entity is what the engine reads back on every restart, so it wins.
    const wi::HairParticleSystem* hair = scene.hairs.GetComponent(terrain->grassEntity);
    const wi::scene::MaterialComponent* mat = scene.materials.GetComponent(terrain->grassEntity);
    const wi::HairParticleSystem& h = hair ? *hair : terrain->grass_properties;
    const wi::scene::MaterialComponent& m = mat ? *mat : terrain->grass_material;
    out.enabled = terrain->IsGrassEnabled();
    out.texture = m.textures[wi::scene::MaterialComponent::BASECOLORMAP].name;
    if (!h.atlas_rects.empty() && h.atlas_rects[0].texMulAdd.x > 0 && h.atlas_rects[0].texMulAdd.y > 0) {
        out.columns = std::max(1, int(std::round(1.0f / h.atlas_rects[0].texMulAdd.x)));
        out.rows = std::max(1, int(std::round(1.0f / h.atlas_rects[0].texMulAdd.y)));
    } else {
        out.columns = out.rows = 1;
    }
    out.height = h.length;
    out.randomness = h.randomness;
    out.density = terrain->grass_density;
    out.viewDistance = h.viewDistance;
    out.crossCards = int(h.billboardCount);
    out.stiffness = h.stiffness;
    return out;
}

void TerrainSystem::SetGrass (wi::scene::Scene& scene, wi::ecs::Entity entity, const GrassSettings& grass) {
    if (Find(entity) == nullptr) return;
    wi::scene::Scene* s = &scene;
    // The generator copies the grass parameters from its threads, so this waits for it like
    // any edit. Grass is decided when a chunk is built, so every chunk rebuilds.
    Edit(entity, [s, entity, grass](TerrainData&) {
        wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity);
        if (terrain == nullptr) return SampleRect();

        wi::HairParticleSystem& h = terrain->grass_properties;
        h.length = std::max(0.01f, grass.height);
        h.randomness = std::clamp(grass.randomness, 0.0f, 1.0f);
        h.viewDistance = std::max(1.0f, grass.viewDistance);
        h.billboardCount = uint32_t(std::clamp(grass.crossCards, 1, 4));
        h.stiffness = std::max(0.0f, grass.stiffness);
        h.segmentCount = 1;
        h.width = 1.0f;
        // The engine bends every card towards the camera's up vector so it never shows edge-on
        // from above. With wide clump cards that lays the whole field down in one direction -
        // parallel stripes like roof tiles when you look down. Upright cards look right.
        h.SetCameraBendEnabled(false);
        h.atlas_rects.clear();
        const int cols = std::max(1, grass.columns), rows = std::max(1, grass.rows);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                wi::HairParticleSystem::AtlasRect rect;
                rect.texMulAdd = XMFLOAT4(1.0f / cols, 1.0f / rows, float(c) / cols, float(r) / rows);
                h.atlas_rects.push_back(rect);
            }
        }
        h.SetDirty();

        using M = wi::scene::MaterialComponent;
        M& m = terrain->grass_material;
        m.textures[M::BASECOLORMAP].name = grass.texture;
        m.textures[M::BASECOLORMAP].resource = {};
        m.SetBaseColor(XMFLOAT4(1, 1, 1, 1));
        m.SetRoughness(1.0f);
        m.SetMetalness(0.0f);
        m.SetReflectance(0.02f);
        m.SetAlphaRef(0.5f);   // cut-out cards, not blended
        m.SetDoubleSided(true);
        m.CreateRenderData(true);
        m.SetDirty();

        terrain->grass_density = std::max(0.0f, grass.density);
        terrain->SetGrassEnabled(grass.enabled);
        // Generation_Restart reads the grass entity back into the fields above, so it must
        // carry them first.
        if (wi::HairParticleSystem* sh = s->hairs.GetComponent(terrain->grassEntity)) *sh = h;
        if (M* sm = s->materials.GetComponent(terrain->grassEntity)) *sm = m;
        terrain->Generation_Restart();
        return SampleRect();
    });
    MarkDirty(entity);
}

std::vector<TextureSet> TerrainSystem::FindTextureSets () {
    std::vector<TextureSet> sets;
    const AssetSystem& assets = AssetSystem::Get();
    auto sibling = [&](const std::string& diff, const char* suffix) {
        // "<dir>/<name>_diff.<ext>" -> "<dir>/<name><suffix>.<ext>", if the packages hold it
        const size_t at = diff.rfind("_diff.");
        std::string path = diff.substr(0, at) + suffix + diff.substr(at + 5);
        return assets.Exists(path) ? path : std::string();
    };
    for (uint32_t m = 0; m < assets.MountCount(); ++m) {
        const asset::AssetPack* pack = assets.PackAt(m);
        if (pack == nullptr) continue;
        for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
            const std::string path = pack->NameString(*pack->AssetAt(i));
            const size_t at = path.rfind("_diff.");
            if (at == std::string::npos) continue;
            const std::string ext = path.substr(at + 6);
            if (ext != "jpg" && ext != "png" && ext != "dds" && ext != "ktx2") continue;
            TextureSet set;
            const size_t slash = path.find_last_of('/');
            set.name = path.substr(slash == std::string::npos ? 0 : slash + 1, at - (slash == std::string::npos ? 0 : slash + 1));
            set.textures.baseColor = path;
            set.textures.normal = sibling(path, "_nor_gl");
            set.textures.surface = sibling(path, "_arm");
            if (std::none_of(sets.begin(), sets.end(), [&](const TextureSet& s) { return s.name == set.name; }))
                sets.push_back(std::move(set));
        }
    }
    std::sort(sets.begin(), sets.end(), [](const TextureSet& a, const TextureSet& b) { return a.name < b.name; });
    return sets;
}

// ---------------------------------------------------------------------- props ---

namespace {

PropSettings FromEngine (const wi::terrain::Prop& p) {
    PropSettings out;
    out.layer = uint32_t(std::max(0, p.region));
    out.minPerChunk = p.min_count_per_chunk;
    out.maxPerChunk = p.max_count_per_chunk;
    out.threshold = p.threshold;
    out.clumping = p.noise_frequency;
    out.minSize = p.min_size;
    out.maxSize = p.max_size;
    out.sink = -p.min_y_offset;
    // The name lives in the serialized entity: read it back the way the generator would.
    if (!p.data.empty()) {
        wi::scene::Scene tmp;
        wi::Archive archive(p.data.data(), p.data.size());
        wi::ecs::EntitySerializer seri;
        const wi::ecs::Entity e = tmp.Entity_Serialize(archive, seri, wi::ecs::INVALID_ENTITY,
            wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
        if (const wi::scene::NameComponent* n = tmp.names.GetComponent(e)) out.name = n->name;
    }
    return out;
}

void ToEngine (const PropSettings& s, wi::terrain::Prop& p) {
    p.region = int(std::min(s.layer, kMaxLayers - 1));
    p.region_power = 1.0f;
    p.min_count_per_chunk = std::max(0, s.minPerChunk);
    p.max_count_per_chunk = std::max(p.min_count_per_chunk, s.maxPerChunk);
    p.threshold = std::clamp(s.threshold, 0.0f, 1.0f);
    p.noise_frequency = std::max(0.0001f, s.clumping);
    p.noise_power = 1.0f;
    p.min_size = std::max(0.01f, std::min(s.minSize, s.maxSize));
    p.max_size = std::max(p.min_size, s.maxSize);
    p.min_y_offset = -std::max(0.0f, s.sink);
    p.max_y_offset = 0.0f;
}

} // namespace

std::vector<PropSettings> TerrainSystem::GetProps (const wi::scene::Scene& scene, wi::ecs::Entity entity) const {
    std::vector<PropSettings> out;
    if (const wi::terrain::Terrain* terrain = scene.terrains.GetComponent(entity))
        for (const wi::terrain::Prop& p : terrain->props) out.push_back(FromEngine(p));
    return out;
}

bool TerrainSystem::AddProp (wi::scene::Scene& scene, wi::ecs::Entity entity, const std::string& modelPath,
                             const PropSettings& settings, std::string* error) {
    using namespace wi::scene;
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
    if (Find(entity) == nullptr || !scene.terrains.Contains(entity)) return fail("not an authored terrain");

    st::model::ImportOptions options;
    options.importAnimations = options.importSkins = false;
    const st::model::ImportResult imported = st::model::Import(scene, modelPath, options);
    if (!imported.ok()) return fail(imported.error.empty() ? "import failed" : imported.error);

    // The biggest object under the import is the prop.
    wi::ecs::Entity best = wi::ecs::INVALID_ENTITY;
    size_t bestIndices = 0;
    for (size_t i = 0; i < scene.objects.GetCount(); ++i) {
        const wi::ecs::Entity e = scene.objects.GetEntity(i);
        if (e != imported.root && !scene.Entity_IsDescendant(e, imported.root)) continue;
        const MeshComponent* mesh = scene.meshes.GetComponent(scene.objects[i].meshID);
        if (mesh != nullptr && mesh->indices.size() > bestIndices) {
            best = e;
            bestIndices = mesh->indices.size();
        }
    }
    if (best == wi::ecs::INVALID_ENTITY) {
        scene.Entity_Remove(imported.root);
        return fail("the model has no mesh");
    }
    const wi::ecs::Entity meshEntity = scene.objects.GetComponent(best)->meshID;
    if (MeshComponent* mesh = scene.meshes.GetComponent(meshEntity)) {
        if (mesh->indices.size() >= 300 * 3) st::render::GenerateMeshLODs(*mesh);
    }

    // One entity: the object with the node's rotation and scale, no position (the generator
    //	places it). Copies reference the mesh by id, so the mesh and its materials stay in
    //	the scene, parked under the terrain; the imported node tree goes.
    const wi::ecs::Entity propEntity = wi::ecs::CreateEntity();
    std::string name = modelPath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    scene.names.Create(propEntity) = name;
    scene.objects.Create(propEntity) = *scene.objects.GetComponent(best);
    // Rotation and scale only, and NOT a large-world transform: the generator writes the
    //	position into translation_local, and a large-world UpdateTransform would replace it
    //	with the absolute position the copy was serialized with (every tree at its chunk's
    //	origin).
    TransformComponent& t = scene.transforms.Create(propEntity);
    XMVECTOR scale, rotation, translation;
    XMMatrixDecompose(&scale, &rotation, &translation, scene.ComputeEntityMatrixRecursive(best));
    XMStoreFloat3(&t.scale_local, scale);
    XMStoreFloat4(&t.rotation_local, rotation);
    t.translation_local = XMFLOAT3(0, 0, 0);
    t.SetLargeWorld(false);
    t.SetDirty();

    wi::terrain::Prop prop;
    {
        wi::Archive archive;
        archive.SetReadModeAndResetPos(false);
        wi::ecs::EntitySerializer seri;
        scene.Entity_Serialize(archive, seri, propEntity, Scene::EntitySerializeFlags::NONE);
        archive.WriteData(prop.data);
    }
    ToEngine(settings, prop);

    scene.Entity_Remove(propEntity);
    scene.Entity_Remove(imported.root);   // the node tree; meshes and materials are not in it
    scene.Component_Attach(meshEntity, entity);
    if (const MeshComponent* mesh = scene.meshes.GetComponent(meshEntity))
        for (const MeshComponent::MeshSubset& subset : mesh->subsets)
            if (scene.materials.Contains(subset.materialID)) scene.Component_Attach(subset.materialID, entity);

    wi::scene::Scene* s = &scene;
    // The generator reads the prop list from its threads: change it while it is idle.
    Edit(entity, [s, entity, prop](TerrainData&) {
        if (wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity)) {
            terrain->props.push_back(prop);
            terrain->InvalidateProps();
        }
        return SampleRect();
    });
    MarkDirty(entity);
    return true;
}

void TerrainSystem::UpdateProp (wi::scene::Scene& scene, wi::ecs::Entity entity, uint32_t index,
                             const PropSettings& settings) {
    wi::scene::Scene* s = &scene;
    Edit(entity, [s, entity, index, settings](TerrainData&) {
        wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity);
        if (terrain != nullptr && index < terrain->props.size()) {
            ToEngine(settings, terrain->props[index]);
            terrain->InvalidateProps();
        }
        return SampleRect();
    });
    MarkDirty(entity);
}

void TerrainSystem::DeleteProp (wi::scene::Scene& scene, wi::ecs::Entity entity, uint32_t index) {
    wi::scene::Scene* s = &scene;
    // The mesh stays in the scene (another prop or a placed copy may use it).
    Edit(entity, [s, entity, index](TerrainData&) {
        wi::terrain::Terrain* terrain = s->terrains.GetComponent(entity);
        if (terrain != nullptr && index < terrain->props.size()) {
            terrain->props.erase(terrain->props.begin() + index);
            terrain->InvalidateProps();
        }
        return SampleRect();
    });
    MarkDirty(entity);
}

// ---------------------------------------------------------------------- flush ---

void TerrainSystem::Flush (wi::scene::Scene& scene) {
    for (auto& b : bindings_) {
        wi::terrain::Terrain* terrain = scene.terrains.GetComponent(b->entity);
        if (terrain == nullptr) continue;
        if (!b->dirty && !terrain->authored_data.empty()) continue;
        std::vector<uint8_t> bytes;
        b->data->Encode(bytes);
        terrain->authored_data.assign(bytes.begin(), bytes.end());
        b->flushedHash = HashBytes(terrain->authored_data);
        b->dirty = false;
    }
}

// --------------------------------------------------------------------- query ---

bool TerrainSystem::Sample (float x, float z, GroundSample& out) const {
    if (bindings_.empty()) return false;
    const TerrainData& data = *bindings_.front()->data;
    out.height = data.SampleHeight(x, z);
    out.normal = data.SampleNormal(x, z);
    out.layerCount = data.LayerCount();
    data.SampleWeights(x, z, out.weights);
    out.dominantLayer = 0;
    for (uint32_t l = 1; l < out.layerCount; ++l)
        if (out.weights[l] > out.weights[out.dominantLayer]) out.dominantLayer = l;
    return true;
}

const SoilParams* TerrainSystem::Soil (uint32_t layer) const {
    if (bindings_.empty()) return nullptr;
    const TerrainData& data = *bindings_.front()->data;
    return layer < data.LayerCount() ? &data.layers[layer].soil : nullptr;
}

bool TerrainSystem::Raycast (const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                             XMFLOAT3& hit) const {
    if (bindings_.empty()) return false;
    return bindings_.front()->data->Raycast(origin, direction, maxDistance, hit);
}

} // namespace st::terrain
