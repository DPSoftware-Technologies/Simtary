#include "imtemplatepreview.h"
#include "imhierarchy.h"
#include "imassets.h"         // SIMTARY_ASSET_PAYLOAD: templates dropped on the view
#include "scene/ObjectTemplate.h"
#include "scene/SceneSnapshot.h"

#include "wiScene.h"
#include "wiRenderer.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "imgui.h"
#include "imgui_internal.h"   // IsMouseDragPastThreshold(): click-vs-drag test on the release frame

#include <algorithm>
#include <cmath>
#include <cstdio>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st {

TemplatePreview::TemplatePreview (int id, SaveFn save, PlaceFn place)
    : id_(id), save_(std::move(save)), place_(std::move(place)) {
}

TemplatePreview::~TemplatePreview () {
    // The path's render targets and the scene's GPU buffers may still be in flight.
    if (path3D_ || scene_)
        wi::graphics::GetDevice()->WaitForGPU();
}

bool TemplatePreview::Open (const std::string& path, std::string* error) {
    ObjectTemplate tpl;
    if (!tpl.Load(path, error)) return false;

    path_ = tpl.Path();
    name_ = tpl.Name();
    // A copy, made the way a placement makes one, rather than the loaded prefab itself.
    scene_ = std::make_unique<Scene>();
    scene_->nativeComponents.lifecycleEnabled = false;
    root_ = tpl.Instantiate(*scene_, XMFLOAT3(0, 0, 0));
    if (root_ == INVALID_ENTITY) {
        if (error) *error = path + " has no root";
        scene_.reset();
        return false;
    }
    selected_ = root_;
    history_.Clear();
    savedRevision_ = history_.Revision();
    touched_ = false;
    needFrame_ = true;
    AddHelpers();

    if (!path3D_) {
        path3D_ = std::make_unique<wi::RenderPath3D>();
        path3D_->camera = &camera_;
        // Stepped by Render() with dt = 0, never by the path itself.
        path3D_->setSceneUpdateEnabled(false);
        path3D_->setOcclusionCullingEnabled(false);
        path3D_->setMSAASampleCount(1);
        path3D_->init(640, 480, 96.0f);
        path3D_->Start();
    }
    path3D_->scene = scene_.get();
    return true;
}

void TemplatePreview::AddHelpers () {
    // A sun and a sky, so the object is lit the way a scene would light it. Top-level and
    // outside the root: the save takes the root's subtree and nothing else.
    const Entity sun = scene_->Entity_CreateLight("Preview Sun", XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 1),
                                                  8.0f, 1000.0f, LightComponent::DIRECTIONAL);
    if (LightComponent* light = scene_->lights.GetComponent(sun)) {
        light->SetCastShadow(true);
        light->cascade_distances = { 8.0f, 30.0f, 100.0f };
    }
    if (TransformComponent* t = scene_->transforms.GetComponent(sun)) {
        // A directional light shines along its local +Y: 50 degrees up, from the front-left.
        t->RotateRollPitchYaw(XMFLOAT3(XM_PIDIV2 - 0.87f, 0.7f, 0.0f));
        t->UpdateTransform();
    }
    if (scene_->weathers.GetCount() == 0) {
        const Entity sky = wi::ecs::CreateEntity();
        scene_->names.Create(sky).name = "Preview Sky";
        // A plain gradient, not the realistic sky: that one is black below the horizon,
        //	and an object seen against it loses its silhouette.
        WeatherComponent& weather = scene_->weathers.Create(sky);
        weather.SetRealisticSky(false);
        weather.horizon = XMFLOAT3(0.42f, 0.44f, 0.47f);
        weather.zenith  = XMFLOAT3(0.20f, 0.27f, 0.36f);
        weather.ambient = XMFLOAT3(0.35f, 0.35f, 0.38f);
    }
}

bool TemplatePreview::IsDirty () const {
    return touched_ || history_.Revision() != savedRevision_;
}

