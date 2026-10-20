#include "render/hurricanes/HurricanesRenderer.h"

#if defined(SIMTARY_HAS_HURRICANES)

#include "render/hurricanes/HurricanesUI.h"

#include "wiScene.h"
#include "wiTerrain.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiVersion.h"
#include "Utility/stb_image.h"
#include "imgui.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <geometry/SurfaceOrientation.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using wi::ecs::Entity;
namespace fl = filament;
namespace fm = filament::math;

namespace st::render::hurricanes {

namespace {

// Draw2D for a renderer that has none yet: nothing loads, nothing draws, so the game UI and
//	RmlUi skip themselves (their Begin / GeometryReady come back false).
class NullDraw2D final : public Draw2D {
public:
    TextureRef LoadTexture (const std::string&) override { return nullptr; }
    TextureRef FromResource (const wi::Resource&) override { return nullptr; }
    TextureRef CreateTexture (const void*, uint32_t, uint32_t, bool) override { return nullptr; }
    TextureRef CreateRenderTarget (uint32_t, uint32_t) override { return nullptr; }
    int LoadFont (const std::string&) override { return 0; }
    bool GeometryReady () override { return false; }
    GeometryRef CompileGeometry (const Vertex2D*, size_t, const int*, size_t) override { return nullptr; }
    bool Begin (const TextureObject&, bool, const char*) override { return false; }
    void End () override {}
    XMUINT2 TargetSize () const override { return XMUINT2(0, 0); }
    void SetScissor (const XMFLOAT4&) override {}
    void DrawImage (const ImageDraw&) override {}
    void DrawString (const TextDraw&) override {}
    void DrawGeometry (const GeometryObject&, const TextureObject*, const XMFLOAT4X4&) override {}
};

// Wicked is left-handed, Filament right-handed: mirror Z.
fm::float3 Mirror (const XMFLOAT3& v) { return fm::float3(v.x, v.y, -v.z); }

// A Wicked world matrix (row vectors) as a Filament transform (column vectors), mirrored in Z
//	on both sides (S * M * S), which keeps its handedness and so the winding it implies.
fm::mat4f MirrorMatrix (const XMFLOAT4X4& m) {
    // Row-vector M and column-vector M^T share memory, so the floats copy straight across;
    //	S M S flips the sign of every element in exactly one of row 2 / column 2.
    fm::mat4f r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = m.m[c][row];   // column c of the column-vector matrix = row c of Wicked's
            if ((c == 2) != (row == 2)) v = -v;
            r[c][row] = v;
        }
    return r;
}

struct MeshGPU {
    fl::VertexBuffer* vb = nullptr;
    fl::IndexBuffer* ib = nullptr;
    fl::Box box;
    uint32_t subsetCount = 0;
};

struct ObjectGPU {
    utils::Entity entity;
    Entity mesh = wi::ecs::INVALID_ENTITY;
};

} // namespace

struct HurricanesRenderer::Impl {
    fl::Engine* engine = nullptr;
    fl::SwapChain* swapChain = nullptr;
    fl::Renderer* renderer = nullptr;
    fl::Scene* scene = nullptr;
    fl::View* view = nullptr;
    fl::Camera* camera = nullptr;
    utils::Entity cameraEntity;
    utils::Entity sun;
    fl::IndirectLight* ibl = nullptr;
    fl::Skybox* skybox = nullptr;
    fl::Material* lit = nullptr;
    fl::Texture* white = nullptr;
    NullDraw2D ui;
    std::unique_ptr<Overlay> overlay;   // the 2D: game UI, RmlUi, ImGui
    XMUINT2 size = XMUINT2(0, 0);

    std::unordered_map<Entity, MeshGPU> meshes;
    std::unordered_map<Entity, ObjectGPU> objects;
    std::unordered_map<Entity, fl::MaterialInstance*> materials;
    struct Tex {
        fl::Texture* texture = nullptr;   // null: not decodable here (white is bound instead)
        XMFLOAT4 average = XMFLOAT4(1, 1, 1, 1);   // linear, for terrain layer tints
    };
    std::unordered_map<std::string, Tex> textures;   // base colour maps by name

