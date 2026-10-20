#pragma once
// Draw2D on Wicked: wi::image for images, wi::font for text, and a small pipeline of our
// own for compiled geometry (assets/shaders/StUi{VS,PS}.hlsl - RmlUi's triangles,
// premultiplied alpha). Each Begin() records into its own command list, so a 2D pass
// lands after whatever was recorded before it this frame.

#include "render/detroit/DetroitRenderer.h"

namespace st::render::detroit {

class Draw2DImpl final : public Draw2D {
public:
    TextureRef LoadTexture (const std::string& path) override;
    TextureRef FromResource (const wi::Resource& resource) override;
    TextureRef CreateTexture (const void* rgba8, uint32_t width, uint32_t height, bool premultiplied) override;
    TextureRef CreateRenderTarget (uint32_t width, uint32_t height) override;
    int LoadFont (const std::string& path) override;
    bool GeometryReady () override;
    GeometryRef CompileGeometry (const Vertex2D* vertices, size_t vertexCount,
                                 const int* indices, size_t indexCount) override;

    bool Begin (const TextureObject& target, bool clear, const char* label) override;
    void End () override;
    XMUINT2 TargetSize () const override { return size_; }
    void SetScissor (const XMFLOAT4& ltrb) override;
    void DrawImage (const ImageDraw& image) override;
    void DrawString (const TextDraw& text) override;
    void DrawGeometry (const GeometryObject& geometry, const TextureObject* texture,
                       const XMFLOAT4X4& toTarget) override;

    void Shutdown ();

private:
    bool CreateGeometryPipeline ();

    // compiled-geometry pipeline, made on first use
    bool tried_ = false;
    bool ready_ = false;
    wi::graphics::Shader vs_, ps_;
    wi::graphics::InputLayout layout_;
    wi::graphics::PipelineState pso_;
    wi::graphics::Sampler sampler_;
    wi::graphics::Texture white_;

    // the open pass
    wi::graphics::CommandList cmd_;
    bool open_ = false;
    bool labelled_ = false;
    bool geometryBound_ = false;   // pso_ bound since the last wi::image / wi::font draw
    XMUINT2 size_ = XMUINT2(0, 0);
    XMMATRIX proj_ = XMMatrixIdentity();
};

} // namespace st::render::detroit
