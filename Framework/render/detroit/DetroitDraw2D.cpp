#include "render/detroit/DetroitDraw2D.h"

#include "wiRenderer.h"
#include "wiImage.h"
#include "wiFont.h"
#include "wiBacklog.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace wi::graphics;

namespace st::render::detroit {

namespace {

struct GeometryImpl final : GeometryObject {
    GPUBuffer vb, ib;
    uint32_t  count = 0;
};

struct GeometryConstants {
    XMFLOAT4X4 transform;
    XMFLOAT4   flags;   // x: premultiply the texture in the shader (a file texture is straight alpha)
};

wi::font::Alignment FontAlign (Align a, bool horizontal) {
    if (horizontal)
        return a == Align::Start ? wi::font::WIFALIGN_LEFT : a == Align::End ? wi::font::WIFALIGN_RIGHT : wi::font::WIFALIGN_CENTER;
    return a == Align::Start ? wi::font::WIFALIGN_TOP : a == Align::End ? wi::font::WIFALIGN_BOTTOM : wi::font::WIFALIGN_CENTER;
}

} // namespace

// ---------------------------------------------------------------------- resources ---

TextureRef Draw2DImpl::LoadTexture (const std::string& path) {
    if (path.empty()) return nullptr;
    wi::Resource res = wi::resourcemanager::Load(path);
    if (!res.IsValid() || !res.GetTexture().IsValid()) return nullptr;
    return FromResource(res);
}

TextureRef Draw2DImpl::FromResource (const wi::Resource& resource) {
    if (!resource.IsValid() || !resource.GetTexture().IsValid()) return nullptr;
    auto t = std::make_shared<Texture>();
    t->asset = resource;
    t->width = resource.GetTexture().desc.width;
    t->height = resource.GetTexture().desc.height;
    t->premultiplied = false;
    return t;
}

TextureRef Draw2DImpl::CreateTexture (const void* rgba8, uint32_t width, uint32_t height, bool premultiplied) {
    if (rgba8 == nullptr || width == 0 || height == 0) return nullptr;
    auto t = std::make_shared<Texture>();
    TextureDesc td;
    td.width = width;
    td.height = height;
    td.format = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::SHADER_RESOURCE;
    SubresourceData sd;
    sd.data_ptr = rgba8;
    sd.row_pitch = width * 4;
    sd.slice_pitch = sd.row_pitch * height;
    if (!GetDevice()->CreateTexture(&td, &sd, &t->owned)) return nullptr;
    t->width = width;
    t->height = height;
    t->premultiplied = premultiplied;
    return t;
}

TextureRef Draw2DImpl::CreateRenderTarget (uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return nullptr;
    auto t = std::make_shared<Texture>();
    TextureDesc td;
    td.width = width;
    td.height = height;
    td.format = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE;
    td.layout = ResourceState::SHADER_RESOURCE;
    td.clear.color[0] = td.clear.color[1] = td.clear.color[2] = 0;
    td.clear.color[3] = 1;
    GraphicsDevice* device = GetDevice();
    if (!device->CreateTexture(&td, nullptr, &t->owned)) return nullptr;
    device->SetName(&t->owned, "st::render 2D target");
    t->width = width;
    t->height = height;
    t->premultiplied = true;
    return t;
}

int Draw2DImpl::LoadFont (const std::string& path) {
    return path.empty() ? 0 : wi::font::AddFontStyle(path);
}

bool Draw2DImpl::GeometryReady () {
    if (!tried_) {
        tried_ = true;
        ready_ = CreateGeometryPipeline();
    }
    return ready_;
}

bool Draw2DImpl::CreateGeometryPipeline () {
    if (!wi::renderer::LoadShader(ShaderStage::VS, vs_, "StUiVS.cso") ||
        !wi::renderer::LoadShader(ShaderStage::PS, ps_, "StUiPS.cso")) {
        wi::backlog::post("[render] StUiVS.cso / StUiPS.cso missing from " + wi::renderer::GetShaderPath() +
                          " - UI documents will not draw", wi::backlog::LogLevel::Error);
        return false;
    }
    layout_.elements = {
        { "POSITION", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(Vertex2D, position), InputClassification::PER_VERTEX_DATA },
        { "COLOR",    0, Format::R8G8B8A8_UNORM, 0, (uint32_t)offsetof(Vertex2D, color),    InputClassification::PER_VERTEX_DATA },
        { "TEXCOORD", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(Vertex2D, uv),       InputClassification::PER_VERTEX_DATA },
    };
    PipelineStateDesc psd;
    psd.vs  = &vs_;
    psd.ps  = &ps_;
    psd.il  = &layout_;
    psd.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHDISABLED);
    psd.rs  = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
    psd.bs  = wi::renderer::GetBlendState(wi::enums::BSTYPE_PREMULTIPLIED);
    psd.pt  = PrimitiveTopology::TRIANGLELIST;
    GraphicsDevice* device = GetDevice();
    device->CreatePipelineState(&psd, &pso_);

