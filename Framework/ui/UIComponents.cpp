#include "ui/UIComponents.h"

#include "wiScene.h"

#include <algorithm>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st::ui {

namespace {

// One body for both directions of every field.
template <typename T>
void Io (wi::Archive& a, T& v) {
    if (a.IsReadMode()) a >> v;
    else a << v;
}

template <typename E>
void IoEnum (wi::Archive& a, E& e) {
    uint32_t v = (uint32_t)e;
    Io(a, v);
    e = (E)v;
}

void IoEntity (wi::Archive& a, Entity& e, wi::ecs::EntitySerializer& seri) {
    wi::ecs::SerializeEntity(a, e, seri);
}

} // namespace

// Version 0 for all of them. A field added later goes at the END, read only when
//	seri.GetVersion() says the archive has it - see wiScene_Serializers.cpp for the pattern.

void UICanvasComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer&) {
    Io(a, _flags);
    IoEnum(a, renderMode);
    IoEnum(a, scaleMode);
    Io(a, referenceResolution);
    Io(a, match);
    Io(a, scaleFactor);
    Io(a, sortOrder);
}

void UIRectComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer&) {
    Io(a, _flags);
    Io(a, anchorMin);
    Io(a, anchorMax);
    Io(a, pivot);
    Io(a, position);
    Io(a, size);
    Io(a, rotation);
    Io(a, scale);
    Io(a, order);
    Io(a, opacity);
}

void UIImageComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer&) {
    Io(a, _flags);
    Io(a, textureName);
    Io(a, color);
    IoEnum(a, type);
    Io(a, border);
    Io(a, borderScale);
    Io(a, fillAmount);
    Io(a, cornerRadius);
    IoEnum(a, gradient);
    Io(a, gradientColor);
    if (a.IsReadMode()) _loaded.clear();   // resolved by the UI system on its next frame
}

void UITextComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer&) {
    Io(a, _flags);
    Io(a, text);
    Io(a, fontName);
    Io(a, size);
    Io(a, color);
    IoEnum(a, hAlign);
    IoEnum(a, vAlign);
    Io(a, shadowColor);
    Io(a, shadowOffset);
    Io(a, bolden);
    Io(a, softness);
    Io(a, spacing);
    if (a.IsReadMode()) _fontLoaded = false;
}

void UIButtonComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer& seri) {
    Io(a, _flags);
    IoEntity(a, targetGraphic, seri);
    Io(a, normalColor);
    Io(a, hoverColor);
    Io(a, pressedColor);
    Io(a, disabledColor);
    Io(a, fadeDuration);
    Io(a, onClick);
    IoEntity(a, eventTarget, seri);
}

void UIToggleComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer& seri) {
    Io(a, _flags);
    IoEntity(a, graphic, seri);
    Io(a, onValueChanged);
    IoEntity(a, eventTarget, seri);
}

void UISliderComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer& seri) {
    Io(a, _flags);
    IoEnum(a, direction);
    Io(a, minValue);
    Io(a, maxValue);
    Io(a, value);
    IoEntity(a, fillRect, seri);
    IoEntity(a, handleRect, seri);
    Io(a, onValueChanged);
    IoEntity(a, eventTarget, seri);
}

void UILayoutComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer&) {
    Io(a, _flags);
    IoEnum(a, type);
    Io(a, padding);
    Io(a, spacing);
    Io(a, childAlignment);
    Io(a, cellSize);
    Io(a, columns);
}

void UIDocumentComponent::Serialize (wi::Archive& a, wi::ecs::EntitySerializer& seri) {
    Io(a, _flags);
    Io(a, source);
    Io(a, fonts);
    IoEntity(a, eventTarget, seri);
}

// ------------------------------------------------------------------------- managers ---

void RegisterUIComponents (Scene& scene) {
    wi::ecs::ComponentLibrary& lib = scene.componentLibrary;
    if (lib.entries.find(kRects) != lib.entries.end()) return;
    lib.Register<UICanvasComponent>(kCanvases);
    lib.Register<UIRectComponent>(kRects);
    lib.Register<UIImageComponent>(kImages);
    lib.Register<UITextComponent>(kTexts);
    lib.Register<UIButtonComponent>(kButtons);
    lib.Register<UIToggleComponent>(kToggles);
    lib.Register<UISliderComponent>(kSliders);
    lib.Register<UILayoutComponent>(kLayouts);
    lib.Register<UIDocumentComponent>(kDocuments);
}

