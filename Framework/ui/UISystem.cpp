#include "ui/UISystem.h"
#include "ui/UIRml.h"

#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"
#include "StTaipan.h"

#include "wiScene.h"
#include "wiBacklog.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace tb = st::taipan;

namespace st::ui {

namespace {

XMFLOAT4 Mul (const XMFLOAT4& a, const XMFLOAT4& b) {
    return XMFLOAT4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

XMFLOAT4 Intersect (const XMFLOAT4& a, const XMFLOAT4& b) {
    return XMFLOAT4(std::max(a.x, b.x), std::max(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w));
}

bool Inside (const XMFLOAT4& rect, const XMFLOAT4& clip, XMFLOAT2 p) {
    return p.x >= rect.x && p.y >= rect.y && p.x < rect.x + rect.z && p.y < rect.y + rect.w &&
           p.x >= clip.x && p.y >= clip.y && p.x < clip.z && p.y < clip.w;
}

float AlignFactor (UITextComponent::Align a) {
    return a == UITextComponent::Align::Start ? 0.0f : a == UITextComponent::Align::End ? 1.0f : 0.5f;
}

// The name a subscription uses for an event kind: a toggle and a slider are both "value".
const char* EventKey (UIEvent::Kind kind) {
    switch (kind) {
    case UIEvent::Kind::Click:    return "click";
    case UIEvent::Kind::Toggle:
    case UIEvent::Kind::Slider:   return "value";
    case UIEvent::Kind::Document: return "document";
    case UIEvent::Kind::Enter:    return "enter";
    case UIEvent::Kind::Exit:     return "exit";
    case UIEvent::Kind::Down:     return "down";
    case UIEvent::Kind::Up:       return "up";
    }
    return "";
}

bool KindFromKey (const std::string& key, UIEvent::Kind& out) {
    for (UIEvent::Kind k : { UIEvent::Kind::Click, UIEvent::Kind::Toggle, UIEvent::Kind::Enter,
                             UIEvent::Kind::Exit, UIEvent::Kind::Down, UIEvent::Kind::Up }) {
        if (key == EventKey(k)) { out = k; return true; }
    }
    return false;
}

bool SameKey (UIEvent::Kind a, UIEvent::Kind b) { return std::strcmp(EventKey(a), EventKey(b)) == 0; }

bool Has (const std::vector<Entity>& v, Entity e) { return std::find(v.begin(), v.end(), e) != v.end(); }

} // namespace

struct UISystem::Ctx {
    Scene&   scene;
    Entity   canvas;
    float    pixelsPerUnit;
    bool     world;
};

UISystem& UISystem::Get () {
    static UISystem system;
    return system;
}

// --------------------------------------------------------------------------- update ---

void UISystem::Update (Scene& scene, float dt, XMUINT2 targetSize, bool interactive) {
    ++frame_;
    target_ = targetSize;
    interactive_ = interactive;
    scene_ = &scene;
    // A subscription outlives nothing: its element is gone, so is it.
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&scene](const Sub& s) {
        return !Rects(scene).Contains(s.element);
    }), subs_.end());
    if (Rects(scene).GetCount() == 0) {
        order_.clear();
        hoverChain_.clear();
        hovered_ = hoveredSel_ = pressed_ = downElement_ = INVALID_ENTITY;
        RmlHost::Get().Retain({});
        return;
    }
    Resources(scene);
    ApplyWidgets(scene);
    Gather(scene);
    order_.clear();
    worldElement_.clear();
    for (Entity canvas : roots_) LayoutCanvas(scene, canvas);
    Interact(scene, dt, interactive);
    Visuals(scene, dt, interactive);
    Documents_(scene, interactive);
}

void UISystem::Resources (Scene& scene) {
    auto& images = Images(scene);
    for (size_t i = 0; i < images.GetCount(); ++i) {
        UIImageComponent& img = images[i];
        if (img._loaded == img.textureName) continue;
        img._loaded = img.textureName;
        img._texture = img.textureName.empty() || !render::Installed() ? nullptr
                     : render::Get().UI().LoadTexture(img.textureName);
    }
    auto& texts = Texts(scene);
    for (size_t i = 0; i < texts.GetCount(); ++i) {
        UITextComponent& t = texts[i];
        if (t._fontLoaded && t._loadedFont == t.fontName) continue;
        t._fontLoaded = true;
        t._loadedFont = t.fontName;
        t._style = t.fontName.empty() || !render::Installed() ? 0 : render::Get().UI().LoadFont(t.fontName);
    }
}

// Widgets drive other elements' rects: a toggle shows its checkmark, a slider sizes its
//	fill and places its handle. Done before layout so the result is this frame's.
void UISystem::ApplyWidgets (Scene& scene) {
    auto& rects = Rects(scene);
    auto& toggles = Toggles(scene);
    for (size_t i = 0; i < toggles.GetCount(); ++i)
        if (UIRectComponent* g = rects.GetComponent(toggles[i].graphic)) g->SetHidden(!toggles[i].IsOn());

    auto& sliders = Sliders(scene);
    for (size_t i = 0; i < sliders.GetCount(); ++i) {
        const UISliderComponent& s = sliders[i];
        const float t = s.Normalized();
        if (UIRectComponent* f = rects.GetComponent(s.fillRect)) {
            switch (s.direction) {
            case UISliderComponent::Direction::LeftToRight: f->anchorMin = XMFLOAT2(0, 0);     f->anchorMax = XMFLOAT2(t, 1); break;
            case UISliderComponent::Direction::RightToLeft: f->anchorMin = XMFLOAT2(1 - t, 0); f->anchorMax = XMFLOAT2(1, 1); break;
            case UISliderComponent::Direction::BottomToTop: f->anchorMin = XMFLOAT2(0, 1 - t); f->anchorMax = XMFLOAT2(1, 1); break;
            case UISliderComponent::Direction::TopToBottom: f->anchorMin = XMFLOAT2(0, 0);     f->anchorMax = XMFLOAT2(1, t); break;
            }
        }
        if (UIRectComponent* h = rects.GetComponent(s.handleRect)) {
            XMFLOAT2 a(t, 0.5f);
            if (s.direction == UISliderComponent::Direction::RightToLeft) a = XMFLOAT2(1 - t, 0.5f);
            if (s.direction == UISliderComponent::Direction::BottomToTop) a = XMFLOAT2(0.5f, 1 - t);
            if (s.direction == UISliderComponent::Direction::TopToBottom) a = XMFLOAT2(0.5f, t);
            h->anchorMin = h->anchorMax = a;
        }
    }
}

