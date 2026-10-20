#pragma once
// The Hurricanes renderer's 2D overlay (internal to render/hurricanes; Filament types).
//
// Filament has no immediate-mode 2D, so everything 2D - the game UI and RmlUi through Draw2D,
// and ImGui - is collected into ONE triangle batch per frame and drawn as one renderable (one
// primitive per texture / scissor change, kept in order with blendOrder) in an orthographic
// view over the 3D: pixels, (0, 0) top left, no post-processing. Shapes are built on the CPU:
// rounded corners are a fan, a gradient is vertex colour, rotation turns the vertices. Text is
// stb_truetype: one glyph atlas per font and pixel size (Latin-1), the engine's Liberation Sans
// as font 0.
//
// Off-screen targets (UI world canvases) are not supported yet: CreateRenderTarget returns null.
//
// Main thread only. No exceptions, no RTTI.

#include "render/api/Renderer.h"

#include <utils/Entity.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filament {
class Engine; class Renderer; class View; class Scene; class Camera; class Material;
class MaterialInstance; class Texture; class VertexBuffer; class IndexBuffer;
}
struct ImDrawData;

namespace st::render::hurricanes {

class Overlay final : public Draw2D {
public:
    explicit Overlay (filament::Engine& engine);
    ~Overlay () override;
    bool Ready () const { return material_ != nullptr; }

    // ------------------------------------------------------------------- Draw2D
    TextureRef LoadTexture (const std::string& path) override;
    TextureRef FromResource (const wi::Resource& resource) override;
    TextureRef CreateTexture (const void* rgba8, uint32_t width, uint32_t height, bool premultiplied) override;
    TextureRef CreateRenderTarget (uint32_t, uint32_t) override { return nullptr; }
    int LoadFont (const std::string& path) override;
    bool GeometryReady () override { return Ready(); }
    GeometryRef CompileGeometry (const Vertex2D* vertices, size_t vertexCount,
                                 const int* indices, size_t indexCount) override;
    bool Begin (const TextureObject& target, bool clear, const char* label) override;
    void End () override { open_ = false; }
    XMUINT2 TargetSize () const override { return size_; }
    void SetScissor (const XMFLOAT4& ltrb) override;
    void DrawImage (const ImageDraw& image) override;
    void DrawString (const TextDraw& text) override;
    void DrawGeometry (const GeometryObject& geometry, const TextureObject* texture,
                       const XMFLOAT4X4& toTarget) override;

    // --------------------------------------------------------------- renderer side
    // The window size this frame - the overlay's pixels.
    void SetScreenSize (uint32_t width, uint32_t height);
    // The target that means "over the game frame" (Renderer::GameImage()).
    const TextureObject* Screen () { return &screen_; }
    void* ImGuiTexture (const TextureObject& texture) const;
    void* ImGuiTexture (const wi::Resource& resource);
    // Appended after whatever Draw2D queued, so ImGui is on top.
    void AddImGui (const ImDrawData* data);
    // Draws the queued batch (inside the renderer's beginFrame/endFrame) and empties it.
    void Render (filament::Renderer& renderer);
    void Destroy ();

    struct Font;
    struct Atlas;

private:
    struct Vtx {
        float x, y, u, v;
        uint32_t color;
    };
    struct Cmd {
        filament::Texture* texture;
        bool texPremul, colorPremul;
        int32_t sx, sy, sw, sh;   // scissor, top-left origin
        uint32_t first, count;
    };

    filament::Texture* Upload (const void* rgba8, uint32_t w, uint32_t h);
    TextureRef Wrap (filament::Texture* texture, uint32_t w, uint32_t h, bool premultiplied, bool owned);
    // Starts a command for this texture / blend / the current scissor unless the last one fits.
    void Use (filament::Texture* texture, bool texPremul, bool colorPremul);
    void Use (filament::Texture* texture, bool texPremul, bool colorPremul, int32_t sx, int32_t sy, int32_t sw, int32_t sh);
    Atlas* AtlasFor (int font, int pixelSize);

    filament::Engine& engine_;
    filament::Material* material_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::Camera* camera_ = nullptr;
    utils::Entity cameraEntity_;
    utils::Entity renderable_;
    filament::VertexBuffer* vb_ = nullptr;
    filament::IndexBuffer* ib_ = nullptr;
    uint32_t vbCapacity_ = 0, ibCapacity_ = 0;
    std::vector<filament::MaterialInstance*> pool_;
    filament::Texture* white_ = nullptr;
    filament::Texture* imguiFont_ = nullptr;

    class ScreenTarget final : public TextureObject {};
    ScreenTarget screen_;
    XMUINT2 size_ = XMUINT2(0, 0);
    bool open_ = false;
    XMFLOAT4 scissor_ = XMFLOAT4(0, 0, 0, 0);

    std::vector<Vtx> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<Cmd> cmds_;

    std::unordered_set<const void*> live_;   // textures this overlay made: valid ImGui ids
    std::vector<std::unique_ptr<Font>> fonts_;
    std::unordered_map<uint64_t, std::unique_ptr<Atlas>> atlases_;
    std::shared_ptr<bool> alive_;            // textures outliving the engine skip their destroy
};

} // namespace st::render::hurricanes
