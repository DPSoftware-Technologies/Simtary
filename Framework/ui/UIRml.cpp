#include "ui/UIRml.h"
#include "ui/UIComponents.h"

#include "wiResourceManager.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiTimer.h"
#include "Utility/liberation_sans.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

using wi::ecs::Entity;

namespace st::ui {

namespace {

// ----------------------------------------------------------------------- interfaces ---

class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime () override { return timer_.elapsed_seconds(); }
    bool LogMessage (Rml::Log::Type type, const Rml::String& message) override {
        wi::backlog::LogLevel level = wi::backlog::LogLevel::Default;
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) level = wi::backlog::LogLevel::Error;
        else if (type == Rml::Log::LT_WARNING) level = wi::backlog::LogLevel::Warning;
        else if (type == Rml::Log::LT_DEBUG) return true;
        wi::backlog::post("[RmlUi] " + message, level);
        return true;
    }
    void SetClipboardText (const Rml::String& text) override { SDL_SetClipboardText(text.c_str()); }
    void GetClipboardText (Rml::String& text) override {
        if (char* t = SDL_GetClipboardText()) {
            text = t;
            SDL_free(t);
        }
    }

private:
    wi::Timer timer_;
};

// Whole-file reads through the engine, so a page inside a mounted package opens by the
// same logical path as a loose one.
class FileInterface final : public Rml::FileInterface {
public:
    Rml::FileHandle Open (const Rml::String& path) override {
        auto file = std::make_unique<File>();
        if (!wi::helper::FileRead(path, file->data)) return 0;
        return (Rml::FileHandle)file.release();
    }
    void Close (Rml::FileHandle handle) override { delete (File*)handle; }
    size_t Read (void* buffer, size_t size, Rml::FileHandle handle) override {
        File* f = (File*)handle;
        const size_t n = std::min(size, f->data.size() - f->pos);
        std::memcpy(buffer, f->data.data() + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek (Rml::FileHandle handle, long offset, int origin) override {
        File* f = (File*)handle;
        long base = origin == SEEK_CUR ? (long)f->pos : origin == SEEK_END ? (long)f->data.size() : 0;
        const long to = base + offset;
        if (to < 0 || to > (long)f->data.size()) return false;
        f->pos = (size_t)to;
        return true;
    }
    size_t Tell (Rml::FileHandle handle) override { return ((File*)handle)->pos; }
    size_t Length (Rml::FileHandle handle) override { return ((File*)handle)->data.size(); }

private:
    struct File {
        wi::vector<uint8_t> data;
        size_t pos = 0;
    };
};

// RmlUi's renderer is render::Draw2D: compiled triangle lists, textures, a scissor and a
//	transform - nothing here touches the GPU itself.
static_assert(sizeof(Rml::Vertex) == sizeof(render::Vertex2D), "Rml::Vertex and render::Vertex2D must match");

class RenderInterface final : public Rml::RenderInterface {
public:
    bool Create () { return render::Installed() && render::Get().UI().GeometryReady(); }

    // What the draws of one document need, set by RmlHost::Render around ctx->Render().
    void Begin (render::Draw2D& draw, XMFLOAT2 offset, XMFLOAT4 clip) {
        draw_ = &draw;
        offset_ = offset;
        clip_ = clip;
        scissorOn_ = false;
        hasTransform_ = false;
    }

    Rml::CompiledGeometryHandle CompileGeometry (Rml::Span<const Rml::Vertex> vertices,
                                                 Rml::Span<const int> indices) override {
        if (!render::Installed()) return 0;
        render::GeometryRef geo = render::Get().UI().CompileGeometry(
            reinterpret_cast<const render::Vertex2D*>(vertices.data()), vertices.size(), indices.data(), indices.size());
        return geo ? (Rml::CompiledGeometryHandle) new render::GeometryRef(std::move(geo)) : 0;
    }

    void RenderGeometry (Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                         Rml::TextureHandle texture) override {
        const render::GeometryRef* geo = (const render::GeometryRef*)handle;
        if (geo == nullptr || draw_ == nullptr) return;

        // The scissor: the element's clip, cut further by RmlUi's own region.
        XMFLOAT4 s = clip_;
        if (scissorOn_) {
            s.x = std::max(s.x, offset_.x + (float)scissor_.Left());
            s.y = std::max(s.y, offset_.y + (float)scissor_.Top());
            s.z = std::min(s.z, offset_.x + (float)scissor_.Right());
            s.w = std::min(s.w, offset_.y + (float)scissor_.Bottom());
        }
        if (s.z <= s.x || s.w <= s.y) return;
        draw_->SetScissor(s);

        // Row-vector order: vertex -> translation -> document transform -> its offset on the target.
        XMMATRIX m = XMMatrixTranslation(translation.x, translation.y, 0);
        if (hasTransform_) m = m * transform_;
        m = m * XMMatrixTranslation(offset_.x, offset_.y, 0);
        XMFLOAT4X4 toTarget;
        XMStoreFloat4x4(&toTarget, m);
        const render::TextureRef* tex = (const render::TextureRef*)texture;
        draw_->DrawGeometry(**geo, tex ? tex->get() : nullptr, toTarget);
    }

    // The GPU may still be reading a buffer released this frame; the renderer defers the
    //	actual destruction until it is not.
    void ReleaseGeometry (Rml::CompiledGeometryHandle handle) override { delete (render::GeometryRef*)handle; }

    Rml::TextureHandle LoadTexture (Rml::Vector2i& dimensions, const Rml::String& source) override {
        if (!render::Installed()) return 0;
        render::TextureRef tex = render::Get().UI().LoadTexture(source);
        if (!tex) return 0;
        dimensions = Rml::Vector2i((int)tex->width, (int)tex->height);
        return (Rml::TextureHandle) new render::TextureRef(std::move(tex));
    }

    Rml::TextureHandle GenerateTexture (Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        if (!render::Installed()) return 0;
        render::TextureRef tex = render::Get().UI().CreateTexture(source.data(), (uint32_t)dimensions.x,
                                                                  (uint32_t)dimensions.y, true);
        return tex ? (Rml::TextureHandle) new render::TextureRef(std::move(tex)) : 0;
    }

    void ReleaseTexture (Rml::TextureHandle handle) override { delete (render::TextureRef*)handle; }

    void EnableScissorRegion (bool enable) override { scissorOn_ = enable; }
    void SetScissorRegion (Rml::Rectanglei region) override { scissor_ = region; }

    void SetTransform (const Rml::Matrix4f* transform) override {
        hasTransform_ = transform != nullptr;
        if (transform) {
            XMFLOAT4X4 m;
            std::memcpy(&m, transform->data(), sizeof(m));
            transform_ = XMLoadFloat4x4(&m);
        }
    }

    void End () { draw_ = nullptr; }

private:
    render::Draw2D* draw_ = nullptr;
    XMFLOAT2 offset_ = XMFLOAT2(0, 0);
    XMFLOAT4 clip_ = XMFLOAT4(0, 0, 0, 0);
    XMMATRIX transform_ = XMMatrixIdentity();
    bool hasTransform_ = false;
    bool scissorOn_ = false;
    Rml::Rectanglei scissor_;
};

// on<event>="handler": every such attribute becomes one of these on its element.
class Listener final : public Rml::EventListener {
public:
    explicit Listener (Rml::String handler) : handler_(std::move(handler)) {}
    void ProcessEvent (Rml::Event& event) override {
        Rml::Element* el = event.GetCurrentElement();
        if (el == nullptr) return;
        RmlHost::Get().FireEvent(el->GetOwnerDocument(), handler_, el->GetId(), event.GetType());
    }
    void OnDetach (Rml::Element*) override { delete this; }

private:
    Rml::String handler_;
};

class ListenerInstancer final : public Rml::EventListenerInstancer {
public:
    Rml::EventListener* InstanceEventListener (const Rml::String& value, Rml::Element*) override {
        return new Listener(value);
    }
};

int Modifiers () {
    const SDL_Keymod m = SDL_GetModState();
    int out = 0;
    if (m & KMOD_CTRL)  out |= Rml::Input::KM_CTRL;
    if (m & KMOD_SHIFT) out |= Rml::Input::KM_SHIFT;
    if (m & KMOD_ALT)   out |= Rml::Input::KM_ALT;
    return out;
}

Rml::Input::KeyIdentifier MapKey (SDL_Keycode k) {
    using namespace Rml::Input;
    if (k >= SDLK_a && k <= SDLK_z) return (KeyIdentifier)(KI_A + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return (KeyIdentifier)(KI_0 + (k - SDLK_0));
    switch (k) {
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_TAB:       return KI_TAB;
    case SDLK_RETURN:    return KI_RETURN;
    case SDLK_KP_ENTER:  return KI_NUMPADENTER;
    case SDLK_ESCAPE:    return KI_ESCAPE;
    case SDLK_SPACE:     return KI_SPACE;
    case SDLK_DELETE:    return KI_DELETE;
    case SDLK_LEFT:      return KI_LEFT;
    case SDLK_RIGHT:     return KI_RIGHT;
    case SDLK_UP:        return KI_UP;
    case SDLK_DOWN:      return KI_DOWN;
    case SDLK_HOME:      return KI_HOME;
    case SDLK_END:       return KI_END;
    case SDLK_PAGEUP:    return KI_PRIOR;
    case SDLK_PAGEDOWN:  return KI_NEXT;
    default:             return KI_UNKNOWN;
    }
}

} // namespace

// ---------------------------------------------------------------------------- host ---

struct RmlHost::Impl {
    SystemInterface   system;
    FileInterface     files;
    RenderInterface   render;
    ListenerInstancer listeners;
    wi::vector<uint8_t> defaultFont;           // must outlive Rml::Shutdown
    std::unordered_set<std::string> loadedFonts;

    struct Doc {
        Rml::Context*         context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string           source;
        std::string           fonts;
        std::string           status = "no source";
        XMINT2                size = XMINT2(0, 0);
        float                 dp = 1.0f;
        bool                  hovered = false;
    };
    std::unordered_map<Entity, Doc> docs;

    void LoadFonts (const std::string& list) {
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find(';', start);
            if (end == std::string::npos) end = list.size();
            std::string path = list.substr(start, end - start);
            while (!path.empty() && path.front() == ' ') path.erase(path.begin());
            while (!path.empty() && path.back() == ' ') path.pop_back();
            if (!path.empty() && loadedFonts.insert(path).second && !Rml::LoadFontFace(path))
                wi::backlog::post("[RmlUi] could not load font " + path, wi::backlog::LogLevel::Warning);
            start = end + 1;
        }
    }

    void Load (Entity e, Doc& d) {
        if (d.document) {
            d.document->Close();
            d.document = nullptr;
        }
        if (d.source.empty()) {
            d.status = "no source";
            return;
        }
        LoadFonts(d.fonts);
        d.document = d.context->LoadDocument(d.source);
        if (d.document == nullptr) {
            d.status = "failed to load (see backlog)";
            return;
        }
        d.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        d.status = "loaded";
        (void)e;
    }

    void Remove (Entity e) {
        auto it = docs.find(e);
        if (it == docs.end()) return;
        Rml::RemoveContext(it->second.context->GetName());
        docs.erase(it);
    }
};

RmlHost& RmlHost::Get () {
    static RmlHost host;
    return host;
}

bool RmlHost::Ready () {
    if (impl_) return true;
    if (failed_) return false;
    auto impl = std::make_unique<Impl>();
    if (!impl->render.Create()) {
        failed_ = true;
        return false;
    }
    Rml::SetSystemInterface(&impl->system);
    Rml::SetFileInterface(&impl->files);
    Rml::SetRenderInterface(&impl->render);
    if (!Rml::Initialise()) {
        wi::backlog::post("[RmlUi] initialisation failed", wi::backlog::LogLevel::Error);
        failed_ = true;
        return false;
    }
    Rml::Factory::RegisterEventListenerInstancer(&impl->listeners);
    // The engine's own font, so a document that names no font still has text.
    wi::helper::Decompress(liberation_sans_zstd, sizeof(liberation_sans_zstd), impl->defaultFont);
    Rml::LoadFontFace(Rml::Span<const Rml::byte>(impl->defaultFont.data(), impl->defaultFont.size()),
                      "LiberationSans", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto, true);
    impl_ = impl.release();
    wi::backlog::post("[RmlUi] " + Rml::GetVersion() + " ready");
    return true;
}

void RmlHost::Shutdown () {
    if (!impl_) return;
    for (auto& kv : impl_->docs) Rml::RemoveContext(kv.second.context->GetName());
    impl_->docs.clear();
    Rml::Shutdown();
    delete impl_;
    impl_ = nullptr;
}

void RmlHost::Sync (Entity e, const UIDocumentComponent& doc, XMINT2 size, float dpRatio) {
    if (!Ready()) return;
    size.x = std::max(size.x, 1);
    size.y = std::max(size.y, 1);
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end()) {
        Impl::Doc d;
        d.context = Rml::CreateContext("st_doc_" + std::to_string(e), Rml::Vector2i(size.x, size.y));
        if (d.context == nullptr) return;
        d.size = size;
        it = impl_->docs.emplace(e, std::move(d)).first;
        it->second.source = "\x01";   // never equal: forces the first load below
    }
    Impl::Doc& d = it->second;
    if (d.size.x != size.x || d.size.y != size.y) {
        d.size = size;
        d.context->SetDimensions(Rml::Vector2i(size.x, size.y));
    }
    if (d.dp != dpRatio) {
        d.dp = dpRatio;
        d.context->SetDensityIndependentPixelRatio(dpRatio);
    }
    if (d.source != doc.source || d.fonts != doc.fonts) {
        d.source = doc.source;
        d.fonts = doc.fonts;
        impl_->Load(e, d);
    }
}