void TemplatePreview::Save () {
    if (!scene_ || root_ == INVALID_ENTITY || !save_) return;
    if (save_(*scene_, root_, path_)) {
        savedRevision_ = history_.Revision();
        touched_ = false;
        if (const NameComponent* n = scene_->names.GetComponent(root_))
            if (!n->name.empty()) name_ = n->name;
        lastMessage_ = "saved";
    } else {
        lastMessage_ = "SAVE FAILED - see the backlog";
    }
}

void TemplatePreview::Revert () {
    // Reopen from disk, keeping the camera where it is.
    const XMFLOAT3 target = target_;
    const float yaw = yaw_, pitch = pitch_, distance = distance_;
    wi::graphics::GetDevice()->WaitForGPU();
    std::string error;
    if (!Open(path_, &error)) {
        lastMessage_ = "REVERT FAILED: " + error;
        return;
    }
    target_ = target;
    yaw_ = yaw;
    pitch_ = pitch;
    distance_ = distance;
    needFrame_ = false;
    lastMessage_ = "reverted";
}

void TemplatePreview::Frame (Entity focus) {
    if (!scene_) return;
    if (focus == wi::ecs::INVALID_ENTITY) focus = root_;
    wi::primitive::AABB box;
    bool any = false;
    for (size_t i = 0; i < scene_->objects.GetCount() && i < scene_->aabb_objects.size(); ++i) {
        const Entity e = scene_->objects.GetEntity(i);
        if (e != focus && !scene_->Entity_IsDescendant(e, focus)) continue;
        box = any ? wi::primitive::AABB::Merge(box, scene_->aabb_objects[i]) : scene_->aabb_objects[i];
        any = true;
    }
    if (any && box.IsValid()) {
        target_ = box.getCenter();
        const float radius = std::max(0.25f, box.getRadius());
        distance_ = radius / std::tan(fov_ * 0.5f) * 1.15f;
    } else if (const TransformComponent* t = scene_->transforms.GetComponent(focus)) {
        target_ = t->GetPosition();
        distance_ = 5.0f;
    }
}

bool TemplatePreview::PickPoint (float u, float v, XMFLOAT3& point, Entity* hit) const {
    if (!path3D_ || !scene_) return false;
    const long cx = (long)(u * path3D_->GetLogicalWidth());
    const long cy = (long)(v * path3D_->GetLogicalHeight());
    const wi::primitive::Ray ray = wi::renderer::GetPickRay(cx, cy, *path3D_, camera_);
    const PickResult result = wi::scene::Pick(ray, wi::enums::FILTER_OPAQUE | wi::enums::FILTER_TRANSPARENT,
                                              ~0u, *scene_);
    if (hit) *hit = result.entity;
    if (result.entity != INVALID_ENTITY) {
        point = result.position;
        return true;
    }
    // Missed the object: the ground plane under it, the grid the view draws.
    if (std::abs(ray.direction.y) > 1e-4f) {
        const float t = -ray.origin.y / ray.direction.y;
        if (t > 0.0f) {
            point = XMFLOAT3(ray.origin.x + ray.direction.x * t, 0.0f, ray.origin.z + ray.direction.z * t);
            return true;
        }
    }
    return false;
}

void TemplatePreview::DropTemplate (const std::string& path, const XMFLOAT3& point) {
    // Another template dropped INTO this one becomes part of it - copied in and flattened,
    // not linked: editing the inner template later does not reach this one.
    ObjectTemplate tpl;
    std::string error;
    if (!tpl.Load(path, &error)) {
        lastMessage_ = "drop failed: " + error;
        return;
    }
    wi::vector<Entity> created;
    const Entity inner = tpl.Instantiate(*scene_, point, XMFLOAT4(0, 0, 0, 1), &created);
    if (inner == INVALID_ENTITY) return;
    scene_->Component_Attach(inner, root_);
    history_.RecordCreatedMany(*scene_, created, "Add Template");
    selected_ = inner;
    lastMessage_ = "added " + tpl.Name();
}