void UISystem::Gather (Scene& scene) {
    auto& rects = Rects(scene);
    auto& canvases = Canvases(scene);
    for (auto& kv : children_) kv.second.clear();
    roots_.clear();
    for (size_t i = 0; i < rects.GetCount(); ++i) {
        UIRectComponent& r = rects[i];
        r._visible = false;
        r._laidOut = false;
        const Entity e = rects.GetEntity(i);
        const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
        const Entity parent = h ? h->parentID : INVALID_ENTITY;
        if (parent != INVALID_ENTITY && rects.Contains(parent)) children_[parent].push_back(e);
        else if (canvases.Contains(e)) roots_.push_back(e);
    }
    // Siblings: `order`, then creation order. By ENTITY id, not manager index: a Play-mode
    //	restore (or an undo) re-reads a changed entity into the END of its managers, which
    //	reshuffled siblings; the id is what a restore keeps.
    for (auto& kv : children_) {
        std::sort(kv.second.begin(), kv.second.end(), [&rects](Entity a, Entity b) {
            const int oa = rects.GetComponent(a)->order, ob = rects.GetComponent(b)->order;
            return oa != ob ? oa < ob : a < b;
        });
    }
    std::sort(roots_.begin(), roots_.end(), [&canvases](Entity a, Entity b) {
        const int oa = canvases.GetComponent(a)->sortOrder, ob = canvases.GetComponent(b)->sortOrder;
        return oa != ob ? oa < ob : a < b;
    });
}

void UISystem::LayoutCanvas (Scene& scene, Entity e) {
    UICanvasComponent& c = *Canvases(scene).GetComponent(e);
    if (c.IsHidden()) return;
    const bool world = c.renderMode == UICanvasComponent::RenderMode::World;
    XMFLOAT2 pixels = world ? c.referenceResolution : XMFLOAT2((float)target_.x, (float)target_.y);
    pixels.x = std::max(pixels.x, 1.0f);
    pixels.y = std::max(pixels.y, 1.0f);
    float s = std::max(0.01f, c.scaleFactor);
    if (!world && c.scaleMode == UICanvasComponent::ScaleMode::ScaleWithScreen) {
        // Unity's CanvasScaler: blend the width and height ratios in log space.
        const float lw = std::log2(pixels.x / std::max(1.0f, c.referenceResolution.x));
        const float lh = std::log2(pixels.y / std::max(1.0f, c.referenceResolution.y));
        s = std::pow(2.0f, lw + (lh - lw) * wi::math::saturate(c.match));
    }
    c._scale = s;
    c._size = XMFLOAT2(pixels.x / s, pixels.y / s);
    Ctx ctx{ scene, e, s, world };
    const XMFLOAT4 rect(0, 0, c._size.x, c._size.y);
    LayoutNode(ctx, e, rect, XMFLOAT2(1, 1), 1.0f, XMFLOAT4(0, 0, pixels.x, pixels.y), true);
}

void UISystem::LayoutNode (Ctx& ctx, Entity e, const XMFLOAT4& pr, XMFLOAT2 k, float opacity,
                           const XMFLOAT4& clip, bool root) {
    UIRectComponent* rp = Rects(ctx.scene).GetComponent(e);
    if (rp == nullptr || rp->IsHidden()) return;
    UIRectComponent& r = *rp;
    const XMFLOAT2 kOwn(k.x * r.scale.x, k.y * r.scale.y);
    if (root) {
        r._rect = pr;
    } else if (!r._laidOut) {
        const float w = (r.anchorMax.x - r.anchorMin.x) * pr.z + r.size.x * kOwn.x;
        const float h = (r.anchorMax.y - r.anchorMin.y) * pr.w + r.size.y * kOwn.y;
        const float px = pr.x + (r.anchorMin.x + (r.anchorMax.x - r.anchorMin.x) * r.pivot.x) * pr.z + r.position.x * k.x;
        const float py = pr.y + (r.anchorMin.y + (r.anchorMax.y - r.anchorMin.y) * r.pivot.y) * pr.w + r.position.y * k.y;
        r._rect = XMFLOAT4(px - r.pivot.x * w, py - r.pivot.y * h, w, h);
    }
    if (Layouts(ctx.scene).Contains(e)) Arrange(ctx, e, kOwn);

    const float s = ctx.pixelsPerUnit;
    r._pixels = XMFLOAT4(r._rect.x * s, r._rect.y * s, r._rect.z * s, r._rect.w * s);
    r._scale = s * (kOwn.x + kOwn.y) * 0.5f;
    r._opacity = opacity * r.opacity;
    r._clip = clip;
    r._canvas = ctx.canvas;
    r._visible = true;
    order_.push_back(e);
    if (ctx.world) worldElement_[e] = true;

    XMFLOAT4 childClip = clip;
    if (r._flags & UIRectComponent::CLIP_CHILDREN)
        childClip = Intersect(clip, XMFLOAT4(r._pixels.x, r._pixels.y, r._pixels.x + r._pixels.z, r._pixels.y + r._pixels.w));
    auto it = children_.find(e);
    if (it == children_.end()) return;
    for (Entity child : it->second)
        LayoutNode(ctx, child, r._rect, kOwn, r._opacity, childClip, false);
}