void RmlHost::Retain (const std::unordered_set<Entity>& alive) {
    if (!impl_) return;
    std::vector<Entity> gone;
    for (auto& kv : impl_->docs)
        if (!alive.count(kv.first)) gone.push_back(kv.first);
    for (Entity e : gone) impl_->Remove(e);
}

void RmlHost::ReloadAll () {
    if (!impl_) return;
    Rml::ReleaseTextures();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    for (auto& kv : impl_->docs) impl_->Load(kv.first, kv.second);
}

bool RmlHost::Has (Entity e) const { return impl_ && impl_->docs.count(e); }

std::string RmlHost::Status (Entity e) const {
    if (failed_) return "RmlUi unavailable (see backlog)";
    if (!impl_) return "not started";
    auto it = impl_->docs.find(e);
    return it == impl_->docs.end() ? "not created" : it->second.status;
}

void RmlHost::Update () {
    if (!impl_) return;
    for (auto& kv : impl_->docs) kv.second.context->Update();
}

void RmlHost::Render (Entity e, render::Draw2D& draw, XMFLOAT2 offset, XMFLOAT4 clip) {
    if (!impl_) return;
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end() || it->second.document == nullptr) return;
    impl_->render.Begin(draw, offset, clip);
    it->second.context->Render();
    impl_->render.End();
}

