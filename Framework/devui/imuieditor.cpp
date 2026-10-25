#include "imuieditor.h"

#include "imassets.h"
#include "imeditorhistory.h"
#include "imcomponentinspectors.h"
#include "ui/UISystem.h"
#include "ui/UIComponents.h"

#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st {

namespace {

constexpr float kHandleRadius = 5.0f;
constexpr ImU32 kSelect = IM_COL32(64, 160, 255, 255);
constexpr ImU32 kFaint = IM_COL32(255, 255, 255, 46);
constexpr ImU32 kAnchor = IM_COL32(255, 210, 80, 255);

bool IsScreenElement (Scene& scene, Entity e) {
    const ui::UIRectComponent* r = ui::Rects(scene).GetComponent(e);
    if (r == nullptr || !r->_visible || ui::Canvases(scene).Contains(e)) return false;
    const ui::UICanvasComponent* c = ui::Canvases(scene).GetComponent(r->_canvas);
    return c && c->renderMode == ui::UICanvasComponent::RenderMode::Screen;
}

std::string Extension (const char* path) {
    std::string p(path);
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = p.substr(dot + 1);
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext;
}

// A widget is the "whole" a click selects: a part inside it (the Text of a Button, the
//	Handle of a Slider) is only reached by double-click or Ctrl+click.
bool IsWidget (Scene& scene, Entity e) {
    return ui::Buttons(scene).Contains(e) || ui::Toggles(scene).Contains(e) ||
           ui::Sliders(scene).Contains(e) || ui::Documents(scene).Contains(e);
}

} // namespace

// What a click selects. The element under the pointer is usually a part of something,
//	so the click takes the nearest widget above it (itself when there is none). A part is
//	reached by drilling: double-click goes one level into the selection, Ctrl+click goes
//	straight to the deepest. A part reached that way stays selected while you click on it,
//	so it can be dragged; any other selection (the Hierarchy's, say) gives way to the widget.
Entity UIEditorPanel::PickForEdit (Scene& scene, Entity selected, XMFLOAT2 pixel, bool deepest, bool drill) {
    const Entity hit = ui::UISystem::Get().Pick(scene, pixel);
    if (hit == INVALID_ENTITY || deepest) {
        drilled_ = hit;
        return hit;
    }
    std::vector<Entity> chain;   // hit, its parent, ... (the canvas excluded)
    for (Entity e = hit; e != INVALID_ENTITY && chain.size() < 64;) {
        if (ui::Canvases(scene).Contains(e)) break;
        chain.push_back(e);
        const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
        e = h ? h->parentID : INVALID_ENTITY;
    }
    size_t group = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (IsWidget(scene, chain[i])) { group = i; break; }
    }
    const auto at = std::find(chain.begin(), chain.end(), selected);
    if (at != chain.end()) {
        const size_t level = (size_t)(at - chain.begin());
        if (drill && level > 0 && level <= group) {
            drilled_ = chain[level - 1];
            return drilled_;
        }
        if (level == group || (level < group && selected == drilled_)) return selected;
    }
    drilled_ = INVALID_ENTITY;
    return chain.empty() ? hit : chain[group];
}

ImVec2 UIEditorPanel::ToScreen (XMFLOAT2 p) const {
    return ImVec2(imagePos_.x + p.x * scale_, imagePos_.y + p.y * scale_);
}

XMFLOAT2 UIEditorPanel::ToPixel (ImVec2 s) const {
    return XMFLOAT2((s.x - imagePos_.x) / scale_, (s.y - imagePos_.y) / scale_);
}

// Handles: 0 tl, 1 t, 2 tr, 3 r, 4 br, 5 b, 6 bl, 7 l.
static void HandlePoints (const XMFLOAT4& px, XMFLOAT2 out[8]) {
    const float x0 = px.x, y0 = px.y, x1 = px.x + px.z, y1 = px.y + px.w;
    const float xm = (x0 + x1) * 0.5f, ym = (y0 + y1) * 0.5f;
    out[0] = XMFLOAT2(x0, y0); out[1] = XMFLOAT2(xm, y0); out[2] = XMFLOAT2(x1, y0); out[3] = XMFLOAT2(x1, ym);
    out[4] = XMFLOAT2(x1, y1); out[5] = XMFLOAT2(xm, y1); out[6] = XMFLOAT2(x0, y1); out[7] = XMFLOAT2(x0, ym);
}