// A UILayout places its direct children itself; they keep their own size unless the
//	layout expands them.
void UISystem::Arrange (Ctx& ctx, Entity e, XMFLOAT2 k) {
    auto it = children_.find(e);
    if (it == children_.end()) return;
    auto& rects = Rects(ctx.scene);
    const UILayoutComponent& l = *Layouts(ctx.scene).GetComponent(e);
    UIRectComponent& r = *rects.GetComponent(e);

    std::vector<UIRectComponent*> items;
    for (Entity c : it->second) {
        UIRectComponent* cr = rects.GetComponent(c);
        if (cr && !cr->IsHidden() && !(cr->_flags & UIRectComponent::IGNORE_LAYOUT)) items.push_back(cr);
    }
    if (l._flags & UILayoutComponent::REVERSE) std::reverse(items.begin(), items.end());
    const size_t n = items.size();
    const XMFLOAT4 pad(l.padding.x * k.x, l.padding.y * k.y, l.padding.z * k.x, l.padding.w * k.y);
    const XMFLOAT2 sp(l.spacing.x * k.x, l.spacing.y * k.y);
    const float col = (float)(l.childAlignment % 3) * 0.5f;
    const float row = (float)(std::min(l.childAlignment, 8u) / 3) * 0.5f;

    auto pref = [&k](const UIRectComponent* c) {
        return XMFLOAT2(std::max(0.0f, c->size.x * k.x * c->scale.x), std::max(0.0f, c->size.y * k.y * c->scale.y));
    };
    // FIT: grow/shrink this rect around its own pivot to what the content needs.
    auto fit = [&r](float contentW, float contentH, bool fw, bool fh) {
        if (fw) {
            const float pivotX = r._rect.x + r.pivot.x * r._rect.z;
            r._rect.z = contentW;
            r._rect.x = pivotX - r.pivot.x * contentW;
        }
        if (fh) {
            const float pivotY = r._rect.y + r.pivot.y * r._rect.w;
            r._rect.w = contentH;
            r._rect.y = pivotY - r.pivot.y * contentH;
        }
    };
    const bool fitW = l._flags & UILayoutComponent::FIT_WIDTH;
    const bool fitH = l._flags & UILayoutComponent::FIT_HEIGHT;
    const bool expandW = l._flags & UILayoutComponent::EXPAND_WIDTH;
    const bool expandH = l._flags & UILayoutComponent::EXPAND_HEIGHT;

    if (l.type == UILayoutComponent::Type::Grid) {
        const XMFLOAT2 cell(l.cellSize.x * k.x, l.cellSize.y * k.y);
        const float innerW = r._rect.z - pad.x - pad.z;
        int cols = l.columns > 0 ? l.columns : (int)std::floor((innerW + sp.x) / std::max(1.0f, cell.x + sp.x));
        cols = std::max(1, std::min(cols, (int)std::max<size_t>(n, 1)));
        const int rows = n == 0 ? 0 : (int)((n + cols - 1) / cols);
        const float blockW = cols * cell.x + (cols - 1) * sp.x;
        const float blockH = rows * cell.y + std::max(0, rows - 1) * sp.y;
        fit(blockW + pad.x + pad.z, blockH + pad.y + pad.w, fitW, fitH);
        const XMFLOAT4 in(r._rect.x + pad.x, r._rect.y + pad.y, r._rect.z - pad.x - pad.z, r._rect.w - pad.y - pad.w);
        const float x0 = in.x + (in.z - blockW) * col;
        const float y0 = in.y + (in.w - blockH) * row;
        for (size_t i = 0; i < n; ++i) {
            const int cx = (int)(i % cols), cy = (int)(i / cols);
            items[i]->_rect = XMFLOAT4(x0 + cx * (cell.x + sp.x), y0 + cy * (cell.y + sp.y), cell.x, cell.y);
            items[i]->_laidOut = true;
        }
        return;
    }

    const bool horizontal = l.type == UILayoutComponent::Type::Horizontal;
    const float spacing = horizontal ? sp.x : sp.y;
    const float innerMain = horizontal ? r._rect.z - pad.x - pad.z : r._rect.w - pad.y - pad.w;
    const bool expandMain = horizontal ? expandW : expandH;
    const bool expandCross = horizontal ? expandH : expandW;

    std::vector<XMFLOAT2> sizes(n);   // x = main, y = cross
    float total = 0, cross = 0;
    for (size_t i = 0; i < n; ++i) {
        const XMFLOAT2 p = pref(items[i]);
        sizes[i] = horizontal ? p : XMFLOAT2(p.y, p.x);
        if (expandMain && n > 0) sizes[i].x = std::max(0.0f, (innerMain - spacing * (n - 1)) / n);
        total += sizes[i].x;
        cross = std::max(cross, sizes[i].y);
    }
    total += n > 1 ? spacing * (n - 1) : 0;
    if (horizontal) fit(total + pad.x + pad.z, cross + pad.y + pad.w, fitW, fitH);
    else fit(cross + pad.x + pad.z, total + pad.y + pad.w, fitW, fitH);

    const XMFLOAT4 in(r._rect.x + pad.x, r._rect.y + pad.y, r._rect.z - pad.x - pad.z, r._rect.w - pad.y - pad.w);
    const float mainLen = horizontal ? in.z : in.w;
    const float crossLen = horizontal ? in.w : in.z;
    float cursor = (horizontal ? in.x : in.y) + (mainLen - total) * (horizontal ? col : row);
    for (size_t i = 0; i < n; ++i) {
        const float c = expandCross ? crossLen : sizes[i].y;
        const float crossPos = (horizontal ? in.y : in.x) + (crossLen - c) * (horizontal ? row : col);
        items[i]->_rect = horizontal ? XMFLOAT4(cursor, crossPos, sizes[i].x, c)
                                     : XMFLOAT4(crossPos, cursor, c, sizes[i].x);
        items[i]->_laidOut = true;
        cursor += sizes[i].x + spacing;
    }
}