bool RmlHost::PointerMove (Entity e, XMFLOAT2 local) {
    if (!impl_) return false;
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end()) return false;
    it->second.hovered = true;
    it->second.context->ProcessMouseMove((int)local.x, (int)local.y, Modifiers());
    return it->second.context->IsMouseInteracting();
}

bool RmlHost::PointerButton (Entity e, bool down) {
    if (!impl_) return false;
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end()) return false;
    Rml::Context* ctx = it->second.context;
    const bool interacting = ctx->IsMouseInteracting();
    if (down) ctx->ProcessMouseButtonDown(0, Modifiers());
    else ctx->ProcessMouseButtonUp(0, Modifiers());
    return interacting;
}

bool RmlHost::PointerWheel (Entity e, float delta) {
    if (!impl_) return false;
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end()) return false;
    return !it->second.context->ProcessMouseWheel(-delta, Modifiers());
}

void RmlHost::PointerLeave (Entity e) {
    if (!impl_) return;
    auto it = impl_->docs.find(e);
    if (it == impl_->docs.end() || !it->second.hovered) return;
    it->second.hovered = false;
    it->second.context->ProcessMouseLeave();
}

bool RmlHost::WantsKeyboard () const {
    if (!impl_) return false;
    for (const auto& kv : impl_->docs) {
        Rml::Element* focus = kv.second.context->GetFocusElement();
        if (focus == nullptr) continue;
        const Rml::String& tag = focus->GetTagName();
        if (tag == "input" || tag == "textarea" || tag == "select") return true;
    }
    return false;
}