int UIEditorPanel::HandleAt (Scene& scene, Entity selected, ImVec2 mouse) const {
    if (!IsScreenElement(scene, selected)) return -1;
    XMFLOAT2 pts[8];
    HandlePoints(ui::Rects(scene).GetComponent(selected)->_pixels, pts);
    for (int i = 0; i < 8; ++i) {
        const ImVec2 s = ToScreen(pts[i]);
        if (std::fabs(mouse.x - s.x) <= kHandleRadius + 2 && std::fabs(mouse.y - s.y) <= kHandleRadius + 2) return i;
    }
    return -1;
}

void UIEditorPanel::ApplyDrag (Scene& scene, Entity selected, XMFLOAT2 total) {
    ui::UIRectComponent* r = ui::Rects(scene).GetComponent(selected);
    if (r == nullptr) return;
    const ui::UICanvasComponent* c = ui::Canvases(scene).GetComponent(r->_canvas);
    const float s = c ? std::max(0.0001f, c->_scale) : 1.0f;
    // Scale inherited from above (kParent) and this element's own (kOwn), recovered from
    //	what the layout pass recorded: _scale = canvas scale x the average own scale.
    const float kOwn = std::max(0.0001f, r->_scale / s);
    const float ownAvg = std::max(0.0001f, (r->scale.x + r->scale.y) * 0.5f);
    const float kParent = kOwn / ownAvg;
    const float dx = total.x / s, dy = total.y / s;

    auto snap = [this](float v) { return snap_ && snapStep_ > 0 ? std::round(v / snapStep_) * snapStep_ : v; };
    if (drag_ == Drag::Move) {
        r->position.x = snap(startPosition_.x + dx / kParent);
        r->position.y = snap(startPosition_.y + dy / kParent);
        return;
    }
    if (drag_ != Drag::Resize) return;
    const bool left = handle_ == 0 || handle_ == 6 || handle_ == 7;
    const bool right = handle_ == 2 || handle_ == 3 || handle_ == 4;
    const bool top = handle_ == 0 || handle_ == 1 || handle_ == 2;
    const bool bottom = handle_ == 4 || handle_ == 5 || handle_ == 6;
    XMFLOAT2 size = startSize_, pos = startPosition_;
    if (right) { size.x += dx / kOwn; pos.x += r->pivot.x * dx / kParent; }
    if (left)  { size.x -= dx / kOwn; pos.x += (1 - r->pivot.x) * dx / kParent; }
    if (bottom) { size.y += dy / kOwn; pos.y += r->pivot.y * dy / kParent; }
    if (top)    { size.y -= dy / kOwn; pos.y += (1 - r->pivot.y) * dy / kParent; }
    r->size = XMFLOAT2(snap(size.x), snap(size.y));
    r->position = XMFLOAT2(snap(pos.x), snap(pos.y));
}