// ---------------------------------------------------------------------------- input ---

bool UISystem::IsTarget (Scene& scene, Entity e) const {
    const UIRectComponent* r = Rects(scene).GetComponent(e);
    if (r && (r->_flags & UIRectComponent::BLOCK_POINTER)) return true;
    if (const UIImageComponent* i = Images(scene).GetComponent(e); i && (i->_flags & UIImageComponent::RAYCAST_TARGET)) return true;
    if (const UITextComponent* t = Texts(scene).GetComponent(e); t && (t->_flags & UITextComponent::RAYCAST_TARGET)) return true;
    if (const UIDocumentComponent* d = Documents(scene).GetComponent(e); d && (d->_flags & UIDocumentComponent::INTERACTIVE)) return true;
    return Buttons(scene).Contains(e) || Toggles(scene).Contains(e) || Sliders(scene).Contains(e);
}

// The pointer event bubbles up to the first element that does something with it.
Entity UISystem::Selectable (Scene& scene, Entity e) const {
    auto& buttons = Buttons(scene);
    auto& toggles = Toggles(scene);
    auto& sliders = Sliders(scene);
    for (int guard = 0; e != INVALID_ENTITY && guard < 64; ++guard) {
        if (buttons.Contains(e) || toggles.Contains(e) || sliders.Contains(e)) return e;
        if (Canvases(scene).Contains(e)) break;
        const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
        e = h ? h->parentID : INVALID_ENTITY;
    }
    return INVALID_ENTITY;
}

void UISystem::SetSlider (Scene& scene, Entity e, XMFLOAT2 p) {
    UISliderComponent* s = Sliders(scene).GetComponent(e);
    const UIRectComponent* r = Rects(scene).GetComponent(e);
    if (s == nullptr || r == nullptr || !(s->_flags & UISliderComponent::INTERACTABLE)) return;
    float t = 0;
    switch (s->direction) {
    case UISliderComponent::Direction::LeftToRight: t = (p.x - r->_pixels.x) / std::max(1.0f, r->_pixels.z); break;
    case UISliderComponent::Direction::RightToLeft: t = 1 - (p.x - r->_pixels.x) / std::max(1.0f, r->_pixels.z); break;
    case UISliderComponent::Direction::TopToBottom: t = (p.y - r->_pixels.y) / std::max(1.0f, r->_pixels.w); break;
    case UISliderComponent::Direction::BottomToTop: t = 1 - (p.y - r->_pixels.y) / std::max(1.0f, r->_pixels.w); break;
    }
    float v = s->minValue + wi::math::saturate(t) * (s->maxValue - s->minValue);
    if (s->_flags & UISliderComponent::WHOLE_NUMBERS) v = std::round(v);
    if (v == s->value) return;
    s->value = v;
    tb::Value args[2] = { EntityValue(e), tb::Value::Float(v) };
    Dispatch(scene, UIEvent::Kind::Slider, e, s->onValueChanged, s->eventTarget, std::to_string(v), args, 2);
    Notify(scene, UIEvent::Kind::Slider, e, std::to_string(v), args, 2, false);
}

void UISystem::Interact (Scene& scene, float, bool interactive) {
    hovered_ = hoveredSel_ = INVALID_ENTITY;
    const bool down = pointer_.down;
    const bool pressedEdge = down && !prevDown_;
    const bool releasedEdge = !down && prevDown_;
    prevDown_ = down;
    if (!interactive) {
        pressed_ = pressedDoc_ = downElement_ = INVALID_ENTITY;
        hoverChain_.clear();
        return;
    }

    auto& rects = Rects(scene);
    if (pointer_.valid) {
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            if (worldElement_.count(*it)) continue;
            const UIRectComponent* r = rects.GetComponent(*it);
            if (r && Inside(r->_pixels, r->_clip, pointer_.pos) && IsTarget(scene, *it)) {
                hovered_ = *it;
                break;
            }
        }
        if (hovered_ != INVALID_ENTITY) hoveredSel_ = Selectable(scene, hovered_);
    }
    PointerEvents(scene, pressedEdge, releasedEdge);

    if (pressedEdge) {
        pressed_ = hoveredSel_;
        if (pressed_ != INVALID_ENTITY && Sliders(scene).Contains(pressed_)) {
            Sliders(scene).GetComponent(pressed_)->_dragging = true;
            SetSlider(scene, pressed_, pointer_.pos);
        }
    } else if (down && pressed_ != INVALID_ENTITY) {
        if (UISliderComponent* s = Sliders(scene).GetComponent(pressed_); s && s->_dragging)
            SetSlider(scene, pressed_, pointer_.pos);
    }
    if (releasedEdge) {
        if (pressed_ != INVALID_ENTITY) {
            UISliderComponent* s = Sliders(scene).GetComponent(pressed_);
            if (s && s->_dragging) s->_dragging = false;
            else if (pressed_ == hoveredSel_) Click(scene, pressed_);
        }
        pressed_ = INVALID_ENTITY;
    }
}