    SamplerDesc samp;
    samp.address_u = samp.address_v = samp.address_w = TextureAddressMode::CLAMP;
    samp.filter = Filter::MIN_MAG_MIP_LINEAR;
    device->CreateSampler(&samp, &sampler_);

    const uint32_t white = 0xFFFFFFFFu;
    TextureDesc td;
    td.width = td.height = 1;
    td.format = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::SHADER_RESOURCE;
    SubresourceData sd;
    sd.data_ptr = &white;
    sd.row_pitch = 4;
    device->CreateTexture(&td, &sd, &white_);
    return pso_.IsValid();
}

GeometryRef Draw2DImpl::CompileGeometry (const Vertex2D* vertices, size_t vertexCount,
                                         const int* indices, size_t indexCount) {
    if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0) return nullptr;
    auto geo = std::make_shared<GeometryImpl>();
    GraphicsDevice* device = GetDevice();
    GPUBufferDesc vd;
    vd.size = vertexCount * sizeof(Vertex2D);
    vd.bind_flags = BindFlag::VERTEX_BUFFER;
    device->CreateBuffer(&vd, vertices, &geo->vb);
    GPUBufferDesc id;
    id.size = indexCount * sizeof(int);
    id.bind_flags = BindFlag::INDEX_BUFFER;
    device->CreateBuffer(&id, indices, &geo->ib);
    geo->count = (uint32_t)indexCount;
    return geo;
}

// ----------------------------------------------------------------------- drawing ---

bool Draw2DImpl::Begin (const TextureObject& target, bool clear, const char* label) {
    if (open_) return false;
    const wi::graphics::Texture& rt = Native(target);
    if (!rt.IsValid() || !has_flag(rt.desc.bind_flags, BindFlag::RENDER_TARGET)) return false;
    GraphicsDevice* device = GetDevice();
    cmd_ = device->BeginCommandList();
    labelled_ = label != nullptr;
    if (labelled_) device->EventBegin(label, cmd_);
    const RenderPassImage rp[] = { RenderPassImage::RenderTarget(&rt,
        clear ? RenderPassImage::LoadOp::CLEAR : RenderPassImage::LoadOp::LOAD,
        RenderPassImage::StoreOp::STORE, rt.desc.layout, rt.desc.layout) };
    device->RenderPassBegin(rp, 1, cmd_);
    size_ = XMUINT2(rt.desc.width, rt.desc.height);
    Viewport vp;
    vp.width = (float)size_.x;
    vp.height = (float)size_.y;
    device->BindViewports(1, &vp, cmd_);
    wi::Canvas canvas;
    canvas.init(size_.x, size_.y, 96.0f);
    wi::image::SetCanvas(canvas);
    wi::font::SetCanvas(canvas);
    proj_ = XMMatrixOrthographicOffCenterLH(0, (float)size_.x, (float)size_.y, 0, -1, 1);
    open_ = true;
    geometryBound_ = false;
    SetScissor(XMFLOAT4(0, 0, (float)size_.x, (float)size_.y));
    return true;
}

void Draw2DImpl::End () {
    if (!open_) return;
    GraphicsDevice* device = GetDevice();
    device->RenderPassEnd(cmd_);
    if (labelled_) device->EventEnd(cmd_);
    open_ = false;
}

void Draw2DImpl::SetScissor (const XMFLOAT4& clip) {
    if (!open_) return;
    Rect s;
    s.left = (int32_t)std::max(0.0f, std::floor(clip.x));
    s.top = (int32_t)std::max(0.0f, std::floor(clip.y));
    s.right = (int32_t)std::ceil(clip.z);
    s.bottom = (int32_t)std::ceil(clip.w);
    if (s.right <= s.left || s.bottom <= s.top) s.right = s.left, s.bottom = s.top;
    GetDevice()->BindScissorRects(1, &s, cmd_);
}