void UIEditorPanel::DrawOverlay (Scene& scene, Entity selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& rects = ui::Rects(scene);
    if (showRects_) {
        for (Entity e : ui::UISystem::Get().DrawOrder()) {
            if (e == selected || !IsScreenElement(scene, e)) continue;
            const XMFLOAT4& px = rects.GetComponent(e)->_pixels;
            dl->AddRect(ToScreen(XMFLOAT2(px.x, px.y)), ToScreen(XMFLOAT2(px.x + px.z, px.y + px.w)), kFaint);
        }
    }
    if (!IsScreenElement(scene, selected)) return;
    const ui::UIRectComponent& r = *rects.GetComponent(selected);
    const XMFLOAT4& px = r._pixels;

    // Anchors: four ticks on the parent rect where anchorMin/anchorMax put them.
    const HierarchyComponent* h = scene.hierarchy.GetComponent(selected);
    if (const ui::UIRectComponent* parent = h ? rects.GetComponent(h->parentID) : nullptr) {
        const XMFLOAT4& pp = parent->_pixels;
        dl->AddRect(ToScreen(XMFLOAT2(pp.x, pp.y)), ToScreen(XMFLOAT2(pp.x + pp.z, pp.y + pp.w)),
                    IM_COL32(255, 210, 80, 70));
        const float ax[2] = { pp.x + r.anchorMin.x * pp.z, pp.x + r.anchorMax.x * pp.z };
        const float ay[2] = { pp.y + r.anchorMin.y * pp.w, pp.y + r.anchorMax.y * pp.w };
        for (int iy = 0; iy < 2; ++iy) {
            for (int ix = 0; ix < 2; ++ix) {
                const ImVec2 a = ToScreen(XMFLOAT2(ax[ix], ay[iy]));
                // A small triangle pointing at the anchor from outside the element.
                const float sx = ix == 0 ? -1.0f : 1.0f, sy = iy == 0 ? -1.0f : 1.0f;
                dl->AddTriangleFilled(a, ImVec2(a.x + sx * 9, a.y + sy * 3), ImVec2(a.x + sx * 3, a.y + sy * 9), kAnchor);
            }
        }
    }

    const ImVec2 p0 = ToScreen(XMFLOAT2(px.x, px.y));
    const ImVec2 p1 = ToScreen(XMFLOAT2(px.x + px.z, px.y + px.w));
    dl->AddRect(p0, p1, kSelect, 0, 0, 2.0f);
    XMFLOAT2 pts[8];
    HandlePoints(px, pts);
    for (const XMFLOAT2& p : pts) {
        const ImVec2 s = ToScreen(p);
        dl->AddRectFilled(ImVec2(s.x - kHandleRadius, s.y - kHandleRadius), ImVec2(s.x + kHandleRadius, s.y + kHandleRadius),
                          IM_COL32(255, 255, 255, 255));
        dl->AddRect(ImVec2(s.x - kHandleRadius, s.y - kHandleRadius), ImVec2(s.x + kHandleRadius, s.y + kHandleRadius), kSelect);
    }
    const ImVec2 pivot = ToScreen(XMFLOAT2(px.x + r.pivot.x * px.z, px.y + r.pivot.y * px.w));
    dl->AddCircle(pivot, 5.0f, kSelect, 16, 2.0f);

    char label[160];
    std::snprintf(label, sizeof(label), "%s  %.0f x %.0f%s", devui::EntityLabel(scene, selected).c_str(),
                  r._rect.z, r._rect.w, r._laidOut ? "  (layout)" : "");
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 lp(p0.x, p0.y - ts.y - 6);
    dl->AddRectFilled(ImVec2(lp.x - 3, lp.y - 2), ImVec2(lp.x + ts.x + 3, lp.y + ts.y + 2), IM_COL32(20, 40, 70, 220), 3);
    dl->AddText(lp, IM_COL32(220, 235, 255, 255), label);
}

