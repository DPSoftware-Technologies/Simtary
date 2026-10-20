#include "render/detroit/DetroitRenderer.h"
#include "render/detroit/DetroitDraw2D.h"

#include "wiRenderer.h"
#include "wiRenderPath3D.h"
#include "wiHelper.h"
#include "wiCanvas.h"
#include "wiVersion.h"
#include "render/GraphicsAPI.h"

namespace st::render::detroit {

DetroitRenderer::DetroitRenderer () : ui_(std::make_unique<Draw2DImpl>()) {}

DetroitRenderer::~DetroitRenderer () {
    if (Installed() && &Get() == this) Install(nullptr);
}

DetroitRenderer* Active () {
    if (!Installed() || Get().GetCaps().backend != Backend::Detroit) return nullptr;
    return static_cast<DetroitRenderer*>(&Get());
}

void DetroitRenderer::BeginFrame () {
    std::vector<DebugShape> shapes;
    {
        std::lock_guard<std::mutex> lock(debugLock_);
        shapes.swap(debug_);
    }
    for (const DebugShape& s : shapes) {
        switch (s.kind) {
        case DebugShape::Kind::Line: {
            wi::renderer::RenderableLine line;
            line.start = s.a;
            line.end = s.b;
            line.color_start = s.colorA;
            line.color_end = s.colorB;
            wi::renderer::DrawLine(line, s.depth);
            break;
        }
        case DebugShape::Kind::Box:
            wi::renderer::DrawBox(s.matrix, s.colorA, s.depth);
            break;
        case DebugShape::Kind::Sphere: {
            wi::primitive::Sphere sphere;
            sphere.center = s.a;
            sphere.radius = s.radius;
            wi::renderer::DrawSphere(sphere, s.colorA, s.depth);
            break;
        }
        }
    }
}


Caps DetroitRenderer::GetCaps () const {
    Caps c;
    c.backend = Backend::Detroit;
    c.name = "Detroit";
    c.codename = wi::version::GetCodename();          // the build's Diesel edition (Detroit, later Gumin / Caterpillar)
    c.architecture = wi::version::GetArchitecture();
    c.api = st::GraphicsAPIName(st::ActiveGraphicsAPI());
    static const std::string basedOn = std::string("Wicked Engine ") + wi::version::GetOGVersionString();
    c.basedOn = basedOn.c_str();
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    c.bindless = true;
    c.compute = true;
    c.rayTracing = device && device->CheckCapability(wi::graphics::GraphicsDeviceCapability::RAYTRACING);
    c.gpuParticles = true;
    c.maxTextureSize = 16384;
    return c;
}

Draw2D& DetroitRenderer::UI () { return *ui_; }

const TextureObject* DetroitRenderer::GameImage () {
    const wi::graphics::Texture* t = gamePath_.GetLastPostprocessRT();
    if (t == nullptr || !t->IsValid()) return nullptr;
    gameImage_.external = t;
    gameImage_.width = t->desc.width;
    gameImage_.height = t->desc.height;
    gameImage_.premultiplied = false;
    return &gameImage_;
}

XMUINT2 DetroitRenderer::GameResolution () const {
    return gamePath_.GetInternalResolution();
}

bool DetroitRenderer::SaveImage (const TextureObject& texture, const std::string& path) {
    const wi::graphics::Texture& t = Native(texture);
    return t.IsValid() && wi::helper::saveTextureToFile(t, path);
}

wi::Resource DetroitRenderer::ToResource (const TextureRef& texture) {
    wi::Resource res;
    if (texture) res.SetTexture(Native(*texture));
    return res;
}

void* DetroitRenderer::ImGuiTexture (const TextureObject& texture) {
    return (void*)&Native(texture);
}

void* DetroitRenderer::ImGuiTexture (const wi::Resource& resource) {
    return resource.IsValid() ? (void*)&resource.GetTexture() : nullptr;
}

wi::primitive::Ray DetroitRenderer::PickRay (float x, float y, float viewWidth, float viewHeight,
                                            const wi::scene::CameraComponent* camera) {
    wi::Canvas canvas;
    canvas.init((uint32_t)std::max(1.0f, viewWidth), (uint32_t)std::max(1.0f, viewHeight));
    return wi::renderer::GetPickRay((long)x, (long)y, canvas, camera ? *camera : wi::scene::GetCamera());
}

void DetroitRenderer::DebugLine (const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& fromColor,
                                 const XMFLOAT4& toColor, bool depth) {
    DebugShape s = {};
    s.kind = DebugShape::Kind::Line;
    s.depth = depth;
    s.a = from;
    s.b = to;
    s.colorA = fromColor;
    s.colorB = toColor;
    std::lock_guard<std::mutex> lock(debugLock_);
    debug_.push_back(s);
}

void DetroitRenderer::DebugBox (const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color, bool depth) {
    DebugShape s = {};
    s.kind = DebugShape::Kind::Box;
    s.depth = depth;
    s.matrix = boxMatrix;
    s.colorA = color;
    std::lock_guard<std::mutex> lock(debugLock_);
    debug_.push_back(s);
}

void DetroitRenderer::DebugSphere (const wi::primitive::Sphere& sphere, const XMFLOAT4& color, bool depth) {
    DebugShape s = {};
    s.kind = DebugShape::Kind::Sphere;
    s.depth = depth;
    s.a = sphere.center;
    s.radius = sphere.radius;
    s.colorA = color;
    std::lock_guard<std::mutex> lock(debugLock_);
    debug_.push_back(s);
}

void DetroitRenderer::SetDebugOverlays (const DebugOverlays& o) {
    if (overlaysSet_ && o == overlays_) return;
    overlays_ = o;
    overlaysSet_ = true;
    wi::renderer::SetToDrawDebugBoneLines(o.boneLines);
    wi::renderer::SetToDrawDebugCameras(o.cameras);
    wi::renderer::SetToDrawDebugColliders(o.colliders);
    wi::renderer::SetToDrawDebugEmitters(o.emitters);
    wi::renderer::SetToDrawDebugEnvProbes(o.envProbes);
    wi::renderer::SetToDrawDebugForceFields(o.forceFields);
    wi::renderer::SetToDrawDebugPartitionTree(o.partitionTree);
    wi::renderer::SetToDrawDebugSprings(o.springs);
    wi::renderer::SetToDrawVoxelHelper(o.voxelHelper, o.voxelClipmapLevel);
    wi::renderer::SetToDrawGridHelper(o.gridHelper);
}

int DetroitRenderer::PipelinesCompiling () const {
    return wi::renderer::IsPipelineCreationActive();
}

std::string DetroitRenderer::AdapterName () const {
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    return device ? device->GetAdapterName() : std::string();
}

bool DetroitRenderer::VideoMemory (uint64_t& used, uint64_t& budget) const {
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    if (device == nullptr) return false;
    const wi::graphics::GraphicsDevice::MemoryUsage m = device->GetMemoryUsage();
    used = m.usage;
    budget = m.budget;
    return true;
}

} // namespace st::render::detroit
