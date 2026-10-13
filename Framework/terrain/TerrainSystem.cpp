#include "terrain/TerrainSystem.h"

#include "wiScene.h"
#include "wiTerrain.h"
#include "wiHelper.h"
#include "wiBacklog.h"

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