void UIEditorPanel::AcceptDrop (Scene& scene, Entity& selected, EditorHistory* history) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(st::SIMTARY_ASSET_PAYLOAD)) {
        if (p->DataSize == (int)sizeof(st::AssetPayload)) {
            const st::AssetPayload* a = (const st::AssetPayload*)p->Data;
            const std::string ext = Extension(a->path);
            const bool image = a->type == asset::AssetType::Texture || a->type == asset::AssetType::Image ||
                               ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "dds" || ext == "tga" ||
                               ext == "ktx2" || ext == "bmp";
            const bool doc = ext == "rml";
            if (image || doc) {
                const XMFLOAT2 drop = ToPixel(ImGui::GetIO().MousePos);
                Entity parent = ui::UISystem::Get().Pick(scene, drop);
                if (parent == INVALID_ENTITY) parent = ui::FindOrCreateScreenCanvas(scene, false);
                wi::vector<Entity> made;
                const Entity e = ui::Create(scene, doc ? ui::Preset::Document : ui::Preset::Image, parent, "", &made);
                if (image) {
                    ui::UIImageComponent* img = ui::Images(scene).GetComponent(e);
                    img->textureName = a->path;
                    img->_flags |= ui::UIImageComponent::PRESERVE_ASPECT;
                    ui::Rects(scene).GetComponent(e)->size = XMFLOAT2(200, 200);
                } else {
                    ui::Documents(scene).GetComponent(e)->source = a->path;
                }
                // At the drop point: anchored to the parent's centre, offset to where it fell.
                const HierarchyComponent* h = scene.hierarchy.GetComponent(e);
                const ui::UIRectComponent* pr = h ? ui::Rects(scene).GetComponent(h->parentID) : nullptr;
                const ui::UICanvasComponent* c = pr ? ui::Canvases(scene).GetComponent(pr->_canvas) : nullptr;
                const float s = c ? std::max(0.0001f, c->_scale) : 1.0f;
                if (pr && pr->_visible) {
                    ui::UIRectComponent* rw = ui::Rects(scene).GetComponent(e);
                    rw->position = XMFLOAT2((drop.x - (pr->_pixels.x + pr->_pixels.z * 0.5f)) / s,
                                            (drop.y - (pr->_pixels.y + pr->_pixels.w * 0.5f)) / s);
                }
                if (history) history->RecordCreatedMany(scene, made, image ? "Drop UI Image" : "Drop UI Document");
                selected = e;
            } else {
                wi::backlog::post(std::string("UI Editor: ") + a->path + " is not an image or an .rml page",
                                  wi::backlog::LogLevel::Warning);
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void UIEditorPanel::Draw (Scene& scene, Entity& selected, EditorHistory* history,
                          const render::TextureObject* frame, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(960, 600), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(kWindowName, open, ImGuiWindowFlags_MenuBar);
    visible_ = visible;
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::SmallButton("Fit")) {
            zoom_ = 0;
            pan_ = ImVec2(0, 0);
        }
        if (ImGui::SmallButton("100%")) {
            zoom_ = 1;
            pan_ = ImVec2(0, 0);
        }
        ImGui::TextDisabled("%.0f%%", scale_ * 100.0f);
        ImGui::Checkbox("Rects", &showRects_);
        ImGui::Checkbox("Snap", &snap_);
        if (snap_) {
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##snap", &snapStep_, 0.25f, 1.0f, 256.0f, "%.0f u");
        }
        if (ImGui::BeginMenu("Create")) {
            static const ui::Preset presets[] = {
                ui::Preset::Canvas, ui::Preset::Panel, ui::Preset::Image, ui::Preset::Text, ui::Preset::Button,
                ui::Preset::Toggle, ui::Preset::Slider, ui::Preset::VerticalLayout, ui::Preset::HorizontalLayout,
                ui::Preset::Grid, ui::Preset::Document,
            };
            for (ui::Preset preset : presets) {
                if (!ImGui::MenuItem(ui::PresetName(preset))) continue;
                wi::vector<Entity> made;
                const Entity parent = ui::Rects(scene).Contains(selected) ? selected : INVALID_ENTITY;
                const Entity e = ui::Create(scene, preset, parent, "", &made);
                const std::string what = std::string("Create UI ") + ui::PresetName(preset);
                if (history) history->RecordCreatedMany(scene, made, what.c_str());
                selected = e;
            }
            ImGui::EndMenu();
        }
        const XMUINT2 t = ui::UISystem::Get().TargetSize();
        ImGui::TextDisabled("  %u x %u - wheel zoom, middle drag pan, arrows nudge", t.x, t.y);
        ImGui::EndMenuBar();
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), IM_COL32(30, 31, 34, 255));
    if (frame == nullptr || avail.x < 16 || avail.y < 16) {
        ImGui::Dummy(avail);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + 8, cursor.y + 8));
        ImGui::TextDisabled("waiting for the game image...");
        ImGui::End();
        return;
    }

    const float W = (float)frame->width, H = (float)frame->height;
    const float fit = std::min(avail.x / W, avail.y / H);
    scale_ = zoom_ > 0 ? zoom_ : fit;
    const ImVec2 imgSize(W * scale_, H * scale_);
    imagePos_ = ImVec2(cursor.x + (avail.x - imgSize.x) * 0.5f + pan_.x, cursor.y + (avail.y - imgSize.y) * 0.5f + pan_.y);

    dl->PushClipRect(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), true);
    dl->AddImage((ImTextureID)render::Get().ImGuiTexture(*frame), imagePos_, ImVec2(imagePos_.x + imgSize.x, imagePos_.y + imgSize.y));
    dl->AddRect(imagePos_, ImVec2(imagePos_.x + imgSize.x, imagePos_.y + imgSize.y), IM_COL32(255, 255, 255, 60));

    // The panel owns its mouse: one invisible button over everything, so a click is ours
    //	and never starts dragging the window.
    ImGui::SetCursorScreenPos(cursor);
    ImGui::InvisibleButton("##ui_canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    AcceptDrop(scene, selected, history);
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;

    if (hovered && io.MouseWheel != 0) {
        const XMFLOAT2 under = ToPixel(mouse);
        const float next = std::clamp(scale_ * std::pow(1.15f, io.MouseWheel), 0.05f, 16.0f);
        const ImVec2 newSize(W * next, H * next);
        const ImVec2 wantPos(mouse.x - under.x * next, mouse.y - under.y * next);
        pan_ = ImVec2(wantPos.x - (cursor.x + (avail.x - newSize.x) * 0.5f),
                      wantPos.y - (cursor.y + (avail.y - newSize.y) * 0.5f));
        zoom_ = next;
    }

    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
        drag_ = Drag::Pan;
    else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        handle_ = HandleAt(scene, selected, mouse);
        Entity target = selected;
        if (handle_ < 0) {
            target = PickForEdit(scene, selected, ToPixel(mouse), io.KeyCtrl,
                                 ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            selected = target;
        }
        if (target != INVALID_ENTITY && ui::Rects(scene).Contains(target)) {
            const ui::UIRectComponent* r = ui::Rects(scene).GetComponent(target);
            drag_ = handle_ >= 0 ? Drag::Resize : Drag::Move;
            if (drag_ == Drag::Move && r->_laidOut) drag_ = Drag::None;   // its parent's layout places it
            dragStart_ = mouse;
            startPosition_ = r->position;
            startSize_ = r->size;
            dragMoved_ = false;
            if (drag_ != Drag::None && history) history->BeginEntity(scene, target, drag_ == Drag::Move ? "Move UI Element" : "Resize UI Element");
        }
    }

    if (drag_ == Drag::Pan) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
        if (zoom_ == 0) zoom_ = scale_;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    } else if (drag_ == Drag::Move || drag_ == Drag::Resize) {
        const XMFLOAT2 total((mouse.x - dragStart_.x) / scale_, (mouse.y - dragStart_.y) / scale_);
        if (total.x != 0 || total.y != 0) dragMoved_ = true;
        if (dragMoved_) ApplyDrag(scene, selected, total);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (history) {
                if (dragMoved_) history->Commit(scene);
                else history->Abort();
            }
            drag_ = Drag::None;
        }
    }

    // Arrow-key nudge, one undo step per press.
    if (ImGui::IsWindowFocused() && !io.WantTextInput && drag_ == Drag::None && IsScreenElement(scene, selected)) {
        const float step = io.KeyShift ? 10.0f : 1.0f;
        XMFLOAT2 d(0, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.y += step;
        if ((d.x != 0 || d.y != 0) && !ui::Rects(scene).GetComponent(selected)->_laidOut) {
            if (history) history->BeginEntity(scene, selected, "Nudge UI Element");
            ui::UIRectComponent* r = ui::Rects(scene).GetComponent(selected);
            r->position.x += d.x;
            r->position.y += d.y;
            if (history) history->Commit(scene);
        }
    }

    DrawOverlay(scene, selected);
    if (hovered && drag_ == Drag::None) {
        const int h = HandleAt(scene, selected, mouse);
        if (h == 1 || h == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        else if (h == 3 || h == 7) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (h == 0 || h == 4) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        else if (h == 2 || h == 6) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    }
    dl->PopClipRect();
    ImGui::End();
}

} // namespace st