bool RmlHost::HandleSdlEvent (const SDL_Event& event) {
    if (!impl_ || !WantsKeyboard()) return false;
    for (auto& kv : impl_->docs) {
        Rml::Context* ctx = kv.second.context;
        if (ctx->GetFocusElement() == nullptr) continue;
        switch (event.type) {
        case SDL_KEYDOWN:
            ctx->ProcessKeyDown(MapKey(event.key.keysym.sym), Modifiers());
            return true;
        case SDL_KEYUP:
            ctx->ProcessKeyUp(MapKey(event.key.keysym.sym), Modifiers());
            return true;
        case SDL_TEXTINPUT:
            ctx->ProcessTextInput(Rml::String(event.text.text));
            return true;
        default:
            return false;
        }
    }
    return false;
}

void RmlHost::ClearFocus () {
    if (!impl_) return;
    for (auto& kv : impl_->docs)
        if (Rml::Element* focus = kv.second.context->GetFocusElement()) focus->Blur();
}

void RmlHost::FireEvent (void* document, const std::string& handler, const std::string& element,
                         const std::string& type) {
    if (!impl_ || !onEvent_) return;
    for (auto& kv : impl_->docs) {
        if (kv.second.document == document) {
            onEvent_(kv.first, handler, element, type);
            return;
        }
    }
}