void TemplatePreview::DrawView () {
    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool   usable = avail.x >= 16.0f && avail.y >= 16.0f;
    viewSize_ = XMUINT2((uint32_t)std::max(16.0f, avail.x), (uint32_t)std::max(16.0f, avail.y));
    live_ = usable && scene_ != nullptr;

    const wi::graphics::Texture* tex = path3D_ ? path3D_->GetLastPostprocessRT() : nullptr;
    if (!usable || tex == nullptr || !tex->IsValid()) {
        ImGui::Dummy(avail);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + 8, cursor.y + 8));
        ImGui::TextDisabled("waiting for the first frame...");
        return;
    }

    ImGui::Image((ImTextureID)(uintptr_t)tex, avail);

    // A template from the Resource Explorer, dropped on the view, is added to this one.
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::AcceptDragDropPayload(SIMTARY_ASSET_PAYLOAD, ImGuiDragDropFlags_AcceptPeekOnly);
        const bool isTemplate = peek && peek->DataSize == (int)sizeof(AssetPayload) &&
                                ((const AssetPayload*)peek->Data)->IsTemplate();
        if (isTemplate) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(SIMTARY_ASSET_PAYLOAD)) {
                const AssetPayload* a = (const AssetPayload*)p->Data;
                const ImVec2 m = ImGui::GetIO().MousePos;
                XMFLOAT3 point(0, 0, 0);
                PickPoint((m.x - cursor.x) / avail.x, (m.y - cursor.y) / avail.y, point, nullptr);
                DropTemplate(a->path, point);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Orbit camera: right or left drag orbits, middle drag pans, wheel zooms, F frames.
    //	Measured on the image rect - an invisible button would steal the drop target above.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                         ImGui::IsMouseHoveringRect(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y));
    ImGuiIO& io = ImGui::GetIO();
    if (hovered) {
        if (io.MouseWheel != 0.0f)
            distance_ = std::clamp(distance_ * std::pow(0.88f, io.MouseWheel), 0.05f, 5000.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.WantTextInput)
            Frame();
    }
    static bool dragging = false; // only one preview can own the mouse at a time
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
        dragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        dragging = false;
    if (dragging || (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
        const ImVec2 d = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            // Pan in the view plane, scaled so the point under the cursor stays under it.
            const XMMATRIX rot = XMMatrixRotationRollPitchYaw(pitch_, yaw_, 0.0f);
            const XMVECTOR right = XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rot);
            const XMVECTOR up    = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rot);
            const float scale = distance_ * std::tan(fov_ * 0.5f) * 2.0f / std::max(1.0f, avail.y);
            XMVECTOR t = XMLoadFloat3(&target_);
            t = XMVectorAdd(t, XMVectorScale(right, -d.x * scale));
            t = XMVectorAdd(t, XMVectorScale(up, d.y * scale));
            XMStoreFloat3(&target_, t);
        } else {
            yaw_  += d.x * 0.008f;
            pitch_ = std::clamp(pitch_ + d.y * 0.008f, -1.5f, 1.5f);
        }
    }

    // Click (not drag) selects what is under the cursor.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)) {
        const ImVec2 m = io.MousePos;
        XMFLOAT3 point;
        Entity hit = INVALID_ENTITY;
        PickPoint((m.x - cursor.x) / avail.x, (m.y - cursor.y) / avail.y, point, &hit);
        if (hit == INVALID_ENTITY || hit == root_ || scene_->Entity_IsDescendant(hit, root_))
            selected_ = hit;
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x + 8, cursor.y + 6));
    ImGui::TextDisabled("RMB orbit  MMB pan  wheel zoom  F frame  -  preview only, nothing runs");
}