bool UISystem::Click (Scene& scene, Entity e) {
    bool any = false;
    if (UIButtonComponent* b = Buttons(scene).GetComponent(e); b && b->IsInteractable()) {
        b->_clickFrame = frame_;
        tb::Value args[1] = { EntityValue(e) };
        Dispatch(scene, UIEvent::Kind::Click, e, b->onClick, b->eventTarget, "", args, 1);
        Notify(scene, UIEvent::Kind::Click, e, "", args, 1, false);
        any = true;
    }
    if (UIToggleComponent* t = Toggles(scene).GetComponent(e); t && (t->_flags & UIToggleComponent::INTERACTABLE)) {
        t->SetOn(!t->IsOn());
        if (UIRectComponent* g = Rects(scene).GetComponent(t->graphic)) g->SetHidden(!t->IsOn());
        tb::Value args[2] = { EntityValue(e), tb::Value::Bool(t->IsOn()) };
        Dispatch(scene, UIEvent::Kind::Toggle, e, t->onValueChanged, t->eventTarget,
                 t->IsOn() ? "true" : "false", args, 2);
        Notify(scene, UIEvent::Kind::Toggle, e, t->IsOn() ? "true" : "false", args, 2, false);
        any = true;
    }
    return any;
}

void UISystem::PointerEvents (Scene& scene, bool pressedEdge, bool releasedEdge) {
    std::vector<Entity> chain;
    for (Entity e = hovered_; e != INVALID_ENTITY && chain.size() < 64;) {
        chain.push_back(e);
        if (Canvases(scene).Contains(e)) break;
        const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
        e = h ? h->parentID : INVALID_ENTITY;
    }
    const std::vector<Entity> before = std::move(hoverChain_);
    hoverChain_ = chain;
    tb::Value args[1];
    for (Entity e : before) {   // deepest first: leave the child before its parent
        if (Has(chain, e)) continue;
        args[0] = EntityValue(e);
        Notify(scene, UIEvent::Kind::Exit, e, "", args, 1, true);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {   // enter the parent first
        if (Has(before, *it)) continue;
        args[0] = EntityValue(*it);
        Notify(scene, UIEvent::Kind::Enter, *it, "", args, 1, true);
    }
    if (pressedEdge && !chain.empty()) {
        downElement_ = INVALID_ENTITY;
        for (Entity e : chain) {
            if (HasSub(e, UIEvent::Kind::Down)) { downElement_ = e; break; }
        }
        if (downElement_ == INVALID_ENTITY) downElement_ = hoveredSel_ != INVALID_ENTITY ? hoveredSel_ : chain.front();
        args[0] = EntityValue(downElement_);
        Notify(scene, UIEvent::Kind::Down, downElement_, "", args, 1, true);
    }
    if (releasedEdge && downElement_ != INVALID_ENTITY) {
        const Entity e = downElement_;
        downElement_ = INVALID_ENTITY;
        args[0] = EntityValue(e);
        Notify(scene, UIEvent::Kind::Up, e, "", args, 1, true);
    }
}

bool UISystem::IsHovered (Entity e) const { return e != INVALID_ENTITY && Has(hoverChain_, e); }

bool UISystem::IsPressed (Entity e) const {
    return e != INVALID_ENTITY && (pressed_ == e || downElement_ == e);
}

bool UISystem::HasSub (Entity element, UIEvent::Kind kind) const {
    for (const Sub& s : subs_)
        if (s.element == element && SameKey(s.kind, kind)) return true;
    return false;
}

bool UISystem::Subscribe (Entity element, const std::string& event, Entity target, const std::string& handler) {
    UIEvent::Kind kind;
    if (element == INVALID_ENTITY || target == INVALID_ENTITY || handler.empty() || !KindFromKey(event, kind)) return false;
    for (const Sub& s : subs_)
        if (s.element == element && SameKey(s.kind, kind) && s.target == target && s.handler == handler) return true;
    subs_.push_back({ element, kind, target, handler });
    return true;
}

int UISystem::Unsubscribe (Entity element, const std::string& event, Entity target, const std::string& handler) {
    const size_t n = subs_.size();
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&](const Sub& s) {
        return s.element == element && s.target == target && (event.empty() || event == EventKey(s.kind)) &&
               (handler.empty() || handler == s.handler);
    }), subs_.end());
    return (int)(n - subs_.size());
}

void UISystem::UnsubscribeTarget (Entity target) {
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [target](const Sub& s) { return s.target == target; }),
                subs_.end());
}

void UISystem::Notify (Scene& scene, UIEvent::Kind kind, Entity sender, const std::string& value,
                       const void* args, int argc, bool toListeners) {
    if (toListeners) {
        UIEvent ev;
        ev.kind = kind;
        ev.sender = sender;
        ev.value = value;
        for (auto& l : listeners_) l.second(ev);
    }
    // A copy: a handler may subscribe, unsubscribe or destroy.
    std::vector<Sub> hit;
    for (const Sub& s : subs_)
        if (s.element == sender && SameKey(s.kind, kind)) hit.push_back(s);
    for (const Sub& s : hit) {
        wi::vector<NativeComponent*> scripts;
        scene.nativeComponents.GetAll(s.target, wi::scene::GetNativeTypeID<st::ScriptComponent>(), scripts);
        bool handled = false;
        for (NativeComponent* c : scripts)
            handled |= static_cast<st::ScriptComponent*>(c)->CallEvent(s.handler.c_str(), (const tb::Value*)args, argc);
        if (!handled && warned_.insert(s.handler + "@" + std::to_string(s.target)).second) {
            const NameComponent* name = scene.names.GetComponent(s.target);
            wi::backlog::post("[UI] UI.On: no running stScript on '" + (name ? name->name : std::to_string(s.target)) +
                              "' defines func " + s.handler, wi::backlog::LogLevel::Warning);
        }
    }
}