namespace {

Rml::Element* Find (const std::unordered_map<Entity, RmlHost::Impl::Doc>* docs, Entity e, const std::string& id) {
    if (docs == nullptr) return nullptr;
    auto it = docs->find(e);
    if (it == docs->end() || it->second.document == nullptr) return nullptr;
    return id.empty() ? it->second.document : it->second.document->GetElementById(id);
}

} // namespace

bool RmlHost::SetInnerRml (Entity e, const std::string& id, const std::string& rml) {
    Rml::Element* el = Find(impl_ ? &impl_->docs : nullptr, e, id);
    if (el) el->SetInnerRML(rml);
    return el != nullptr;
}

bool RmlHost::SetClass (Entity e, const std::string& id, const std::string& cls, bool on) {
    Rml::Element* el = Find(impl_ ? &impl_->docs : nullptr, e, id);
    if (el) el->SetClass(cls, on);
    return el != nullptr;
}

bool RmlHost::SetProperty (Entity e, const std::string& id, const std::string& name, const std::string& value) {
    Rml::Element* el = Find(impl_ ? &impl_->docs : nullptr, e, id);
    return el && el->SetProperty(name, value);
}

bool RmlHost::SetAttribute (Entity e, const std::string& id, const std::string& name, const std::string& value) {
    Rml::Element* el = Find(impl_ ? &impl_->docs : nullptr, e, id);
    if (el) el->SetAttribute(name, value);
    return el != nullptr;
}

std::string RmlHost::GetValue (Entity e, const std::string& id) const {
    Rml::Element* el = Find(impl_ ? &impl_->docs : nullptr, e, id);
    if (el == nullptr) return {};
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(el)) return control->GetValue();
    return el->GetInnerRML();
}

} // namespace st::ui