    bool LoadMaterials ();
    const Tex& TextureFor (const std::string& name);
    // `terrain`: a terrain chunk's material. Its colour is the vertex colour, and its emissive
    //	is dropped - Wicked gives chunks emissive strength 100 against a virtual-texture emissive
    //	map that is black, which this renderer does not have.
    fl::MaterialInstance* MaterialFor (const wi::scene::Scene& scene, Entity material, bool terrain);
    // `colors`: one RGBA8 per vertex (terrain layer blend), null for white.
    MeshGPU* MeshFor (const wi::scene::MeshComponent& mesh, Entity e, const uint32_t* colors);
    // A terrain chunk's layer weights as vertex colours: each layer's base colour (times its
    //	texture's average), stacked the way the engine's blend shader stacks them.
    std::vector<uint32_t> TerrainColors (const wi::scene::Scene& scene, const wi::terrain::Terrain& terrain,
                                         const wi::terrain::ChunkData& chunk, size_t vertexCount);
    void Sync (const wi::scene::Scene& scene);
    void SyncCamera (const wi::scene::CameraComponent& cam);
    void SyncSun (const wi::scene::Scene& scene);
    void Destroy ();
};

// ---------------------------------------------------------------------- startup ---

HurricanesRenderer::HurricanesRenderer (void* nativeWindow) : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.engine = fl::Engine::Builder().backend(fl::Engine::Backend::OPENGL).build();
    if (d.engine == nullptr) {
        wi::backlog::post("[hurricanes] Filament engine could not start (OpenGL)", wi::backlog::LogLevel::Error);
        return;
    }
    d.swapChain = d.engine->createSwapChain(nativeWindow);
    d.renderer = d.engine->createRenderer();
    d.scene = d.engine->createScene();
    d.view = d.engine->createView();
    d.cameraEntity = utils::EntityManager::get().create();
    d.camera = d.engine->createCamera(d.cameraEntity);
    // Sunny-16: what a sun of ~100 000 lux is exposed for.
    d.camera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
    d.view->setScene(d.scene);
    d.view->setCamera(d.camera);

    fl::Renderer::ClearOptions clear;
    clear.clearColor = { 0.4, 0.55, 0.75, 1.0 };
    clear.clear = true;
    d.renderer->setClearOptions(clear);

    // Sky and ambient. A flat sky colour and a constant (band 0) irradiance until the
    //	weather is mirrored properly.
    d.skybox = fl::Skybox::Builder().color({ 0.45f, 0.6f, 0.85f, 1.0f }).build(*d.engine);
    d.scene->setSkybox(d.skybox);
    const fm::float3 sh[1] = { fm::float3(0.65f, 0.75f, 0.9f) };
    d.ibl = fl::IndirectLight::Builder().irradiance(1, sh).intensity(6000.0f).build(*d.engine);
    d.scene->setIndirectLight(d.ibl);

    d.sun = utils::EntityManager::get().create();
    fl::LightManager::Builder(fl::LightManager::Type::SUN)
        .color({ 1.0f, 0.97f, 0.92f })
        .intensity(110000.0f)
        .direction({ 0.3f, -1.0f, -0.4f })
        .castShadows(true)
        .build(*d.engine, d.sun);
    d.scene->addEntity(d.sun);

    if (!d.LoadMaterials()) return;
    d.overlay = std::make_unique<Overlay>(*d.engine);
    if (!d.overlay->Ready()) d.overlay.reset();
    wi::backlog::post("[hurricanes] Filament " + std::string("OpenGL") + " renderer ready");
}

HurricanesRenderer::~HurricanesRenderer () {
    if (Installed() && &Get() == this) Install(nullptr);
    Shutdown();
}

bool HurricanesRenderer::Ready () const { return impl_ && impl_->engine && impl_->lit; }

bool HurricanesRenderer::Impl::LoadMaterials () {
    wi::vector<uint8_t> data;
    const std::string path = wi::helper::GetCurrentPath() + "/shaders/filament/StLit.filamat";
    if (!wi::helper::FileRead(path, data)) {
        wi::backlog::post("[hurricanes] " + path + " missing - nothing will draw", wi::backlog::LogLevel::Error);
        return false;
    }
    lit = fl::Material::Builder().package(data.data(), data.size()).build(*engine);

    const uint32_t px = 0xFFFFFFFFu;
    white = fl::Texture::Builder().width(1).height(1).levels(1)
        .format(fl::Texture::InternalFormat::RGBA8).sampler(fl::Texture::Sampler::SAMPLER_2D).build(*engine);
    uint32_t* copy = new uint32_t(px);
    white->setImage(*engine, 0, fl::Texture::PixelBufferDescriptor(copy, 4, fl::Texture::Format::RGBA, fl::Texture::Type::UBYTE,
        [](void* buffer, size_t, void*) { delete (uint32_t*)buffer; }));
    return lit != nullptr;
}