void Draw2DImpl::DrawImage (const ImageDraw& d) {
    if (!open_) return;
    wi::image::Params p;
    p.color = d.color;
    p.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
    p.quality = wi::image::QUALITY_LINEAR;
    p.blendFlag = wi::enums::BLENDMODE_ALPHA;
    if (d.gradient) {
        p.gradient = wi::image::Params::Gradient::Linear;
        p.gradient_uv_start = d.gradientFrom;
        p.gradient_uv_end = d.gradientTo;
        p.gradient_color = d.gradientColor;
    }
    p.pivot = d.pivot;
    p.pos = XMFLOAT3(d.rect.x + d.pivot.x * d.rect.z, d.rect.y + d.pivot.y * d.rect.w, 0);
    p.siz = XMFLOAT2(d.rect.z, d.rect.w);
    p.rotation = d.rotation;
    if (d.texture != nullptr && d.useSource) p.enableDrawRect(d.source);
    if (d.cornerRadius > 0) {
        for (auto& c : p.corners_rounding) c.radius = d.cornerRadius;
        p.enableCornerRounding();
    }
    wi::image::Draw(d.texture ? &Native(*d.texture) : nullptr, p, cmd_);
    geometryBound_ = false;
}

void Draw2DImpl::DrawString (const TextDraw& t) {
    if (!open_ || t.text == nullptr || t.text[0] == 0) return;
    wi::font::Params fp;
    fp.position = XMFLOAT3(t.position.x, t.position.y, 0);
    fp.size = std::max(1, t.size);
    fp.h_align = FontAlign(t.hAlign, true);
    fp.v_align = FontAlign(t.vAlign, false);
    fp.color = wi::Color::fromFloat4(t.color);
    fp.style = t.font;
    fp.bolden = t.bolden;
    fp.softness = t.softness;
    fp.spacingX = t.spacing.x;
    fp.spacingY = t.spacing.y;
    fp.rotation = t.rotation;
    if (t.wrapWidth > 0) fp.h_wrap = t.wrapWidth;
    if (t.shadowColor.w > 0) {
        fp.shadowColor = wi::Color::fromFloat4(t.shadowColor);
        fp.shadow_offset_x = t.shadowOffset.x;
        fp.shadow_offset_y = t.shadowOffset.y;
    } else {
        fp.shadowColor = wi::Color(0, 0, 0, 0);
    }
    wi::font::Draw(t.text, fp, cmd_);
    geometryBound_ = false;
}

void Draw2DImpl::DrawGeometry (const GeometryObject& geometry, const TextureObject* texture,
                               const XMFLOAT4X4& toTarget) {
    if (!open_ || !GeometryReady()) return;
    const GeometryImpl& geo = static_cast<const GeometryImpl&>(geometry);
    GraphicsDevice* device = GetDevice();
    if (!geometryBound_) {
        Viewport vp;
        vp.width = (float)size_.x;
        vp.height = (float)size_.y;
        device->BindViewports(1, &vp, cmd_);
        device->BindPipelineState(&pso_, cmd_);
        device->BindSampler(&sampler_, 0, cmd_);
        geometryBound_ = true;
    }
    // Row-vector order: vertex -> the caller's transform to target pixels -> projection.
    //	RmlUi's matrices are column-major for column vectors, which is the same memory as a
    //	row-major row-vector matrix (see StUiVS.hlsl).
    GeometryConstants c;
    XMStoreFloat4x4(&c.transform, XMLoadFloat4x4(&toTarget) * proj_);
    c.flags = XMFLOAT4(texture && !texture->premultiplied ? 1.0f : 0.0f, 0, 0, 0);
    device->BindDynamicConstantBuffer(c, 0, cmd_);

    const GPUBuffer* vbs[] = { &geo.vb };
    const uint32_t strides[] = { sizeof(Vertex2D) };
    const uint64_t offsets[] = { 0 };
    device->BindVertexBuffers(vbs, 0, 1, strides, offsets, cmd_);
    device->BindIndexBuffer(&geo.ib, IndexBufferFormat::UINT32, 0, cmd_);
    device->BindResource(texture ? &Native(*texture) : &white_, 0, cmd_);
    device->DrawIndexed(geo.count, 0, 0, cmd_);
}

void Draw2DImpl::Shutdown () {
    pso_ = {};
    vs_ = {};
    ps_ = {};
    sampler_ = {};
    white_ = {};
    tried_ = ready_ = false;
}

} // namespace st::render::detroit