void InstallUIComponents () {
    wi::scene::AddSceneComponentRegistrar(&RegisterUIComponents);
}

namespace {

template <typename T>
wi::ecs::ComponentManager<T>& Manager (Scene& scene, const char* key) {
    wi::ecs::ComponentManager<T>* m = scene.componentLibrary.Get<T>(key);
    if (m == nullptr) {
        // A Scene built before InstallUIComponents (only the global one can be).
        RegisterUIComponents(scene);
        m = scene.componentLibrary.Get<T>(key);
    }
    return *m;
}

} // namespace

wi::ecs::ComponentManager<UICanvasComponent>&   Canvases (Scene& s)  { return Manager<UICanvasComponent>(s, kCanvases); }
wi::ecs::ComponentManager<UIRectComponent>&     Rects (Scene& s)     { return Manager<UIRectComponent>(s, kRects); }
wi::ecs::ComponentManager<UIImageComponent>&    Images (Scene& s)    { return Manager<UIImageComponent>(s, kImages); }
wi::ecs::ComponentManager<UITextComponent>&     Texts (Scene& s)     { return Manager<UITextComponent>(s, kTexts); }
wi::ecs::ComponentManager<UIButtonComponent>&   Buttons (Scene& s)   { return Manager<UIButtonComponent>(s, kButtons); }
wi::ecs::ComponentManager<UIToggleComponent>&   Toggles (Scene& s)   { return Manager<UIToggleComponent>(s, kToggles); }
wi::ecs::ComponentManager<UISliderComponent>&   Sliders (Scene& s)   { return Manager<UISliderComponent>(s, kSliders); }
wi::ecs::ComponentManager<UILayoutComponent>&   Layouts (Scene& s)   { return Manager<UILayoutComponent>(s, kLayouts); }
wi::ecs::ComponentManager<UIDocumentComponent>& Documents (Scene& s) { return Manager<UIDocumentComponent>(s, kDocuments); }

// ------------------------------------------------------------------------ creation ---

const char* PresetName (Preset preset) {
    switch (preset) {
    case Preset::Canvas:           return "Canvas";
    case Preset::WorldCanvas:      return "World Canvas";
    case Preset::Panel:            return "Panel";
    case Preset::Image:            return "Image";
    case Preset::Text:             return "Text";
    case Preset::Button:           return "Button";
    case Preset::Toggle:           return "Toggle";
    case Preset::Slider:           return "Slider";
    case Preset::Document:         return "Document";
    case Preset::VerticalLayout:   return "Vertical Layout";
    case Preset::HorizontalLayout: return "Horizontal Layout";
    case Preset::Grid:             return "Grid";
    }
    return "UI";
}

Entity FindCanvas (Scene& scene, Entity e) {
    auto& canvases = Canvases(scene);
    for (int guard = 0; e != INVALID_ENTITY && guard < 256; ++guard) {
        if (canvases.Contains(e)) return e;
        const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
        e = h ? h->parentID : INVALID_ENTITY;
    }
    return INVALID_ENTITY;
}

namespace {

std::string UniqueName (Scene& scene, const std::string& base) {
    if (scene.Entity_FindByName(base) == INVALID_ENTITY) return base;
    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = base + " " + std::to_string(i);
        if (scene.Entity_FindByName(candidate) == INVALID_ENTITY) return candidate;
    }
    return base;
}

struct Maker {
    Scene& scene;
    wi::vector<Entity>* created;

    Entity Element (const std::string& name, Entity parent) {
        // Sibling order is saved, not derived: entity ids are renumbered when a scene loads.
        int order = 0;
        auto& rects = Rects(scene);
        for (size_t i = 0; i < scene.hierarchy.GetCount(); ++i) {
            if (scene.hierarchy[i].parentID != parent || parent == INVALID_ENTITY) continue;
            if (const UIRectComponent* sibling = rects.GetComponent(scene.hierarchy.GetEntity(i)))
                order = std::max(order, sibling->order + 1);
        }
        const Entity e = scene.Entity_CreateTransform(UniqueName(scene, name));
        rects.Create(e).order = order;
        if (parent != INVALID_ENTITY) scene.Component_Attach(e, parent);
        if (created) created->push_back(e);
        return e;
    }
};