void HurricanesRenderer::Shutdown () {
    if (impl_) impl_->Destroy();
}

void HurricanesRenderer::Impl::Destroy () {
    if (engine == nullptr) return;
    if (overlay) {
        overlay->Destroy();
        overlay.reset();
    }
    for (auto& kv : objects) {
        scene->remove(kv.second.entity);
        engine->destroy(kv.second.entity);
        utils::EntityManager::get().destroy(kv.second.entity);
    }
    objects.clear();
    for (auto& kv : meshes) {
        engine->destroy(kv.second.vb);
        engine->destroy(kv.second.ib);
    }
    meshes.clear();
    for (auto& kv : materials) engine->destroy(kv.second);
    materials.clear();
    for (auto& kv : textures) if (kv.second.texture) engine->destroy(kv.second.texture);
    textures.clear();
    if (white) engine->destroy(white);
    if (lit) engine->destroy(lit);
    engine->destroy(sun);
    engine->destroy(ibl);
    engine->destroy(skybox);
    engine->destroyCameraComponent(cameraEntity);
    utils::EntityManager::get().destroy(cameraEntity);
    utils::EntityManager::get().destroy(sun);
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroy(renderer);
    engine->destroy(swapChain);
    fl::Engine::destroy(&engine);
    engine = nullptr;
}

// ------------------------------------------------------------------- scene sync ---

const HurricanesRenderer::Impl::Tex& HurricanesRenderer::Impl::TextureFor (const std::string& name) {
    static const Tex none;
    if (name.empty()) return none;
    auto it = textures.find(name);
    if (it != textures.end()) return it->second;
    Tex t;
    // Wicked's device is the null one in a Hurricanes run, so its resources hold no
    //	pixels: decode the file again (through FileRead, so out of the asset package too).
    //	stb covers PNG / JPG / TGA / BMP; DDS / KTX2 fall back to white for now.
    wi::vector<uint8_t> file;
    if (wi::helper::FileRead(name, file)) {
        int w = 0, h = 0, n = 0;
        stbi_uc* pixels = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
        if (pixels) {
            double sum[4] = { 0, 0, 0, 0 };
            const size_t count = size_t(w) * h;
            const size_t step = std::max<size_t>(1, count / 4096);
            size_t taken = 0;
            for (size_t i = 0; i < count; i += step, ++taken)
                for (int c = 0; c < 4; ++c) {
                    const double v = pixels[i * 4 + c] / 255.0;
                    sum[c] += c < 3 ? v * v : v;   // ~sRGB to linear
                }
            if (taken > 0) t.average = XMFLOAT4(float(sum[0] / taken), float(sum[1] / taken), float(sum[2] / taken), float(sum[3] / taken));
            uint8_t levels = 1;
            while ((std::max(w, h) >> levels) > 0) ++levels;
            t.texture = fl::Texture::Builder().width(w).height(h).levels(levels)
                .format(fl::Texture::InternalFormat::SRGB8_A8).sampler(fl::Texture::Sampler::SAMPLER_2D)
                .usage(fl::Texture::Usage::DEFAULT | fl::Texture::Usage::GEN_MIPMAPPABLE).build(*engine);
            t.texture->setImage(*engine, 0, fl::Texture::PixelBufferDescriptor(pixels, count * 4,
                fl::Texture::Format::RGBA, fl::Texture::Type::UBYTE,
                [](void* buffer, size_t, void*) { stbi_image_free(buffer); }));
            t.texture->generateMipmaps(*engine);
        }
    }
    return textures[name] = t;
}

fl::MaterialInstance* HurricanesRenderer::Impl::MaterialFor (const wi::scene::Scene& scene, Entity e, bool terrain) {
    fl::MaterialInstance*& mi = materials[e];
    if (mi == nullptr) mi = lit->createInstance();
    const wi::scene::MaterialComponent* m = scene.materials.GetComponent(e);
    wi::scene::MaterialComponent fallback;
    if (m == nullptr) m = &fallback;
    mi->setParameter("baseColor", fm::float4(m->baseColor.x, m->baseColor.y, m->baseColor.z, m->baseColor.w));
    mi->setParameter("roughness", m->roughness);
    mi->setParameter("metallic", m->metalness);
    const float e_ = terrain ? 0.0f : m->emissiveColor.w;
    mi->setParameter("emissive", fm::float3(m->emissiveColor.x * e_, m->emissiveColor.y * e_, m->emissiveColor.z * e_));
    fl::TextureSampler sampler(fl::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
                               fl::TextureSampler::MagFilter::LINEAR, fl::TextureSampler::WrapMode::REPEAT);
    const Tex& map = TextureFor(m->textures[wi::scene::MaterialComponent::BASECOLORMAP].name);
    mi->setParameter("baseColorMap", map.texture ? map.texture : white, sampler);
    return mi;
}

