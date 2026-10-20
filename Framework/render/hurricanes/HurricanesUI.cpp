#if defined(SIMTARY_HAS_HURRICANES)

#include "render/hurricanes/HurricanesUI.h"

#include "wiHelper.h"
#include "wiBacklog.h"
#include "Utility/stb_image.h"
#include "Utility/stb_truetype.h"
#include "Utility/liberation_sans.h"

#include "imgui.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fl = filament;

namespace st::render::hurricanes {

namespace {

// A texture the overlay hands out. Owned ones are destroyed with the handle - unless the
//	engine already went (alive == false), when there is nothing left to destroy them on.
class HTexture final : public TextureObject {
public:
    fl::Texture* texture = nullptr;
    bool owned = false;
    fl::Engine* engine = nullptr;
    std::shared_ptr<bool> alive;
    std::unordered_set<const void*>* live = nullptr;
    ~HTexture () override {
        if (owned && texture && alive && *alive) {
            live->erase(texture);
            engine->destroy(texture);
        }
    }
};

class HGeometry final : public GeometryObject {
public:
    std::vector<Vertex2D> vertices;
    std::vector<int> indices;
};

uint32_t PackColor (const XMFLOAT4& c) {
    auto b = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return b(c.x) | (b(c.y) << 8) | (b(c.z) << 16) | (b(c.w) << 24);
}

XMFLOAT4 Lerp (const XMFLOAT4& a, const XMFLOAT4& b, float t) {
    return XMFLOAT4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// UTF-8 -> code points (invalid bytes become '?').
uint32_t NextCodepoint (const char*& s) {
    const unsigned char c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return '?';
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0) {
        if ((*s & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    return cp;
}

constexpr int kFirstChar = 32;
constexpr int kCharCount = 224;   // Latin-1: 32..255

} // namespace

struct Overlay::Font {
    std::vector<uint8_t> ttf;
    stbtt_fontinfo info;
};

struct Overlay::Atlas {
    fl::Texture* texture = nullptr;
    int width = 0, height = 0;
    stbtt_packedchar chars[kCharCount];
    float ascent = 0;   // pixels above the baseline
};

// --------------------------------------------------------------------- lifetime ---

Overlay::Overlay (fl::Engine& engine) : engine_(engine), alive_(std::make_shared<bool>(true)) {
    wi::vector<uint8_t> data;
    const std::string path = wi::helper::GetCurrentPath() + "/shaders/filament/StUi.filamat";
    if (!wi::helper::FileRead(path, data)) {
        wi::backlog::post("[hurricanes] " + path + " missing - no UI or DevUI", wi::backlog::LogLevel::Error);
        return;
    }
    material_ = fl::Material::Builder().package(data.data(), data.size()).build(engine_);

    view_ = engine_.createView();
    scene_ = engine_.createScene();
    cameraEntity_ = utils::EntityManager::get().create();
    camera_ = engine_.createCamera(cameraEntity_);
    view_->setScene(scene_);
    view_->setCamera(camera_);
    view_->setPostProcessingEnabled(false);
    view_->setShadowingEnabled(false);
    view_->setBlendMode(fl::View::BlendMode::TRANSLUCENT);

    const uint32_t px = 0xFFFFFFFFu;
    white_ = Upload(&px, 1, 1);

    // Font 0: the engine's own Liberation Sans, the same face RmlUi falls back to.
    auto font = std::make_unique<Font>();
    wi::vector<uint8_t> ttf;
    wi::helper::Decompress(liberation_sans_zstd, sizeof(liberation_sans_zstd), ttf);
    font->ttf.assign(ttf.begin(), ttf.end());
    stbtt_InitFont(&font->info, font->ttf.data(), stbtt_GetFontOffsetForIndex(font->ttf.data(), 0));
    fonts_.push_back(std::move(font));

    // ImGui's font atlas, on this renderer (Detroit made its own on the null device).
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        imguiFont_ = Upload(pixels, (uint32_t)w, (uint32_t)h);
        io.Fonts->SetTexID((ImTextureID)imguiFont_);
    }
}

Overlay::~Overlay () { Destroy(); }

void Overlay::Destroy () {
    if (alive_ && !*alive_) return;
    *alive_ = false;
    for (auto& kv : atlases_) if (kv.second->texture) engine_.destroy(kv.second->texture);
    atlases_.clear();
    if (renderable_) {
        scene_->remove(renderable_);
        engine_.destroy(renderable_);
        utils::EntityManager::get().destroy(renderable_);
    }
    for (fl::MaterialInstance* mi : pool_) engine_.destroy(mi);
    pool_.clear();
    if (vb_) engine_.destroy(vb_);
    if (ib_) engine_.destroy(ib_);
    if (white_) engine_.destroy(white_);
    if (imguiFont_) engine_.destroy(imguiFont_);
    if (camera_) {
        engine_.destroyCameraComponent(cameraEntity_);
        utils::EntityManager::get().destroy(cameraEntity_);
    }
    if (view_) engine_.destroy(view_);
    if (scene_) engine_.destroy(scene_);
    if (material_) engine_.destroy(material_);
    material_ = nullptr;
}

// -------------------------------------------------------------------- resources ---

fl::Texture* Overlay::Upload (const void* rgba8, uint32_t w, uint32_t h) {
    // RGBA8, not sRGB: the overlay works in display values end to end (no post-processing).
    fl::Texture* t = fl::Texture::Builder().width(w).height(h).levels(1)
        .format(fl::Texture::InternalFormat::RGBA8).sampler(fl::Texture::Sampler::SAMPLER_2D).build(engine_);
    const size_t bytes = size_t(w) * h * 4;
    uint8_t* copy = new uint8_t[bytes];
    std::memcpy(copy, rgba8, bytes);
    t->setImage(engine_, 0, fl::Texture::PixelBufferDescriptor(copy, bytes, fl::Texture::Format::RGBA,
        fl::Texture::Type::UBYTE, [](void* buffer, size_t, void*) { delete[] (uint8_t*)buffer; }));
    live_.insert(t);
    return t;
}

TextureRef Overlay::Wrap (fl::Texture* texture, uint32_t w, uint32_t h, bool premultiplied, bool owned) {
    if (texture == nullptr) return nullptr;
    auto t = std::make_shared<HTexture>();
    t->texture = texture;
    t->owned = owned;
    t->engine = &engine_;
    t->alive = alive_;
    t->live = &live_;
    t->width = w;
    t->height = h;
    t->premultiplied = premultiplied;
    return t;
}

TextureRef Overlay::LoadTexture (const std::string& path) {
    wi::vector<uint8_t> file;
    if (path.empty() || !wi::helper::FileRead(path, file)) return nullptr;
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
    if (pixels == nullptr) return nullptr;
    fl::Texture* t = Upload(pixels, (uint32_t)w, (uint32_t)h);
    stbi_image_free(pixels);
    return Wrap(t, (uint32_t)w, (uint32_t)h, false, true);
}

TextureRef Overlay::FromResource (const wi::Resource& resource) {
    // Wicked's device is the null one here: a resource only has pixels if it kept its file.
    if (!resource.IsValid() || resource.GetFileData().empty()) return nullptr;
    const auto& file = resource.GetFileData();
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
    if (pixels == nullptr) return nullptr;
    fl::Texture* t = Upload(pixels, (uint32_t)w, (uint32_t)h);
    stbi_image_free(pixels);
    return Wrap(t, (uint32_t)w, (uint32_t)h, false, true);
}

TextureRef Overlay::CreateTexture (const void* rgba8, uint32_t width, uint32_t height, bool premultiplied) {
    if (rgba8 == nullptr || width == 0 || height == 0) return nullptr;
    return Wrap(Upload(rgba8, width, height), width, height, premultiplied, true);
}

int Overlay::LoadFont (const std::string& path) {
    wi::vector<uint8_t> file;
    if (path.empty() || !wi::helper::FileRead(path, file)) return 0;
    auto font = std::make_unique<Font>();
    font->ttf.assign(file.begin(), file.end());
    const int offset = stbtt_GetFontOffsetForIndex(font->ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, font->ttf.data(), offset)) return 0;
    fonts_.push_back(std::move(font));
    return (int)fonts_.size() - 1;
}

GeometryRef Overlay::CompileGeometry (const Vertex2D* vertices, size_t vertexCount, const int* indices, size_t indexCount) {
    if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0) return nullptr;
    auto g = std::make_shared<HGeometry>();
    g->vertices.assign(vertices, vertices + vertexCount);
    g->indices.assign(indices, indices + indexCount);
    return g;
}

void* Overlay::ImGuiTexture (const TextureObject& texture) const {
    return static_cast<const HTexture&>(texture).texture;
}

void* Overlay::ImGuiTexture (const wi::Resource&) {
    return nullptr;   // a Wicked resource has no pixels on this renderer (see FromResource)
}

Overlay::Atlas* Overlay::AtlasFor (int font, int pixelSize) {
    if (font < 0 || font >= (int)fonts_.size()) font = 0;
    pixelSize = std::clamp(pixelSize, 4, 256);
    const uint64_t key = (uint64_t(font) << 32) | uint32_t(pixelSize);
    auto it = atlases_.find(key);
    if (it != atlases_.end()) return it->second.get();

    auto atlas = std::make_unique<Atlas>();
    const int side = pixelSize <= 32 ? 512 : pixelSize <= 72 ? 1024 : 2048;
    atlas->width = atlas->height = side;
    std::vector<uint8_t> alpha(size_t(side) * side);
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, alpha.data(), side, side, 0, 1, nullptr);
    stbtt_PackSetOversampling(&pc, 1, 1);
    stbtt_PackFontRange(&pc, fonts_[font]->ttf.data(), 0, (float)pixelSize, kFirstChar, kCharCount, atlas->chars);
    stbtt_PackEnd(&pc);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&fonts_[font]->info, &ascent, &descent, &gap);
    atlas->ascent = ascent * stbtt_ScaleForPixelHeight(&fonts_[font]->info, (float)pixelSize);

    // White glyphs with coverage in alpha (straight alpha, like ImGui's atlas).
    std::vector<uint32_t> rgba(alpha.size());
    for (size_t i = 0; i < alpha.size(); ++i) rgba[i] = 0x00FFFFFFu | (uint32_t(alpha[i]) << 24);
    atlas->texture = Upload(rgba.data(), (uint32_t)side, (uint32_t)side);
    return (atlases_[key] = std::move(atlas)).get();
}