void Stretch (UIRectComponent& r, const XMFLOAT4& inset = XMFLOAT4(0, 0, 0, 0)) {
    r.anchorMin = XMFLOAT2(0, 0);
    r.anchorMax = XMFLOAT2(1, 1);
    r.pivot = XMFLOAT2(0.5f, 0.5f);
    // size is the extra over the anchors, position moves the pivot: an inset of l,t,r,b
    r.size = XMFLOAT2(-(inset.x + inset.z), -(inset.y + inset.w));
    r.position = XMFLOAT2((inset.x - inset.z) * 0.5f, (inset.y - inset.w) * 0.5f);
}

UITextComponent& MakeText (Scene& scene, Entity e, const char* text, float size) {
    UITextComponent& t = Texts(scene).Create(e);
    t.text = text;
    t.size = size;
    return t;
}

} // namespace

Entity FindOrCreateScreenCanvas (Scene& scene, bool create) {
    auto& canvases = Canvases(scene);
    for (size_t i = 0; i < canvases.GetCount(); ++i)
        if (canvases[i].renderMode == UICanvasComponent::RenderMode::Screen) return canvases.GetEntity(i);
    return create ? Create(scene, Preset::Canvas, INVALID_ENTITY) : INVALID_ENTITY;
}

Entity Create (Scene& scene, Preset preset, Entity parent, const std::string& name,
               wi::vector<Entity>* created) {
    Maker make{ scene, created };
    const std::string label = name.empty() ? PresetName(preset) : name;

    if (preset == Preset::Canvas) {
        const Entity e = make.Element(label, parent);
        Canvases(scene).Create(e);
        UIRectComponent& r = *Rects(scene).GetComponent(e);
        Stretch(r);
        r.pivot = XMFLOAT2(0, 0);
        return e;
    }
    if (preset == Preset::WorldCanvas) {
        // A plane to show it on: the canvas renders into the plane's material.
        const Entity e = scene.Entity_CreatePlane(UniqueName(scene, label));
        if (created) {
            auto add = [created](Entity x) {
                if (x != INVALID_ENTITY && std::find(created->begin(), created->end(), x) == created->end())
                    created->push_back(x);
            };
            add(e);
            if (const ObjectComponent* o = scene.objects.GetComponent(e)) {
                add(o->meshID);
                if (const MeshComponent* m = scene.meshes.GetComponent(o->meshID))
                    for (const auto& subset : m->subsets) add(subset.materialID);
            }
        }
        Rects(scene).Create(e);
        UICanvasComponent& c = Canvases(scene).Create(e);
        c.renderMode = UICanvasComponent::RenderMode::World;
        c.scaleMode = UICanvasComponent::ScaleMode::ConstantPixel;
        c.referenceResolution = XMFLOAT2(1024, 576);
        Stretch(*Rects(scene).GetComponent(e));
        Rects(scene).GetComponent(e)->pivot = XMFLOAT2(0, 0);
        if (TransformComponent* t = scene.transforms.GetComponent(e)) {
            // Upright, facing -Z (towards a camera looking down +Z), 16:9 metres.
            t->RotateRollPitchYaw(XMFLOAT3(-XM_PIDIV2, 0, 0));
            t->Scale(XMFLOAT3(1.6f, 1, 0.9f));
            t->UpdateTransform();
        }
        if (parent != INVALID_ENTITY) scene.Component_Attach(e, parent);
        return e;
    }

    // Everything else lives on a canvas.
    if (parent == INVALID_ENTITY || !Rects(scene).Contains(parent)) {
        parent = FindOrCreateScreenCanvas(scene, false);
        if (parent == INVALID_ENTITY) parent = Create(scene, Preset::Canvas, INVALID_ENTITY, "", created);
    }

    const Entity e = make.Element(label, parent);
    UIRectComponent& r = *Rects(scene).GetComponent(e);
    switch (preset) {
    case Preset::Panel: {
        r.size = XMFLOAT2(480, 320);
        UIImageComponent& img = Images(scene).Create(e);
        img.color = XMFLOAT4(0.08f, 0.09f, 0.11f, 0.85f);
        img.cornerRadius = 10;
        break;
    }
    case Preset::Image:
        r.size = XMFLOAT2(100, 100);
        Images(scene).Create(e);
        break;
    case Preset::Text:
        r.size = XMFLOAT2(300, 60);
        MakeText(scene, e, "New Text", 32);
        break;
    case Preset::Button: {
        r.size = XMFLOAT2(240, 64);
        UIImageComponent& img = Images(scene).Create(e);
        img.color = XMFLOAT4(0.18f, 0.42f, 0.82f, 1);
        img.cornerRadius = 8;
        Buttons(scene).Create(e);
        const Entity text = make.Element("Text", e);
        Stretch(*Rects(scene).GetComponent(text));
        UITextComponent& label = MakeText(scene, text, "Button", 28);
        label._flags &= ~UITextComponent::WRAP;
        break;
    }
    case Preset::Toggle: {
        r.size = XMFLOAT2(260, 40);
        UIToggleComponent& toggle = Toggles(scene).Create(e);
        UIButtonComponent& button = Buttons(scene).Create(e);

        const Entity box = make.Element("Background", e);
        UIRectComponent& br = *Rects(scene).GetComponent(box);
        br.anchorMin = br.anchorMax = XMFLOAT2(0, 0.5f);
        br.pivot = XMFLOAT2(0, 0.5f);
        br.size = XMFLOAT2(36, 36);
        UIImageComponent& bi = Images(scene).Create(box);
        bi.color = XMFLOAT4(0.85f, 0.87f, 0.9f, 1);
        bi.cornerRadius = 6;
        button.targetGraphic = box;

        const Entity check = make.Element("Checkmark", box);
        Rects(scene).GetComponent(check)->size = XMFLOAT2(22, 22);
        UIImageComponent& ci = Images(scene).Create(check);
        ci.color = XMFLOAT4(0.18f, 0.42f, 0.82f, 1);
        ci.cornerRadius = 4;
        ci._flags &= ~UIImageComponent::RAYCAST_TARGET;
        toggle.graphic = check;

        const Entity text = make.Element("Label", e);
        Stretch(*Rects(scene).GetComponent(text), XMFLOAT4(48, 0, 0, 0));
        UITextComponent& t = MakeText(scene, text, "Toggle", 26);
        t.hAlign = UITextComponent::Align::Start;
        t._flags &= ~UITextComponent::WRAP;
        break;
    }
    case Preset::Slider: {
        r.size = XMFLOAT2(320, 28);
        UISliderComponent& slider = Sliders(scene).Create(e);
        UIButtonComponent& button = Buttons(scene).Create(e);
        UIImageComponent& track = Images(scene).Create(e);
        track.color = XMFLOAT4(0.2f, 0.22f, 0.25f, 1);
        track.cornerRadius = 6;

        const Entity fill = make.Element("Fill", e);
        UIRectComponent& fr = *Rects(scene).GetComponent(fill);
        fr.anchorMin = XMFLOAT2(0, 0);
        fr.anchorMax = XMFLOAT2(slider.Normalized(), 1);
        fr.pivot = XMFLOAT2(0, 0.5f);
        fr.size = XMFLOAT2(0, 0);
        UIImageComponent& fi = Images(scene).Create(fill);
        fi.color = XMFLOAT4(0.18f, 0.42f, 0.82f, 1);
        fi.cornerRadius = 6;
        fi._flags &= ~UIImageComponent::RAYCAST_TARGET;
        slider.fillRect = fill;

        const Entity handle = make.Element("Handle", e);
        UIRectComponent& hr = *Rects(scene).GetComponent(handle);
        hr.anchorMin = hr.anchorMax = XMFLOAT2(slider.Normalized(), 0.5f);
        hr.size = XMFLOAT2(22, 40);
        UIImageComponent& hi = Images(scene).Create(handle);
        hi.cornerRadius = 6;
        hi._flags &= ~UIImageComponent::RAYCAST_TARGET;
        slider.handleRect = handle;
        button.targetGraphic = handle;
        break;
    }
    case Preset::Document:
        r.size = XMFLOAT2(640, 400);
        Documents(scene).Create(e);
        break;
    case Preset::VerticalLayout:
    case Preset::HorizontalLayout:
    case Preset::Grid: {
        r.size = XMFLOAT2(360, 360);
        UILayoutComponent& l = Layouts(scene).Create(e);
        l.type = preset == Preset::Grid ? UILayoutComponent::Type::Grid
               : preset == Preset::HorizontalLayout ? UILayoutComponent::Type::Horizontal
               : UILayoutComponent::Type::Vertical;
        l.padding = XMFLOAT4(12, 12, 12, 12);
        if (l.type != UILayoutComponent::Type::Grid)
            l._flags |= l.type == UILayoutComponent::Type::Vertical ? UILayoutComponent::EXPAND_WIDTH
                                                                    : UILayoutComponent::EXPAND_HEIGHT;
        UIImageComponent& img = Images(scene).Create(e);
        img.color = XMFLOAT4(1, 1, 1, 0.06f);
        img.cornerRadius = 8;
        break;
    }
    default:
        break;
    }
    return e;
}

} // namespace st::ui