bool UISystem::WasClicked (Entity e) const {
    if (scene_ == nullptr) return false;
    const UIButtonComponent* b = Buttons(*scene_).GetComponent(e);
    return b && b->_clickFrame == frame_;
}

void UISystem::Dispatch (Scene& scene, UIEvent::Kind kind, Entity sender, const std::string& handler,
                         Entity target, const std::string& value, const void* args, int argc) {
    UIEvent ev;
    ev.kind = kind;
    ev.sender = sender;
    ev.handler = handler;
    ev.value = value;
    for (auto& l : listeners_) l.second(ev);

    if (handler.empty()) return;
    const Entity to = target != INVALID_ENTITY ? target : sender;
    wi::vector<NativeComponent*> scripts;
    scene.nativeComponents.GetAll(to, wi::scene::GetNativeTypeID<st::ScriptComponent>(), scripts);
    bool handled = false;
    for (NativeComponent* c : scripts)
        handled |= static_cast<st::ScriptComponent*>(c)->CallEvent(handler.c_str(), (const tb::Value*)args, argc);
    if (!handled && warned_.insert(handler + "@" + std::to_string(to)).second) {
        const NameComponent* name = scene.names.GetComponent(to);
        wi::backlog::post("[UI] no running stScript on '" + (name ? name->name : std::to_string(to)) +
                          "' defines func " + handler, wi::backlog::LogLevel::Warning);
    }
}

int UISystem::AddListener (std::function<void(const UIEvent&)> listener) {
    listeners_.emplace_back(nextListener_, std::move(listener));
    return nextListener_++;
}

void UISystem::RemoveListener (int handle) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [handle](const auto& l) { return l.first == handle; }),
                     listeners_.end());
}

// Button colour transitions, and the tint they put on their graphic.
void UISystem::Visuals (Scene& scene, float dt, bool interactive) {
    auto& images = Images(scene);
    auto& texts = Texts(scene);
    for (size_t i = 0; i < images.GetCount(); ++i) images[i]._tint = XMFLOAT4(1, 1, 1, 1);
    for (size_t i = 0; i < texts.GetCount(); ++i) texts[i]._tint = XMFLOAT4(1, 1, 1, 1);

    auto& buttons = Buttons(scene);
    for (size_t i = 0; i < buttons.GetCount(); ++i) {
        UIButtonComponent& b = buttons[i];
        const Entity e = buttons.GetEntity(i);
        if (!b.IsInteractable()) b._state = UIButtonComponent::State::Disabled;
        else if (!interactive) b._state = UIButtonComponent::State::Normal;
        else if (pressed_ == e && hoveredSel_ == e) b._state = UIButtonComponent::State::Pressed;
        else if (hoveredSel_ == e && pressed_ == INVALID_ENTITY) b._state = UIButtonComponent::State::Hover;
        else b._state = UIButtonComponent::State::Normal;
        const XMFLOAT4 want = b._state == UIButtonComponent::State::Disabled ? b.disabledColor
                            : b._state == UIButtonComponent::State::Pressed ? b.pressedColor
                            : b._state == UIButtonComponent::State::Hover ? b.hoverColor : b.normalColor;
        const float t = (!interactive || b.fadeDuration <= 0) ? 1.0f : wi::math::saturate(dt / b.fadeDuration);
        b._color = wi::math::Lerp(b._color, want, t);
        const Entity g = b.targetGraphic != INVALID_ENTITY ? b.targetGraphic : e;
        if (UIImageComponent* img = images.GetComponent(g)) img->_tint = b._color;
        else if (UITextComponent* txt = texts.GetComponent(g)) txt->_tint = b._color;
    }
}

void UISystem::Documents_ (Scene& scene, bool interactive) {
    auto& docs = Documents(scene);
    auto& rects = Rects(scene);
    std::unordered_set<Entity> alive;
    RmlHost& host = RmlHost::Get();
    if (docs.GetCount() > 0 && host.Ready()) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            host.SetEventCallback([this](Entity doc, const std::string& handler, const std::string& element,
                                         const std::string&) {
                if (scene_ == nullptr || !interactive_) return;
                const UIDocumentComponent* d = Documents(*scene_).GetComponent(doc);
                tb::Value args[2] = { EntityValue(doc), tb::Value::String(element.c_str()) };
                Dispatch(*scene_, UIEvent::Kind::Document, doc, handler, d ? d->eventTarget : INVALID_ENTITY,
                         element, args, 2);
            });
        }
    }
    for (size_t i = 0; i < docs.GetCount(); ++i) {
        const Entity e = docs.GetEntity(i);
        const UIRectComponent* r = rects.GetComponent(e);
        if (r == nullptr || !r->_visible) continue;
        alive.insert(e);
        const UICanvasComponent* c = Canvases(scene).GetComponent(r->_canvas);
        host.Sync(e, docs[i], XMINT2((int)std::lround(r->_pixels.z), (int)std::lround(r->_pixels.w)),
                  c ? c->_scale : 1.0f);
    }
    host.Retain(alive);
    if (alive.empty()) return;

    // The pointer goes to the document under it - or, mid-press, the one it was pressed in.
    Entity over = INVALID_ENTITY;
    if (interactive && pointer_.valid && hovered_ != INVALID_ENTITY && docs.Contains(hovered_) &&
        (docs.GetComponent(hovered_)->_flags & UIDocumentComponent::INTERACTIVE))
        over = hovered_;
    const Entity target = pressedDoc_ != INVALID_ENTITY ? pressedDoc_ : over;
    for (Entity e : alive)
        if (e != target) host.PointerLeave(e);
    if (target != INVALID_ENTITY) {
        const UIRectComponent* r = rects.GetComponent(target);
        host.PointerMove(target, XMFLOAT2(pointer_.pos.x - r->_pixels.x, pointer_.pos.y - r->_pixels.y));
        if (pointer_.wheel != 0) host.PointerWheel(target, pointer_.wheel);
        const bool pressedEdge = pointer_.down && pressedDoc_ == INVALID_ENTITY && over != INVALID_ENTITY;
        if (pressedEdge && pressed_ == INVALID_ENTITY) {
            pressedDoc_ = over;
            host.PointerButton(over, true);
        } else if (!pointer_.down && pressedDoc_ != INVALID_ENTITY) {
            host.PointerButton(pressedDoc_, false);
            pressedDoc_ = INVALID_ENTITY;
        }
    } else if (!pointer_.down) {
        pressedDoc_ = INVALID_ENTITY;
    }
    if (!interactive) host.ClearFocus();
    host.Update();
}