// ---------------------------------------------------------------------- drawing ---

void Overlay::SetScreenSize (uint32_t width, uint32_t height) {
    size_ = XMUINT2(width, height);
    screen_.width = width;
    screen_.height = height;
}

bool Overlay::Begin (const TextureObject& target, bool, const char*) {
    if (!Ready() || &target != &screen_ || open_) return false;
    open_ = true;
    scissor_ = XMFLOAT4(0, 0, (float)size_.x, (float)size_.y);
    return true;
}

void Overlay::SetScissor (const XMFLOAT4& ltrb) { scissor_ = ltrb; }

void Overlay::Use (fl::Texture* texture, bool texPremul, bool colorPremul) {
    const int32_t x0 = (int32_t)std::max(0.0f, std::floor(scissor_.x));
    const int32_t y0 = (int32_t)std::max(0.0f, std::floor(scissor_.y));
    const int32_t x1 = std::min((int32_t)size_.x, (int32_t)std::ceil(scissor_.z));
    const int32_t y1 = std::min((int32_t)size_.y, (int32_t)std::ceil(scissor_.w));
    Use(texture, texPremul, colorPremul, x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

void Overlay::Use (fl::Texture* texture, bool texPremul, bool colorPremul, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
    if (texture == nullptr) texture = white_;
    if (!cmds_.empty()) {
        const Cmd& c = cmds_.back();
        if (c.texture == texture && c.texPremul == texPremul && c.colorPremul == colorPremul &&
            c.sx == sx && c.sy == sy && c.sw == sw && c.sh == sh) return;
    }
    cmds_.push_back({ texture, texPremul, colorPremul, sx, sy, sw, sh, (uint32_t)indices_.size(), 0 });
}

void Overlay::DrawImage (const ImageDraw& d) {
    if (!open_ || d.rect.z <= 0 || d.rect.w <= 0) return;
    const HTexture* tex = d.texture ? static_cast<const HTexture*>(d.texture) : nullptr;
    Use(tex ? tex->texture : white_, tex ? tex->premultiplied : false, false);

    // UVs: the source rect (texels) or the whole texture.
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (tex && d.useSource && tex->width > 0 && tex->height > 0) {
        u0 = d.source.x / tex->width;
        v0 = d.source.y / tex->height;
        u1 = (d.source.x + d.source.z) / tex->width;
        v1 = (d.source.y + d.source.w) / tex->height;
    }
    const float cx = d.rect.x + d.pivot.x * d.rect.z, cy = d.rect.y + d.pivot.y * d.rect.w;
    const float cs = std::cos(d.rotation), sn = std::sin(d.rotation);
    const XMFLOAT2 g0 = d.gradientFrom, g1 = d.gradientTo;
    const float gx = g1.x - g0.x, gy = g1.y - g0.y, gl = gx * gx + gy * gy;
    auto vertex = [&](float lx, float ly) {   // lx, ly: 0..1 across the rect
        float x = d.rect.x + lx * d.rect.z, y = d.rect.y + ly * d.rect.w;
        if (d.rotation != 0) {
            const float rx = x - cx, ry = y - cy;
            x = cx + rx * cs - ry * sn;
            y = cy + rx * sn + ry * cs;
        }
        XMFLOAT4 color = d.color;
        if (d.gradient && gl > 0)
            color = Lerp(d.color, d.gradientColor, std::clamp(((lx - g0.x) * gx + (ly - g0.y) * gy) / gl, 0.0f, 1.0f));
        vertices_.push_back({ x, y, u0 + (u1 - u0) * lx, v0 + (v1 - v0) * ly, PackColor(color) });
    };

    const uint32_t base = (uint32_t)vertices_.size();
    if (d.cornerRadius <= 0) {
        vertex(0, 0); vertex(1, 0); vertex(1, 1); vertex(0, 1);
        indices_.insert(indices_.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    } else {
        // A fan from the centre around the rounded outline, 8 segments a corner.
        const float rx = std::min(d.cornerRadius, d.rect.z * 0.5f) / d.rect.z;
        const float ry = std::min(d.cornerRadius, d.rect.w * 0.5f) / d.rect.w;
        vertex(0.5f, 0.5f);
        const float corners[4][3] = { { 1 - rx, 1 - ry, 0 }, { rx, 1 - ry, 0.5f }, { rx, ry, 1 }, { 1 - rx, ry, 1.5f } };
        constexpr int seg = 8;
        for (const auto& c : corners)
            for (int i = 0; i <= seg; ++i) {
                const float a = (c[2] + 0.5f * i / seg) * XM_PI;
                vertex(c[0] + std::cos(a) * rx, c[1] + std::sin(a) * ry);
            }
        const uint32_t ring = (uint32_t)vertices_.size() - base - 1;
        for (uint32_t i = 0; i < ring; ++i)
            indices_.insert(indices_.end(), { base, base + 1 + i, base + 1 + (i + 1) % ring });
    }
    cmds_.back().count = (uint32_t)indices_.size() - cmds_.back().first;
}

void Overlay::DrawString (const TextDraw& t) {
    if (!open_ || t.text == nullptr || t.text[0] == 0) return;
    Atlas* atlas = AtlasFor(t.font, t.size);
    if (atlas == nullptr) return;
    const float lineHeight = (float)t.size + t.spacing.y;

    // Lines (explicit breaks, then greedy word wrap), each with its width.
    struct Line { std::vector<uint32_t> cps; float width = 0; };
    std::vector<Line> lines(1);
    auto advance = [&](uint32_t cp) {
        const int i = (cp >= (uint32_t)kFirstChar && cp < uint32_t(kFirstChar + kCharCount) ? (int)cp : '?') - kFirstChar;
        return atlas->chars[i].xadvance + t.spacing.x;
    };
    std::vector<uint32_t> word;
    float wordWidth = 0;
    auto flushWord = [&]() {
        Line& line = lines.back();
        if (t.wrapWidth > 0 && !line.cps.empty() && line.width + wordWidth > t.wrapWidth) {
            while (!line.cps.empty() && line.cps.back() == ' ') { line.width -= advance(' '); line.cps.pop_back(); }
            lines.emplace_back();
        }
        lines.back().cps.insert(lines.back().cps.end(), word.begin(), word.end());
        lines.back().width += wordWidth;
        word.clear();
        wordWidth = 0;
    };
    for (const char* s = t.text; *s;) {
        const uint32_t cp = NextCodepoint(s);
        if (cp == '\n') { flushWord(); lines.emplace_back(); continue; }
        word.push_back(cp);
        wordWidth += advance(cp);
        if (cp == ' ') flushWord();
    }
    flushWord();

    const float blockHeight = lineHeight * lines.size();
    const float top = t.vAlign == Align::Start ? 0 : t.vAlign == Align::End ? -blockHeight : -blockHeight * 0.5f;
    const float cs = std::cos(t.rotation), sn = std::sin(t.rotation);

    auto emit = [&](const XMFLOAT2& offset, const XMFLOAT4& color) {
        Use(atlas->texture, false, false);
        const uint32_t packed = PackColor(color);
        for (size_t li = 0; li < lines.size(); ++li) {
            const Line& line = lines[li];
            float x = t.hAlign == Align::Start ? 0 : t.hAlign == Align::End ? -line.width : -line.width * 0.5f;
            float y = top + li * lineHeight + atlas->ascent;
            for (uint32_t cp : line.cps) {
                const int i = (cp >= (uint32_t)kFirstChar && cp < uint32_t(kFirstChar + kCharCount) ? (int)cp : '?') - kFirstChar;
                stbtt_aligned_quad q;
                stbtt_GetPackedQuad(atlas->chars, atlas->width, atlas->height, i, &x, &y, &q, 0);
                x += t.spacing.x;
                if (q.x1 <= q.x0) continue;
                const uint32_t base = (uint32_t)vertices_.size();
                auto put = [&](float px, float py, float u, float v) {
                    px += offset.x;
                    py += offset.y;
                    vertices_.push_back({ t.position.x + px * cs - py * sn, t.position.y + px * sn + py * cs, u, v, packed });
                };
                put(q.x0, q.y0, q.s0, q.t0);
                put(q.x1, q.y0, q.s1, q.t0);
                put(q.x1, q.y1, q.s1, q.t1);
                put(q.x0, q.y1, q.s0, q.t1);
                indices_.insert(indices_.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            }
        }
        cmds_.back().count = (uint32_t)indices_.size() - cmds_.back().first;
    };
    if (t.shadowColor.w > 0) emit(t.shadowOffset, t.shadowColor);
    emit(XMFLOAT2(0, 0), t.color);
}

void Overlay::DrawGeometry (const GeometryObject& geometry, const TextureObject* texture, const XMFLOAT4X4& m) {
    if (!open_) return;
    const HGeometry& g = static_cast<const HGeometry&>(geometry);
    const HTexture* tex = texture ? static_cast<const HTexture*>(texture) : nullptr;
    Use(tex ? tex->texture : white_, tex ? tex->premultiplied : false, true);
    const uint32_t base = (uint32_t)vertices_.size();
    for (const Vertex2D& v : g.vertices) {
        // Row vector (x, y, 0, 1) through the caller's matrix; RmlUi transforms are affine in 2D.
        const float x = v.position.x * m._11 + v.position.y * m._21 + m._41;
        const float y = v.position.x * m._12 + v.position.y * m._22 + m._42;
        vertices_.push_back({ x, y, v.uv.x, v.uv.y, v.color });
    }
    for (int i : g.indices) indices_.push_back(base + (uint32_t)i);
    cmds_.back().count = (uint32_t)indices_.size() - cmds_.back().first;
}

void Overlay::AddImGui (const ImDrawData* data) {
    if (!Ready() || data == nullptr || data->TotalVtxCount == 0) return;
    const ImVec2 off = data->DisplayPos, scale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; ++n) {
        const ImDrawList* list = data->CmdLists[n];
        const uint32_t base = (uint32_t)vertices_.size();
        for (const ImDrawVert& v : list->VtxBuffer)
            vertices_.push_back({ (v.pos.x - off.x) * scale.x, (v.pos.y - off.y) * scale.y, v.uv.x, v.uv.y, v.col });
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback || c.ElemCount == 0) continue;
            // Only textures this renderer made are drawable: an editor viewport's Wicked
            //	texture (null device) is skipped rather than read as garbage.
            fl::Texture* tex = (fl::Texture*)c.GetTexID();
            if (live_.count(tex) == 0) continue;
            const int32_t x0 = (int32_t)std::max(0.0f, std::floor((c.ClipRect.x - off.x) * scale.x));
            const int32_t y0 = (int32_t)std::max(0.0f, std::floor((c.ClipRect.y - off.y) * scale.y));
            const int32_t x1 = std::min((int32_t)size_.x, (int32_t)std::ceil((c.ClipRect.z - off.x) * scale.x));
            const int32_t y1 = std::min((int32_t)size_.y, (int32_t)std::ceil((c.ClipRect.w - off.y) * scale.y));
            if (x1 <= x0 || y1 <= y0) continue;
            cmds_.push_back({ tex, false, false, x0, y0, x1 - x0, y1 - y0, (uint32_t)indices_.size(), c.ElemCount });
            for (unsigned i = 0; i < c.ElemCount; ++i)
                indices_.push_back(base + c.VtxOffset + list->IdxBuffer[c.IdxOffset + i]);
        }
    }
}

void Overlay::Render (fl::Renderer& renderer) {
    cmds_.erase(std::remove_if(cmds_.begin(), cmds_.end(), [](const Cmd& c) { return c.count == 0 || c.sw == 0 || c.sh == 0; }),
                cmds_.end());
    if (!Ready() || cmds_.empty() || size_.x == 0 || size_.y == 0) {
        vertices_.clear(); indices_.clear(); cmds_.clear();
        return;
    }
    if (cmds_.size() > 32767) cmds_.resize(32767);   // blend order is 15 bits

    // Buffers grow (powers of two) and are refilled every frame.
    auto grow = [](uint32_t need) { uint32_t c = 1024; while (c < need) c <<= 1; return c; };
    if (vertices_.size() > vbCapacity_) {
        if (vb_) engine_.destroy(vb_);
        vbCapacity_ = grow((uint32_t)vertices_.size());
        vb_ = fl::VertexBuffer::Builder().vertexCount(vbCapacity_).bufferCount(1)
            .attribute(fl::VertexAttribute::POSITION, 0, fl::VertexBuffer::AttributeType::FLOAT2, offsetof(Vtx, x), sizeof(Vtx))
            .attribute(fl::VertexAttribute::UV0, 0, fl::VertexBuffer::AttributeType::FLOAT2, offsetof(Vtx, u), sizeof(Vtx))
            .attribute(fl::VertexAttribute::COLOR, 0, fl::VertexBuffer::AttributeType::UBYTE4, offsetof(Vtx, color), sizeof(Vtx))
            .normalized(fl::VertexAttribute::COLOR)
            .build(engine_);
    }
    if (indices_.size() > ibCapacity_) {
        if (ib_) engine_.destroy(ib_);
        ibCapacity_ = grow((uint32_t)indices_.size());
        ib_ = fl::IndexBuffer::Builder().indexCount(ibCapacity_).bufferType(fl::IndexBuffer::IndexType::UINT).build(engine_);
    }
    Vtx* vcopy = new Vtx[vertices_.size()];
    std::memcpy(vcopy, vertices_.data(), vertices_.size() * sizeof(Vtx));
    vb_->setBufferAt(engine_, 0, fl::VertexBuffer::BufferDescriptor(vcopy, vertices_.size() * sizeof(Vtx),
        [](void* b, size_t, void*) { delete[] (Vtx*)b; }));
    uint32_t* icopy = new uint32_t[indices_.size()];
    std::memcpy(icopy, indices_.data(), indices_.size() * sizeof(uint32_t));
    ib_->setBuffer(engine_, fl::IndexBuffer::BufferDescriptor(icopy, indices_.size() * sizeof(uint32_t),
        [](void* b, size_t, void*) { delete[] (uint32_t*)b; }));

    // One renderable, one primitive per command, drawn in command order.
    if (renderable_) {
        scene_->remove(renderable_);
        engine_.destroy(renderable_);
    } else {
        renderable_ = utils::EntityManager::get().create();
    }
    while (pool_.size() < cmds_.size()) pool_.push_back(material_->createInstance());
    const fl::TextureSampler sampler(fl::TextureSampler::MinFilter::LINEAR, fl::TextureSampler::MagFilter::LINEAR,
                                     fl::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    fl::RenderableManager::Builder b(cmds_.size());
    b.boundingBox({ { -1e5f, -1e5f, -1e5f }, { 1e5f, 1e5f, 1e5f } }).culling(false).castShadows(false).receiveShadows(false);
    for (size_t i = 0; i < cmds_.size(); ++i) {
        const Cmd& c = cmds_[i];
        fl::MaterialInstance* mi = pool_[i];
        mi->setParameter("albedo", c.texture, sampler);
        mi->setParameter("texPremul", c.texPremul ? 1.0f : 0.0f);
        mi->setParameter("colorPremul", c.colorPremul ? 1.0f : 0.0f);
        // Filament's scissor origin is the bottom left.
        mi->setScissor((uint32_t)c.sx, (uint32_t)std::max(0, (int32_t)size_.y - (c.sy + c.sh)), (uint32_t)c.sw, (uint32_t)c.sh);
        b.geometry(i, fl::RenderableManager::PrimitiveType::TRIANGLES, vb_, ib_, c.first, c.count);
        b.material(i, mi);
        b.blendOrder(i, (uint16_t)i);
    }
    b.build(engine_, renderable_);
    scene_->addEntity(renderable_);

    view_->setViewport({ 0, 0, size_.x, size_.y });
    camera_->setProjection(fl::Camera::Projection::ORTHO, 0.0, size_.x, size_.y, 0.0, -1.0, 1.0);
    renderer.render(view_);

    vertices_.clear();
    indices_.clear();
    cmds_.clear();
}

} // namespace st::render::hurricanes

#endif // SIMTARY_HAS_HURRICANES