MeshGPU* HurricanesRenderer::Impl::MeshFor (const wi::scene::MeshComponent& mesh, Entity e, const uint32_t* colors) {
    auto it = meshes.find(e);
    if (it != meshes.end()) return &it->second;
    const size_t n = mesh.vertex_positions.size();
    if (n == 0 || mesh.indices.empty()) return nullptr;

    struct Vertex {
        fm::float3 position;
        fm::short4 tangents;
        fm::float2 uv;
        uint32_t color;
    };
    std::vector<fm::float3> normals(n, fm::float3(0, 1, 0));
    for (size_t i = 0; i < n && i < mesh.vertex_normals.size(); ++i) normals[i] = Mirror(mesh.vertex_normals[i]);
    filament::geometry::SurfaceOrientation* orientation =
        filament::geometry::SurfaceOrientation::Builder().vertexCount(n).normals(normals.data()).build();
    std::vector<fm::short4> quats(n);
    orientation->getQuats(quats.data(), n);
    delete orientation;

    Vertex* vertices = new Vertex[n];
    fm::float3 lo(1e30f), hi(-1e30f);
    for (size_t i = 0; i < n; ++i) {
        vertices[i].position = Mirror(mesh.vertex_positions[i]);
        vertices[i].tangents = quats[i];
        vertices[i].uv = i < mesh.vertex_uvset_0.size()
            ? fm::float2(mesh.vertex_uvset_0[i].x, 1.0f - mesh.vertex_uvset_0[i].y) : fm::float2(0, 0);   // Filament's v runs bottom-up
        vertices[i].color = colors ? colors[i] : 0xFFFFFFFFu;
        lo = min(lo, vertices[i].position);
        hi = max(hi, vertices[i].position);
    }
    MeshGPU g;
    g.vb = fl::VertexBuffer::Builder().vertexCount((uint32_t)n).bufferCount(1)
        .attribute(fl::VertexAttribute::POSITION, 0, fl::VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
        .attribute(fl::VertexAttribute::TANGENTS, 0, fl::VertexBuffer::AttributeType::SHORT4, offsetof(Vertex, tangents), sizeof(Vertex))
        .normalized(fl::VertexAttribute::TANGENTS)
        .attribute(fl::VertexAttribute::UV0, 0, fl::VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
        .attribute(fl::VertexAttribute::COLOR, 0, fl::VertexBuffer::AttributeType::UBYTE4, offsetof(Vertex, color), sizeof(Vertex))
        .normalized(fl::VertexAttribute::COLOR)
        .build(*engine);
    g.vb->setBufferAt(*engine, 0, fl::VertexBuffer::BufferDescriptor(vertices, n * sizeof(Vertex),
        [](void* buffer, size_t, void*) { delete[] (Vertex*)buffer; }));

    uint32_t* indices = new uint32_t[mesh.indices.size()];
    std::memcpy(indices, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
    g.ib = fl::IndexBuffer::Builder().indexCount((uint32_t)mesh.indices.size())
        .bufferType(fl::IndexBuffer::IndexType::UINT).build(*engine);
    g.ib->setBuffer(*engine, fl::IndexBuffer::BufferDescriptor(indices, mesh.indices.size() * sizeof(uint32_t),
        [](void* buffer, size_t, void*) { delete[] (uint32_t*)buffer; }));
    g.box = fl::Box().set(lo, hi);
    g.subsetCount = mesh.subsets_per_lod > 0 ? mesh.subsets_per_lod : (uint32_t)mesh.subsets.size();
    return &(meshes[e] = g);
}

std::vector<uint32_t> HurricanesRenderer::Impl::TerrainColors (const wi::scene::Scene& scene,
        const wi::terrain::Terrain& terrain, const wi::terrain::ChunkData& chunk, size_t n) {
    std::vector<XMFLOAT3> layer;
    for (Entity me : terrain.materialEntities) {
        XMFLOAT3 c(0.5f, 0.5f, 0.5f);
        if (const wi::scene::MaterialComponent* m = scene.materials.GetComponent(me)) {
            const Tex& t = TextureFor(m->textures[wi::scene::MaterialComponent::BASECOLORMAP].name);
            c = XMFLOAT3(m->baseColor.x * t.average.x, m->baseColor.y * t.average.y, m->baseColor.z * t.average.z);
        }
        layer.push_back(c);
    }
    std::vector<uint32_t> out(n, 0xFFFFFFFFu);
    if (layer.empty()) return out;
    for (size_t v = 0; v < n; ++v) {
        XMFLOAT3 c = layer[0];
        for (size_t i = 1; i < layer.size() && i < chunk.blendmap_layers.size(); ++i) {
            if (v >= chunk.blendmap_layers[i].pixels.size()) continue;
            const float w = chunk.blendmap_layers[i].pixels[v] / 255.0f;
            c.x += (layer[i].x - c.x) * w;
            c.y += (layer[i].y - c.y) * w;
            c.z += (layer[i].z - c.z) * w;
        }
        const uint32_t r = (uint32_t)std::min(255.0f, c.x * 255.0f + 0.5f);
        const uint32_t g = (uint32_t)std::min(255.0f, c.y * 255.0f + 0.5f);
        const uint32_t b = (uint32_t)std::min(255.0f, c.z * 255.0f + 0.5f);
        out[v] = r | (g << 8) | (b << 16) | (0xFFu << 24);   // COLOR is read as linear
    }
    return out;
}

void HurricanesRenderer::Impl::Sync (const wi::scene::Scene& scene) {
    // Terrain chunks by their object entity: their colour comes from the layer weights.
    std::unordered_map<Entity, std::pair<const wi::terrain::Terrain*, const wi::terrain::ChunkData*>> chunks;
    for (size_t i = 0; i < scene.terrains.GetCount(); ++i)
        for (const auto& kv : scene.terrains[i].chunks)
            if (kv.second.entity != wi::ecs::INVALID_ENTITY) chunks[kv.second.entity] = { &scene.terrains[i], &kv.second };

    std::unordered_set<Entity> live;
    fl::TransformManager& tm = engine->getTransformManager();
    for (size_t i = 0; i < scene.objects.GetCount(); ++i) {
        const wi::scene::ObjectComponent& object = scene.objects[i];
        const Entity e = scene.objects.GetEntity(i);
        const wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(object.meshID);
        if (mesh == nullptr || !object.IsRenderable()) continue;
        if (const wi::scene::LayerComponent* layer = scene.layers.GetComponent(e); layer && layer->layerMask == 0) continue;
        const wi::scene::TransformComponent* t = scene.transforms.GetComponent(e);
        if (t == nullptr) continue;

        auto found = objects.find(e);
        if (found != objects.end() && found->second.mesh != object.meshID) {   // the object swapped mesh
            this->scene->remove(found->second.entity);
            engine->destroy(found->second.entity);
            utils::EntityManager::get().destroy(found->second.entity);
            objects.erase(found);
            found = objects.end();
        }
        if (found == objects.end()) {
            std::vector<uint32_t> colors;
            const bool isChunk = chunks.count(e) > 0;
            if (auto c = chunks.find(e); c != chunks.end())
                colors = TerrainColors(scene, *c->second.first, *c->second.second, mesh->vertex_positions.size());
            MeshGPU* g = MeshFor(*mesh, object.meshID, colors.empty() ? nullptr : colors.data());
            if (g == nullptr || g->subsetCount == 0) continue;
            ObjectGPU o;
            o.entity = utils::EntityManager::get().create();
            o.mesh = object.meshID;
            fl::RenderableManager::Builder builder(g->subsetCount);
            builder.boundingBox(g->box).culling(true).castShadows(true).receiveShadows(true);
            for (uint32_t s = 0; s < g->subsetCount; ++s) {
                const auto& subset = mesh->subsets[s];
                builder.geometry(s, fl::RenderableManager::PrimitiveType::TRIANGLES, g->vb, g->ib,
                                 subset.indexOffset, subset.indexCount);
                builder.material(s, MaterialFor(scene, subset.materialID, isChunk));
            }
            builder.build(*engine, o.entity);
            this->scene->addEntity(o.entity);
            found = objects.emplace(e, o).first;
        } else {
            for (uint32_t s = 0; s < meshes[object.meshID].subsetCount; ++s)
                MaterialFor(scene, mesh->subsets[s].materialID, chunks.count(e) > 0);
        }
        tm.setTransform(tm.getInstance(found->second.entity), MirrorMatrix(t->world));
        live.insert(e);
    }
    for (auto it = objects.begin(); it != objects.end();) {
        if (live.count(it->first)) { ++it; continue; }
        this->scene->remove(it->second.entity);
        engine->destroy(it->second.entity);
        utils::EntityManager::get().destroy(it->second.entity);
        it = objects.erase(it);
    }
}

void HurricanesRenderer::Impl::SyncCamera (const wi::scene::CameraComponent& cam) {
    const fm::float3 eye = Mirror(cam.Eye);
    const fm::float3 dir = Mirror(cam.At);
    const fm::float3 up = Mirror(cam.Up);
    camera->lookAt(fm::double3(eye), fm::double3(eye + dir), fm::double3(up));
    const double aspect = size.y > 0 ? double(size.x) / double(size.y) : 16.0 / 9.0;
    camera->setProjection(cam.fov * 180.0 / XM_PI, aspect, std::max(0.05f, cam.zNearP),
                          std::max(cam.zNearP + 1.0f, cam.zFarP), fl::Camera::Fov::VERTICAL);
}

void HurricanesRenderer::Impl::SyncSun (const wi::scene::Scene& scene) {
    for (size_t i = 0; i < scene.lights.GetCount(); ++i) {
        const wi::scene::LightComponent& l = scene.lights[i];
        if (l.GetType() != wi::scene::LightComponent::DIRECTIONAL) continue;
        fl::LightManager& lm = engine->getLightManager();
        const auto inst = lm.getInstance(sun);
        // Wicked's `direction` points at the light; Filament wants where the light travels.
        const fm::float3 d = -Mirror(l.direction);
        lm.setDirection(inst, normalize(d));
        lm.setColor(inst, { l.color.x, l.color.y, l.color.z });
        // Wicked's sun intensities are scene units (DayNight drives ~14 at noon), Filament's
        //	are lux against the camera's sunny-16 exposure: a fixed scale for the spike.
        lm.setIntensity(inst, std::max(0.0f, l.intensity) * 8000.0f);
        return;
    }
}

// ------------------------------------------------------------------------ frame ---

void HurricanesRenderer::Update (float) {}

void HurricanesRenderer::RenderFrame (uint32_t width, uint32_t height) {
    if (!Ready()) return;
    Impl& d = *impl_;
    d.size = XMUINT2(width, height);
    d.view->setViewport({ 0, 0, width, height });
    if (d.overlay) d.overlay->SetScreenSize(width, height);
    const wi::scene::Scene& scene = wi::scene::GetScene();
    d.Sync(scene);
    d.SyncCamera(wi::scene::GetCamera());
    d.SyncSun(scene);
    if (d.renderer->beginFrame(d.swapChain)) {
        d.renderer->render(d.view);
        if (d.overlay) {
            // The game UI was queued during App::Render; ImGui's draw data was built in Update.
            d.overlay->AddImGui(ImGui::GetCurrentContext() ? ImGui::GetDrawData() : nullptr);
            d.overlay->Render(*d.renderer);
        }
        d.renderer->endFrame();
    } else if (d.overlay) {
        d.overlay->Render(*d.renderer);   // frame skipped: drop the queue (Render empties it)
    }
}

// ------------------------------------------------------------------------- info ---

Caps HurricanesRenderer::GetCaps () const {
    Caps c;
    c.backend = Backend::Hurricanes;
    c.name = "Hurricanes";
    c.codename = "Hurricanes";
    c.architecture = "Petrol";
    c.api = "OpenGL";
    c.basedOn = "Filament " ST_FILAMENT_VERSION;
    c.bindless = false;
    c.compute = false;
    c.gpuParticles = false;
    c.maxTextureSize = 4096;
    return c;
}

Draw2D& HurricanesRenderer::UI () {
    if (impl_->overlay) return *impl_->overlay;
    return impl_->ui;
}

const TextureObject* HurricanesRenderer::GameImage () {
    return impl_->overlay ? impl_->overlay->Screen() : nullptr;
}

void* HurricanesRenderer::ImGuiTexture (const TextureObject& texture) {
    return impl_->overlay ? impl_->overlay->ImGuiTexture(texture) : nullptr;
}

void* HurricanesRenderer::ImGuiTexture (const wi::Resource& resource) {
    return impl_->overlay ? impl_->overlay->ImGuiTexture(resource) : nullptr;
}

XMUINT2 HurricanesRenderer::GameResolution () const { return impl_->size; }

std::string HurricanesRenderer::AdapterName () const { return "OpenGL (Filament)"; }

wi::primitive::Ray HurricanesRenderer::PickRay (float x, float y, float viewWidth, float viewHeight,
                                                const wi::scene::CameraComponent* camera) {
    // Same math as Wicked's GetPickRay, from the camera's own matrices (scene data).
    const wi::scene::CameraComponent& cam = camera ? *camera : wi::scene::GetCamera();
    const float nx = (x / std::max(1.0f, viewWidth)) * 2.0f - 1.0f;
    const float ny = 1.0f - (y / std::max(1.0f, viewHeight)) * 2.0f;
    const XMMATRIX inv = cam.GetInvViewProjection();
    XMVECTOR a = XMVector3TransformCoord(XMVectorSet(nx, ny, 1.0f, 1.0f), inv);   // reverse Z: near = 1
    XMVECTOR b = XMVector3TransformCoord(XMVectorSet(nx, ny, 0.0f, 1.0f), inv);
    XMFLOAT3 o, dir;
    XMStoreFloat3(&o, a);
    XMStoreFloat3(&dir, XMVector3Normalize(b - a));
    return wi::primitive::Ray(o, dir);
}

} // namespace st::render::hurricanes

#else // !SIMTARY_HAS_HURRICANES

// Not in this build (SIMTARY_RENDERERS without Hurricanes): a renderer that never becomes
//	Ready(), so st::App never installs it. Defined only so the host links either way.
namespace st::render::hurricanes {

namespace {
class NullDraw2D final : public Draw2D {
public:
    TextureRef LoadTexture (const std::string&) override { return nullptr; }
    TextureRef FromResource (const wi::Resource&) override { return nullptr; }
    TextureRef CreateTexture (const void*, uint32_t, uint32_t, bool) override { return nullptr; }
    TextureRef CreateRenderTarget (uint32_t, uint32_t) override { return nullptr; }
    int LoadFont (const std::string&) override { return 0; }
    bool GeometryReady () override { return false; }
    GeometryRef CompileGeometry (const Vertex2D*, size_t, const int*, size_t) override { return nullptr; }
    bool Begin (const TextureObject&, bool, const char*) override { return false; }
    void End () override {}
    XMUINT2 TargetSize () const override { return XMUINT2(0, 0); }
    void SetScissor (const XMFLOAT4&) override {}
    void DrawImage (const ImageDraw&) override {}
    void DrawString (const TextDraw&) override {}
    void DrawGeometry (const GeometryObject&, const TextureObject*, const XMFLOAT4X4&) override {}
};
} // namespace

struct HurricanesRenderer::Impl {
    NullDraw2D ui;
};

HurricanesRenderer::HurricanesRenderer (void*) : impl_(std::make_unique<Impl>()) {}
HurricanesRenderer::~HurricanesRenderer () = default;
bool HurricanesRenderer::Ready () const { return false; }
void HurricanesRenderer::RenderFrame (uint32_t, uint32_t) {}
void HurricanesRenderer::Shutdown () {}
void HurricanesRenderer::Update (float) {}
Caps HurricanesRenderer::GetCaps () const { Caps c; c.backend = Backend::Hurricanes; c.name = "Hurricanes"; return c; }
Draw2D& HurricanesRenderer::UI () { return impl_->ui; }
const TextureObject* HurricanesRenderer::GameImage () { return nullptr; }
void* HurricanesRenderer::ImGuiTexture (const TextureObject&) { return nullptr; }
void* HurricanesRenderer::ImGuiTexture (const wi::Resource&) { return nullptr; }
XMUINT2 HurricanesRenderer::GameResolution () const { return XMUINT2(0, 0); }
std::string HurricanesRenderer::AdapterName () const { return std::string(); }
wi::primitive::Ray HurricanesRenderer::PickRay (float, float, float, float, const wi::scene::CameraComponent*) {
    return wi::primitive::Ray();
}

} // namespace st::render::hurricanes

#endif // SIMTARY_HAS_HURRICANES