bool UISystem::WantsKeyboard () const { return interactive_ && RmlHost::Get().WantsKeyboard(); }

bool UISystem::HandleSdlEvent (const SDL_Event& event) {
    return interactive_ && RmlHost::Get().HandleSdlEvent(event);
}

Entity UISystem::Pick (Scene& scene, XMFLOAT2 p) const {
    auto& rects = Rects(scene);
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (worldElement_.count(*it) || Canvases(scene).Contains(*it)) continue;
        const UIRectComponent* r = rects.GetComponent(*it);
        if (r == nullptr || !r->_visible || !Inside(r->_pixels, r->_clip, p)) continue;
        // Only something you can SEE is picked: an invisible container (a layout with a
        //	transparent background) under a button would otherwise always win the click.
        const UIImageComponent* img = Images(scene).GetComponent(*it);
        const UITextComponent* txt = Texts(scene).GetComponent(*it);
        const bool seen = (img && img->color.w * r->_opacity > 0.02f) || (txt && !txt->text.empty()) ||
                          Documents(scene).Contains(*it) || Buttons(scene).Contains(*it) ||
                          Toggles(scene).Contains(*it) || Sliders(scene).Contains(*it);
        if (seen) return *it;
    }
    return INVALID_ENTITY;
}

// --------------------------------------------------------------------------- render ---

namespace {

void DrawImage (render::Draw2D& d, const UIImageComponent& img, const UIRectComponent& r) {
    XMFLOAT4 col = Mul(img.color, img._tint);
    col.w *= r._opacity;
    if (col.w <= 0.001f || r._pixels.z <= 0 || r._pixels.w <= 0) return;
    const render::TextureObject* tex = img._texture.get();

    XMFLOAT4 px = r._pixels;
    const float texW = tex ? (float)tex->width : 1.0f;
    const float texH = tex ? (float)tex->height : 1.0f;
    if (tex && (img._flags & UIImageComponent::PRESERVE_ASPECT)) {
        const float aspect = texW / texH;
        if (px.z / px.w > aspect) {
            const float w = px.w * aspect;
            px.x += (px.z - w) * r.pivot.x;
            px.z = w;
        } else {
            const float h = px.z / aspect;
            px.y += (px.w - h) * r.pivot.y;
            px.w = h;
        }
    }

    render::ImageDraw p;
    p.color = col;
    p.texture = tex;
    if (img.gradient != UIImageComponent::Gradient::None) {
        p.gradient = true;
        p.gradientFrom = XMFLOAT2(0, 0);
        p.gradientTo = img.gradient == UIImageComponent::Gradient::Horizontal ? XMFLOAT2(1, 0) : XMFLOAT2(0, 1);
        XMFLOAT4 g = Mul(img.gradientColor, img._tint);
        g.w *= r._opacity;
        p.gradientColor = g;
    }
    auto rounding = [&](render::ImageDraw& q, float w, float h) {
        if (img.cornerRadius <= 0) return;
        q.cornerRadius = std::min(img.cornerRadius * r._scale, std::min(w, h) * 0.5f);
    };

    if (img.type == UIImageComponent::Type::Sliced && tex &&
        (img.border.x > 0 || img.border.y > 0 || img.border.z > 0 || img.border.w > 0)) {
        // 9-slice: corners at texture size (x borderScale x canvas scale), edges stretch one way.
        const float k = img.borderScale * r._scale;
        const float L = std::min(img.border.x * k, px.z * 0.5f), R = std::min(img.border.z * k, px.z * 0.5f);
        const float T = std::min(img.border.y * k, px.w * 0.5f), B = std::min(img.border.w * k, px.w * 0.5f);
        const float xs[4] = { px.x, px.x + L, px.x + px.z - R, px.x + px.z };
        const float ys[4] = { px.y, px.y + T, px.y + px.w - B, px.y + px.w };
        const float us[4] = { 0, img.border.x, texW - img.border.z, texW };
        const float vs[4] = { 0, img.border.y, texH - img.border.w, texH };
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                const float w = xs[i + 1] - xs[i], h = ys[j + 1] - ys[j];
                if (w <= 0 || h <= 0) continue;
                render::ImageDraw q = p;
                q.rect = XMFLOAT4(xs[i], ys[j], w, h);
                q.useSource = true;
                q.source = XMFLOAT4(us[i], vs[j], us[i + 1] - us[i], vs[j + 1] - vs[j]);
                d.DrawImage(q);
            }
        }
        return;
    }

    if (img.type == UIImageComponent::Type::FilledHorizontal || img.type == UIImageComponent::Type::FilledVertical) {
        const float f = wi::math::saturate(img.fillAmount);
        if (f <= 0) return;
        const bool rev = img._flags & UIImageComponent::FILL_REVERSE;
        XMFLOAT4 rect(0, 0, texW, texH);
        if (img.type == UIImageComponent::Type::FilledHorizontal) {
            const float w = px.z * f;
            if (rev) { px.x += px.z - w; rect.x = texW * (1 - f); }
            px.z = w;
            rect.z = texW * f;
        } else {
            // Vertical fills from the bottom by default, like a gauge.
            const float h = px.w * f;
            if (!rev) { px.y += px.w - h; rect.y = texH * (1 - f); }
            px.w = h;
            rect.w = texH * f;
        }
        p.rect = px;
        if (tex) {
            p.useSource = true;
            p.source = rect;
        }
        rounding(p, px.z, px.w);
        d.DrawImage(p);
        return;
    }

    p.rect = px;
    p.pivot = r.pivot;
    p.rotation = r.rotation;
    rounding(p, px.z, px.w);
    d.DrawImage(p);
}

