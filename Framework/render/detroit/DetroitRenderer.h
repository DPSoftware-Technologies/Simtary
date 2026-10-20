#pragma once
// DetroitRenderer - st::render on Wicked's renderer (wi::renderer + RenderPath3D, Vulkan 1.3 /
// DX12, bindless). The Detroit, Gumin and Caterpillar editions (EDITIONS.md).
//
// Two halves:
//   - the portable st::render::Renderer interface (render/api/Renderer.h), which is all
//     Framework and game code see;
//   - the host half below it, which st::App (a wi::Application) drives: it owns the game's
//     render path and the effects that ride on it (lens flare, projectors, lasers), the
//     engine graphics options, and the ImGui backend, and composes the window.
//
// Code outside render/detroit that needs the real device (the editor's viewports, the
// optical effects, XR, streaming) is listed as Detroit-only in
// cmake/SimtaryRenderBoundary.cmake and reaches it through detroit::Active().
//
// Main thread only, except DebugLine/DebugBox/DebugSphere (queued under a lock, handed
// to wi::renderer by BeginFrame on the main thread). No exceptions, no RTTI.

#include "render/api/Renderer.h"
#include "render/LensFlare.h"
#include "render/Projector.h"
#include "render/Laser.h"
#include "devui/imgraphicsettings.h"

#include "wiGraphicsDevice.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace wi { class Application; class RenderPath3D; }

namespace st::render::detroit {

// The concrete handles. Code holding a TextureRef from this renderer may static_cast.
class Texture final : public TextureObject {
public:
    wi::graphics::Texture owned;                     // made here (CreateTexture / CreateRenderTarget)
    wi::Resource asset;                              // or a loaded resource's
    const wi::graphics::Texture* external = nullptr; // or someone else's, not kept alive (the game image)

    const wi::graphics::Texture& Get () const {
        if (external != nullptr) return *external;
        if (asset.IsValid()) return asset.GetTexture();
        return owned;
    }
};

inline const wi::graphics::Texture& Native (const TextureObject& t) { return static_cast<const Texture&>(t).Get(); }

class Draw2DImpl;

class DetroitRenderer final : public Renderer {
public:
    DetroitRenderer ();
    ~DetroitRenderer () override;

    // ------------------------------------------------------------ st::render::Renderer
    Caps GetCaps () const override;
    Draw2D& UI () override;
    const TextureObject* GameImage () override;
    XMUINT2 GameResolution () const override;
    bool SaveImage (const TextureObject& texture, const std::string& path) override;
    wi::Resource ToResource (const TextureRef& texture) override;
    void* ImGuiTexture (const TextureObject& texture) override;
    void* ImGuiTexture (const wi::Resource& resource) override;
    wi::primitive::Ray PickRay (float x, float y, float viewWidth, float viewHeight,
                                const wi::scene::CameraComponent* camera) override;
    void DebugLine (const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& fromColor,
                    const XMFLOAT4& toColor, bool depth) override;
    void DebugBox (const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color, bool depth) override;
    void DebugSphere (const wi::primitive::Sphere& sphere, const XMFLOAT4& color, bool depth) override;
    void SetDebugOverlays (const DebugOverlays& overlays) override;
    int PipelinesCompiling () const override;
    std::string AdapterName () const override;
    bool VideoMemory (uint64_t& used, uint64_t& budget) const override;
    void Update (float dt) override;
    void LoadSettings () override;
    void SaveSettings () override;

    // ------------------------------------------------------------------- host (st::App)
    // What the app adds to the frame - its project hooks.
    struct Hooks {
        std::function<void(wi::RenderPath3D&)>            onPathSetup;    // before the path goes live
        std::function<void(wi::graphics::CommandList)>    onPreCompose;   // under the composed frame
        std::function<void(wi::graphics::CommandList)>    onCompose;      // over it, under ImGui
    };

    // After wi::Application::Initialize (device and canvas exist) and the video options:
    //	effects, the game path (activated unless the app activated its own), saved options.
    void Initialize (wi::Application& app, Hooks hooks);
    // The ImGui backend: font atlas, pipeline. The ImGui context must exist. False when the
    //	shaders are missing (logged; ImGui then draws nothing, the app keeps running).
    bool CreateImGuiObjects ();
    // Main thread, once a frame before anything renders: queued debug shapes -> wi::renderer.
    void BeginFrame ();
    // The headset's eyes (XR), configured like the game path.
    void RenderXREyes (float dt);

    enum class ComposeMode : uint8_t {
        Game,       // the game path's image, lens flare, hooks, ImGui
        EditorOnly, // ImGui only - the editor's panels show the 3D themselves
        XRMirror,   // the left eye letterboxed, hooks, ImGui
    };
    void Compose (wi::graphics::CommandList cmd, ComposeMode mode);
    // Before the device goes away.
    void Shutdown ();

    st::ProjectorRenderPath& GamePath () { return gamePath_; }
    st::LensFlare& LensFlare () { return lensFlare_; }
    st::ProjectorSystem& Projectors () { return projectors_; }
    st::LaserSystem& Lasers () { return lasers_; }
    GraphicsSettings& Graphics () { return graphics_; }

private:
    struct DebugShape {
        enum class Kind : uint8_t { Line, Box, Sphere } kind;
        bool depth;
        XMFLOAT3 a, b;
        XMFLOAT4 colorA, colorB;
        XMFLOAT4X4 matrix;
        float radius;
    };

    void RenderImGui (wi::graphics::CommandList cmd);

    wi::Application* app_ = nullptr;
    Hooks hooks_;

    st::ProjectorRenderPath gamePath_;
    st::LensFlare lensFlare_;
    st::ProjectorSystem projectors_;
    st::LaserSystem lasers_;
    GraphicsSettings graphics_;

    Texture gameImage_;
    std::unique_ptr<Draw2DImpl> ui_;
    std::mutex debugLock_;
    std::vector<DebugShape> debug_;
    DebugOverlays overlays_;
    bool overlaysSet_ = false;

    // ImGui backend
    wi::graphics::Shader imguiVS_, imguiPS_;
    wi::graphics::InputLayout imguiLayout_;
    wi::graphics::PipelineState imguiPSO_;
    wi::graphics::Sampler imguiSampler_;
    wi::graphics::Texture imguiFont_;
};

// The installed renderer as Detroit, for Detroit-only code. Null on any other backend.
DetroitRenderer* Active ();

} // namespace st::render::detroit