void TemplatePreview::DrawCloseConfirm () {
    const std::string popup = "Unsaved template##tplclose" + std::to_string(id_);
    if (closeAsked_) {
        ImGui::OpenPopup(popup.c_str());
        closeAsked_ = false;
    }
    if (!ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%s has unsaved changes.", name_.c_str());
    if (ImGui::Button("Save and close")) {
        Save();
        if (!IsDirty()) open_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void TemplatePreview::StepFrozen () {
    // The preview's scene never runs; see StepSceneFrozen for why it is still stepped.
    StepSceneFrozen(*scene_, camera_);
}

void TemplatePreview::Draw () {
    if (!open_) return;
    // Stepped here, in the update half of the frame like the main scene, not from Render.
    if (scene_) StepFrozen();
    const bool dirty = IsDirty();
    const std::string title = "Template: " + name_ + (dirty ? " *" : "") +
                              "###simtary_template_" + std::to_string(id_);

    ImGui::SetNextWindowSize(ImVec2(1100, 620), ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    const bool visible = ImGui::Begin(title.c_str(), &keepOpen, ImGuiWindowFlags_MenuBar);
    focused_ = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    live_ = false;

    if (visible && scene_) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::MenuItem("Save", "Ctrl+S"))
                Save();
            if (ImGui::MenuItem("Revert", nullptr, false, dirty))
                Revert();
            if (ImGui::MenuItem("Frame", "F"))
                Frame();
            if (ImGui::MenuItem("Place in Scene", nullptr, false, place_ != nullptr && !dirty))
                place_(path_);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(dirty ? "Save first - a placed copy is made from the file."
                                        : "Place a copy in the open scene, in front of the editor camera.");
            ImGui::TextDisabled("%s", path_.c_str());
            if (!lastMessage_.empty())
                ImGui::TextDisabled("- %s", lastMessage_.c_str());
            ImGui::EndMenuBar();
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (focused_ && io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                Save();
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                const Entity e = io.KeyShift ? history_.Redo(*scene_) : history_.Undo(*scene_);
                if (e != INVALID_ENTITY) selected_ = e;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                const Entity e = history_.Redo(*scene_);
                if (e != INVALID_ENTITY) selected_ = e;
            }
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##template_layout", 3, flags)) {
            ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthFixed, 240.0f);
            ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Properties", ImGuiTableColumnFlags_WidthFixed, 340.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("##template_hierarchy");
            HierarchyGUI(*scene_, selected_, &history_, root_);
            const Entity framed = TakeHierarchyDoubleClick();
            if (framed != wi::ecs::INVALID_ENTITY) Frame(framed);
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("##template_view", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawView();
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginChild("##template_properties");
            PropertiesGUI(*scene_, selected_, &history_);
            // Not every inspector field records an undo step; any edit made here still
            // counts as unsaved.
            if (ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
                touched_ = true;
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }
    ImGui::End();

    if (!keepOpen) {
        if (IsDirty()) closeAsked_ = true;
        else           open_ = false;
    }
    DrawCloseConfirm();
}

void TemplatePreview::Render (float dt) {
    if (!open_ || !live_ || !scene_ || !path3D_) return;

    if (needFrame_) {
        Frame();
        needFrame_ = false;
    }

    path3D_->init(viewSize_.x, viewSize_.y, 96.0f);

    TransformComponent eye;
    const XMMATRIX rot = XMMatrixRotationRollPitchYaw(pitch_, yaw_, 0.0f);
    const XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rot);
    XMStoreFloat3(&eye.translation_local, XMVectorSubtract(XMLoadFloat3(&target_), XMVectorScale(forward, distance_)));
    eye.RotateRollPitchYaw(XMFLOAT3(pitch_, yaw_, 0.0f));
    eye.UpdateTransform();

    path3D_->PreUpdate();
    path3D_->Update(dt);   // resizes targets and writes camera width/height
    path3D_->PostUpdate();

    camera_.TransformCamera(eye);
    camera_.fov    = fov_;
    camera_.zNearP = std::max(0.01f, distance_ * 0.01f);
    camera_.zFarP  = std::max(100.0f, distance_ * 50.0f);
    camera_.UpdateCamera();


    // The ground grid and the selection box are debug draws: global switches, so set for
    // this one render and put back.
    const bool grid = wi::renderer::GetToDrawGridHelper();
    wi::renderer::SetToDrawGridHelper(true);
    if (selected_ != INVALID_ENTITY) {
        const size_t i = scene_->objects.GetIndex(selected_);
        if (i != wi::ecs::INVALID_INDEX && i < scene_->aabb_objects.size())
            wi::renderer::DrawBox(scene_->aabb_objects[i], XMFLOAT4(1.0f, 0.8f, 0.2f, 1.0f));
    }

    path3D_->PreRender();
    path3D_->Render();
    path3D_->PostRender();

    wi::renderer::SetToDrawGridHelper(grid);
}

} // namespace st