void DrawLabel (render::Draw2D& d, const UITextComponent& t, const UIRectComponent& r) {
    if (t.text.empty()) return;
    XMFLOAT4 col = Mul(t.color, t._tint);
    col.w *= r._opacity;
    if (col.w <= 0.001f) return;
    const XMFLOAT4& px = r._pixels;
    render::TextDraw fp;
    fp.text = t.text.c_str();
    fp.position = XMFLOAT2(px.x + px.z * AlignFactor(t.hAlign), px.y + px.w * AlignFactor(t.vAlign));
    fp.size = std::max(1, (int)std::lround(t.size * r._scale));
    fp.hAlign = (render::Align)t.hAlign;
    fp.vAlign = (render::Align)t.vAlign;
    fp.color = col;
    fp.font = t._style;
    fp.bolden = t.bolden;
    fp.softness = t.softness;
    fp.spacing = XMFLOAT2(t.spacing.x * r._scale, t.spacing.y * r._scale);
    fp.rotation = r.rotation;
    if (t._flags & UITextComponent::WRAP) fp.wrapWidth = px.z;
    if (t._flags & UITextComponent::SHADOW) {
        XMFLOAT4 s = t.shadowColor;
        s.w *= r._opacity;
        fp.shadowColor = s;
        fp.shadowOffset = XMFLOAT2(t.shadowOffset.x * r._scale, t.shadowOffset.y * r._scale);
    }
    d.DrawString(fp);
}

} // namespace

void UISystem::DrawElement (Scene& scene, Entity e, render::Draw2D& d) {
    const UIRectComponent* r = Rects(scene).GetComponent(e);
    if (r == nullptr || !r->_visible || r->_opacity <= 0.001f) return;
    if (r->_clip.z <= r->_clip.x || r->_clip.w <= r->_clip.y) return;
    d.SetScissor(r->_clip);
    if (const UIImageComponent* img = Images(scene).GetComponent(e)) DrawImage(d, *img, *r);
    if (Documents(scene).Contains(e)) {
        RmlHost::Get().Render(e, d, XMFLOAT2(r->_pixels.x, r->_pixels.y),
                              Intersect(r->_clip, XMFLOAT4(r->_pixels.x, r->_pixels.y,
                                                           r->_pixels.x + r->_pixels.z, r->_pixels.y + r->_pixels.w)));
        d.SetScissor(r->_clip);
    }
    if (const UITextComponent* t = Texts(scene).GetComponent(e)) DrawLabel(d, *t, *r);
}

void UISystem::RenderWorldCanvases (Scene& scene, render::Draw2D& d) {
    auto& canvases = Canvases(scene);
    std::unordered_set<Entity> live;
    for (Entity canvas : roots_) {
        const UICanvasComponent& c = *canvases.GetComponent(canvas);
        if (c.renderMode != UICanvasComponent::RenderMode::World || c.IsHidden()) continue;
        live.insert(canvas);
        const XMUINT2 size((uint32_t)std::max(1.0f, c.referenceResolution.x), (uint32_t)std::max(1.0f, c.referenceResolution.y));
        render::TextureRef& rt = worldTargets_[canvas];
        if (!rt || rt->width != size.x || rt->height != size.y) rt = d.CreateRenderTarget(size.x, size.y);
        if (!rt || !d.Begin(*rt, true, "st::ui world canvas")) continue;
        for (Entity e : order_) {
            const UIRectComponent* r = Rects(scene).GetComponent(e);
            if (r && r->_canvas == canvas) DrawElement(scene, e, d);
        }
        d.End();

        // Shown as the base colour of the canvas object's materials.
        if (const ObjectComponent* o = scene.objects.GetComponent(canvas)) {
            if (const MeshComponent* m = scene.meshes.GetComponent(o->meshID)) {
                for (const auto& subset : m->subsets) {
                    if (MaterialComponent* mat = scene.materials.GetComponent(subset.materialID)) {
                        mat->textures[MaterialComponent::BASECOLORMAP].resource = render::Get().ToResource(rt);
                        mat->SetDirty();
                    }
                }
            }
        }
    }
    for (auto it = worldTargets_.begin(); it != worldTargets_.end();)
        it = live.count(it->first) ? std::next(it) : worldTargets_.erase(it);
}

void UISystem::Render (Scene& scene, render::Draw2D& d, const render::TextureObject* target) {
    if (order_.empty()) return;
    RenderWorldCanvases(scene, d);
    if (target == nullptr) return;
    bool any = false;
    for (Entity e : order_) {
        if (!worldElement_.count(e)) {
            any = true;
            break;
        }
    }
    if (!any || !d.Begin(*target, false, "st::ui screen canvases")) return;
    for (Entity e : order_)
        if (!worldElement_.count(e)) DrawElement(scene, e, d);
    d.End();
}

void UISystem::Shutdown () {
    worldTargets_.clear();
    RmlHost::Get().Shutdown();
    listeners_.clear();
    scene_ = nullptr;
}

} // namespace st::ui
