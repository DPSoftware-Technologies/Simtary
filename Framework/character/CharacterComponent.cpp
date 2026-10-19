// "stCharacter" - a walking, running soldier on the engine's capsule character controller.
//
// Attach it to the ROOT of an imported character model (the GLB that Milistry/tools/character builds:
// a skinned body with actions "idle", "walk" and "run"), next to an "stInput" pointed at the "Player"
// map (Move, Look, Sprint, Jump):
//
//     NCI_0 = "stInput"       NCA_0_actionMap = "Player"
//     NCI_1 = "stCharacter"
//
// Start() adds the engine's wi::scene::CharacterComponent if the entity has none and finds the clips of
// THIS model (animations whose channels target a descendant). The controller itself - capsule, ground,
// slopes, gravity, facing, leaning - is the engine's; this component turns input into Move / Turn / Jump
// and picks the clip by speed, playing it faster or slower so the feet roughly match the ground.
//
// Conventions: the engine's controller makes the entity face -Z of its own space, so the model must face
// -Z too (the Blender script rotates the character 180 degrees before export to get that).
//
// Camera: with followCamera on it also owns the game camera - behind the character, right mouse orbits,
// the wheel zooms. Turn it off when something else (a chase camera, the editor) drives the view.

#include "stNativeComponent.h"
#include "input/InputComponent.h"
#include "io/PlayerPrefs.h"
#include "scene/GuiHooks.h"
#include "scene/PlayControl.h"
#include "terrain/TerrainSystem.h"

#include "imgui.h"
#include "wiBacklog.h"
#include "wiScene.h"

#include <algorithm>
#include <cmath>

using namespace wi::scene;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st {

struct StCharacter : NativeComponent {
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    float height = 1.75f;        // capsule height, m
    float radius = 0.28f;        // capsule radius, m
    float walkSpeed = 1.5f;      // m/s the walk clip is authored for
    float runSpeed = 3.7f;       // m/s the run clip is authored for
    float sprintSpeed = 5.0f;    // m/s at full tilt (run clip sped up)
    float jumpSpeed = 5.0f;      // m/s upwards
    float turnRate = 10.0f;      // controller turning, 1/s
    bool followCamera = true;
    float cameraDistance = 3.6f;
    float cameraHeight = 1.55f;  // look-at height above the feet
    float cameraPitch = 12.0f;   // degrees above the horizon
    std::string idleName = "idle", walkName = "walk", runName = "run";

    // Appearance: the player's choices, kept in the options store (PlayerPrefs "char.*") and applied to the
    // model's materials by name (mat_body, mat_top, mat_pants, mat_boots, mat_gloves - see
    // Milistry/tools/character). F4 opens the customiser. Later this becomes a record sent at spawn.
    static constexpr int kSlots = 5;
    XMFLOAT3 colour_[kSlots] = { XMFLOAT3(0.55f, 0.38f, 0.30f), XMFLOAT3(0.33f, 0.35f, 0.24f), XMFLOAT3(0.31f, 0.33f, 0.22f),
                                 XMFLOAT3(0.12f, 0.10f, 0.08f), XMFLOAT3(0.15f, 0.15f, 0.14f) };
    float heightScale_ = 1.0f;
    bool  customiserOpen_ = false;

    void Start () override {
        Bind(height, "height");
        Bind(radius, "radius");
        Bind(walkSpeed, "walkSpeed");
        Bind(runSpeed, "runSpeed");
        Bind(sprintSpeed, "sprintSpeed");
        Bind(jumpSpeed, "jumpSpeed");
        Bind(turnRate, "turnRate");
        Bind(followCamera, "followCamera");
        Bind(cameraDistance, "cameraDistance");
        Bind(cameraHeight, "cameraHeight");
        Bind(cameraPitch, "cameraPitch");
        Bind(idleName, "idleName");
        Bind(walkName, "walkName");
        Bind(runName, "runName");

        CharacterComponent* ch = scene->characters.GetComponent(entity);
        if (ch == nullptr) {
            ch = &scene->characters.Create(entity);
        }
        // A saved map restores the engine component with a default position: start where the entity IS.
        if (const TransformComponent* t = scene->transforms.GetComponent(entity)) ch->SetPosition(t->GetPosition());
        ch->width = radius;
        ch->height = height;
        ch->scale = 1.0f;
        ch->turning_speed = turnRate;
        ch->SetFootPlacementEnabled(false);   // no humanoid mapping on the imported rig yet

        // The controller's ground and wall checks test every navigation mesh / collider whose layer is not the
        // character's own. Without a layer of its own the soldier collided with ITS OWN body: an arm or knee
        // swinging into the capsule read as ground, and every step pushed it up (it flew away while running).
        // Put the entity and every mesh of the model on one dedicated layer bit; the world keeps all bits.
        constexpr uint32_t kCharacterLayer = 1u << 30;
        auto setLayer = [&](Entity e) {
            LayerComponent* l = scene->layers.GetComponent(e);
            if (l == nullptr) l = &scene->layers.Create(e);
            l->layerMask = kCharacterLayer;
        };
        setLayer(entity);
        for (size_t o = 0; o < scene->objects.GetCount(); ++o) {
            const Entity oe = scene->objects.GetEntity(o);
            if (scene->Entity_IsDescendant(oe, entity)) setLayer(oe);
        }

        idle_ = FindClip(idleName);
        walk_ = FindClip(walkName);
        run_ = FindClip(runName);
        for (Entity e : { idle_, walk_, run_ }) {
            if (e == INVALID_ENTITY) continue;
            ch->AddAnimation(e);
            if (AnimationComponent* a = scene->animations.GetComponent(e)) a->SetLooped(true);
        }
        if (idle_ == INVALID_ENTITY || walk_ == INVALID_ENTITY)
            wi::backlog::post("stCharacter: clips not found under this entity (idle " + std::to_string(idle_) +
                              ", walk " + std::to_string(walk_) + ", run " + std::to_string(run_) + ")", wi::backlog::LogLevel::Warning);
        LoadAppearance();
        ApplyAppearance();
        if (hook_ < 0) hook_ = st::GuiHooks::Get().Add([this] { DrawCustomiser(); });
        input_ = GetComponent<st::InputComponent>();
        if (input_ != nullptr) {
            idMove_ = input_->Find("Move");
            idSprint_ = input_->Find("Sprint");
            idJump_ = input_->Find("Jump");
        }
    }

    void Update (float dt) override {
        CharacterComponent* ch = scene->characters.GetComponent(entity);
        if (ch == nullptr || dt <= 0.0f) return;
        if (input_ == nullptr) input_ = GetComponent<st::InputComponent>();

        // The terrain only has chunks (and so ground) around the camera, and a character spawned before they
        // exist would fall for ever. Hold it where it was placed until there is ground under it, and put it
        // back if it ever drops out of the world.
        if (!holdInit_) {
            const TransformComponent* t = scene->transforms.GetComponent(entity);
            holdPos_ = t != nullptr ? t->GetPosition() : ch->GetPosition();
            holdInit_ = true;
        }
        // Authored terrain answers at once from its data, long before its chunk meshes (what the controller
        // collides with) are built around the camera: stand on that and go.
        st::terrain::GroundSample ground;
        if (!released_ && st::terrain::TerrainSystem::Get().Sample(holdPos_.x, holdPos_.z, ground)) {
            holdPos_.y = std::max(holdPos_.y, ground.height + 0.02f);
            ch->SetPosition(holdPos_);
            ch->SetVelocity(XMFLOAT3(0, 0, 0));
            ch->inertia = XMFLOAT3(0, 0, 0);
            released_ = true;
        }
        if (!released_ || ch->GetPosition().y < holdPos_.y - 150.0f) {
            // from well above: a spawn point just under the surface must still find it
            const wi::primitive::Ray down(XMFLOAT3(holdPos_.x, holdPos_.y + 60.0f, holdPos_.z), XMFLOAT3(0, -1, 0));
            const auto hit = scene->Intersects(down, wi::enums::FILTER_NAVIGATION_MESH | wi::enums::FILTER_COLLIDER);
            holdTime_ += dt;
            if (hit.entity != INVALID_ENTITY || holdTime_ > 30.0f) {
                if (hit.entity == INVALID_ENTITY)
                    wi::backlog::post("stCharacter: no ground under the character after 30 s - releasing it anyway", wi::backlog::LogLevel::Warning);
                released_ = true;
                if (hit.entity != INVALID_ENTITY && hit.position.y > holdPos_.y) {
                    holdPos_.y = hit.position.y + 0.1f;   // placed under the ground: stand on it
                    ch->SetPosition(holdPos_);
                }
            } else {
                released_ = false;
                ch->SetPosition(holdPos_);
                ch->SetVelocity(XMFLOAT3(0, 0, 0));
                if (followCamera) UpdateCamera(*ch, dt);   // the camera is what makes the chunks appear
                return;
            }
        }

        XMFLOAT2 move(0, 0);
        bool sprint = false;
        if (input_ != nullptr) {
            if (idMove_ < 0) {
                idMove_ = input_->Find("Move");
                idSprint_ = input_->Find("Sprint");
                idJump_ = input_->Find("Jump");
            }
            if (idMove_ >= 0) move = input_->Vector2(idMove_);
            sprint = idSprint_ >= 0 && input_->Value(idSprint_) > 0.5f;
            if (idJump_ >= 0 && input_->Pressed(idJump_) && (ch->IsGrounded() || onTerrain_)) ch->Jump(jumpSpeed);
        }

        // camera-relative direction on the ground plane
        const CameraComponent& cam = GetCamera();
        XMFLOAT3 fwd(cam.At.x, 0.0f, cam.At.z);
        float fl = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
        if (fl < 1e-4f) { fwd = XMFLOAT3(0, 0, 1); fl = 1.0f; }
        fwd.x /= fl; fwd.z /= fl;
        const XMFLOAT3 right(fwd.z, 0.0f, -fwd.x);
        XMFLOAT3 dir(fwd.x * move.y + right.x * move.x, 0.0f, fwd.z * move.y + right.z * move.x);
        const float mag = std::min(1.0f, std::sqrt(dir.x * dir.x + dir.z * dir.z));
        const bool moving = mag > 0.05f;

        // target speed: stick tilt picks walk .. run, sprint pushes past it
        float speed = 0.0f;
        if (moving) speed = sprint ? sprintSpeed : walkSpeed + (runSpeed - walkSpeed) * std::clamp((mag - 0.5f) * 2.0f, 0.0f, 1.0f);
        if (moving) {
            const float inv = 1.0f / std::max(1e-4f, std::sqrt(dir.x * dir.x + dir.z * dir.z));
            dir.x *= inv; dir.z *= inv;
            // steady speed of the controller = movement / (1 - ground_friction) per step
            const float push = speed * (1.0f - ch->ground_friction);
            ch->Move(XMFLOAT3(dir.x * push, 0.0f, dir.z * push));
            ch->Turn(dir);
        }

        // clip by the speed we actually move at, not the one we ask for
        const XMFLOAT3 v = ch->GetVelocity();
        const float actual = std::sqrt(v.x * v.x + v.z * v.z);
        Entity want = idle_;
        float rate = 1.0f;
        if (actual > 0.25f) {
            if (actual < (walkSpeed + runSpeed) * 0.5f || run_ == INVALID_ENTITY) {
                want = walk_;
                rate = std::clamp(actual / walkSpeed, 0.6f, 1.6f);
            } else {
                want = run_;
                rate = std::clamp(actual / runSpeed, 0.7f, 1.5f);
            }
        }
        if (want != INVALID_ENTITY && want != current_) {
            ch->PlayAnimation(want);
            current_ = want;
        }
        if (AnimationComponent* a = scene->animations.GetComponent(current_)) a->speed = rate;

        if (followCamera) UpdateCamera(*ch, dt);
    }

    // After the engine's own systems (character, physics, hierarchy) so nothing writes over it: the
    // controller owns position and facing, and this puts them on the entity.
    void LateUpdate (float dt) override {
        const CharacterComponent* ch = scene->characters.GetComponent(entity);
        TransformComponent* tr = scene->transforms.GetComponent(entity);
        if (ch == nullptr || tr == nullptr || !released_) return;
        // Never below the authored ground, even where its chunk mesh is not built yet.
        onTerrain_ = false;
        st::terrain::GroundSample ground;
        XMFLOAT3 p = ch->GetPosition();
        const bool overTerrain = st::terrain::TerrainSystem::Get().Sample(p.x, p.z, ground);
        if (overTerrain && p.y < ground.height + 1.0f) {
            // On (or just above) the authored terrain there is no moving platform under the character.
            scene->characters.GetComponent(entity)->inertia = XMFLOAT3(0, 0, 0);
        }
        if (overTerrain && p.y <= ground.height + 0.03f) {
            CharacterComponent* w = scene->characters.GetComponent(entity);
            XMFLOAT3 v = w->GetVelocity();
            if (p.y < ground.height) {
                p.y = ground.height;
                w->SetPosition(p);
            }
            if (v.y < 0.0f) {
                v.y = 0.0f;
                w->SetVelocity(v);
            }
            // Standing on the terrain data: no moving platform under it. The controller keeps the velocity of the
            // last thing it stood on as "inertia" until it touches ground again, and a terrain chunk that was just
            // placed reads as a fast platform - the soldier drifted up at 37 m/s for ever.
            w->inertia = XMFLOAT3(0, 0, 0);
            onTerrain_ = true;
        }
        if (CharacterComponent* w = scene->characters.GetComponent(entity)) {
            const XMFLOAT3 in = w->inertia;
            if (in.x * in.x + in.y * in.y + in.z * in.z > 100.0f) w->inertia = XMFLOAT3(0, 0, 0);   // > 10 m/s: not a platform
        }
        // GetPosition(), not the interpolated one: the interpolation reads a stale previous step while the
        // character stands still, which put it 0.3 m off the ground it stood on.
        tr->translation_local = ch->GetPosition();
        const XMFLOAT3 f = ch->GetFacingSmoothed();
        const float yaw = std::atan2(f.x, f.z) + 3.14159265f;   // the entity faces -Z when the controller faces +Z
        XMStoreFloat4(&tr->rotation_local, XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
        tr->scale_local = XMFLOAT3(heightScale_, heightScale_, heightScale_);
        // This fork keeps a 64-bit ABSOLUTE position as the truth and rebuilds translation_local from it, so
        // writing the local one alone is undone on the next update (the engine's own character write was too).
        tr->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
        tr->SetDirty();
        tr->UpdateTransform();
        (void)dt;
    }

    void Destroy () override {
        if (hook_ >= 0) st::GuiHooks::Get().Remove(hook_);
        hook_ = -1;
    }

    void DrawDebug () override {
        ImGui::DragFloat("Height", &height, 0.01f, 1.0f, 2.2f);
        ImGui::DragFloat("Radius", &radius, 0.01f, 0.1f, 0.6f);
        ImGui::DragFloat("Walk speed", &walkSpeed, 0.05f, 0.5f, 3.0f);
        ImGui::DragFloat("Run speed", &runSpeed, 0.05f, 2.0f, 8.0f);
        ImGui::Checkbox("Follow camera", &followCamera);
        if (const CharacterComponent* ch = scene->characters.GetComponent(entity)) {
            const XMFLOAT3 vel = ch->GetVelocity();
            ImGui::Text("grounded %d  speed %.2f m/s", ch->IsGrounded() ? 1 : 0, std::sqrt(vel.x * vel.x + vel.z * vel.z));
        }
        if (ImGui::Button("Save parameters")) SaveBoundParams();
    }

private:
    static const char* SlotName (int i) {
        static const char* names[kSlots] = { "skin", "top", "pants", "boots", "gloves" };
        return names[i];
    }
    static const char* MaterialName (int i) {
        static const char* names[kSlots] = { "mat_body", "mat_top", "mat_pants", "mat_boots", "mat_gloves" };
        return names[i];
    }

    void LoadAppearance () {
        for (int i = 0; i < kSlots; ++i) {
            const std::string k = std::string("char.") + SlotName(i);
            colour_[i].x = st::PlayerPrefs::GetFloat(k + ".r", colour_[i].x);
            colour_[i].y = st::PlayerPrefs::GetFloat(k + ".g", colour_[i].y);
            colour_[i].z = st::PlayerPrefs::GetFloat(k + ".b", colour_[i].z);
        }
        heightScale_ = st::PlayerPrefs::GetFloat("char.height", 1.0f);
    }

    void SaveAppearance () {
        for (int i = 0; i < kSlots; ++i) {
            const std::string k = std::string("char.") + SlotName(i);
            st::PlayerPrefs::SetFloat(k + ".r", colour_[i].x);
            st::PlayerPrefs::SetFloat(k + ".g", colour_[i].y);
            st::PlayerPrefs::SetFloat(k + ".b", colour_[i].z);
        }
        st::PlayerPrefs::SetFloat("char.height", heightScale_);
        st::PlayerPrefs::Save();
    }

    // Set the colour of every material of this model's meshes that carries a known part name.
    void ApplyAppearance () {
        for (size_t o = 0; o < scene->objects.GetCount(); ++o) {
            const Entity oe = scene->objects.GetEntity(o);
            if (oe != entity && !scene->Entity_IsDescendant(oe, entity)) continue;
            const MeshComponent* mesh = scene->meshes.GetComponent(scene->objects[o].meshID);
            if (mesh == nullptr) continue;
            for (const MeshComponent::MeshSubset& sub : mesh->subsets) {
                const NameComponent* n = scene->names.GetComponent(sub.materialID);
                MaterialComponent* mat = scene->materials.GetComponent(sub.materialID);
                if (n == nullptr || mat == nullptr) continue;
                for (int i = 0; i < kSlots; ++i) {
                    if (n->name == MaterialName(i)) mat->SetBaseColor(XMFLOAT4(colour_[i].x, colour_[i].y, colour_[i].z, 1.0f));
                }
            }
        }
        if (CharacterComponent* ch = scene->characters.GetComponent(entity)) ch->scale = heightScale_;
    }

    void DrawCustomiser () {
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false) && !ImGui::GetIO().WantTextInput) customiserOpen_ = !customiserOpen_;
        if (!customiserOpen_) return;
        ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Character (F4)", &customiserOpen_, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
        bool changed = false;
        for (int i = 0; i < kSlots; ++i) {
            static const char* labels[kSlots] = { "Skin", "Jacket", "Trousers", "Boots", "Gloves" };
            changed |= ImGui::ColorEdit3(labels[i], &colour_[i].x, ImGuiColorEditFlags_NoInputs);
        }
        changed |= ImGui::SliderFloat("Height", &heightScale_, 0.9f, 1.1f, "%.2f x");
        if (changed) ApplyAppearance();
        if (ImGui::Button("Save")) SaveAppearance();
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            StCharacter fresh;
            for (int i = 0; i < kSlots; ++i) colour_[i] = fresh.colour_[i];
            heightScale_ = 1.0f;
            ApplyAppearance();
        }
        ImGui::End();
    }

    // the clip with this name whose channels drive bones of THIS model
    Entity FindClip (const std::string& name) {
        for (size_t i = 0; i < scene->animations.GetCount(); ++i) {
            const Entity e = scene->animations.GetEntity(i);
            const NameComponent* n = scene->names.GetComponent(e);
            if (n == nullptr || n->name != name) continue;
            const AnimationComponent& a = scene->animations[i];
            for (const AnimationComponent::AnimationChannel& c : a.channels) {
                if (c.target != INVALID_ENTITY && (c.target == entity || scene->Entity_IsDescendant(c.target, entity))) return e;
            }
        }
        return INVALID_ENTITY;
    }

    void UpdateCamera (const CharacterComponent& ch, float dt) {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                yaw_ += io.MouseDelta.x * 0.006f;
                pitch_ = std::clamp(pitch_ - io.MouseDelta.y * 0.004f, -0.4f, 1.3f);
            }
            cameraDistance = std::clamp(cameraDistance - io.MouseWheel * 0.3f, 1.2f, 12.0f);
        }
        if (!camInit_) {
            const XMFLOAT3 f = ch.GetFacingSmoothed();
            yaw_ = std::atan2(f.x, f.z) + 3.14159265f;   // behind the way it faces
            pitch_ = cameraPitch * 0.0174533f;
            camInit_ = true;
        }
        const XMFLOAT3 p = ch.GetPosition();
        const XMFLOAT3 target(p.x, p.y + cameraHeight, p.z);
        const float cp = std::cos(pitch_);
        const XMFLOAT3 eye(target.x + std::sin(yaw_) * cp * cameraDistance, target.y + std::sin(pitch_) * cameraDistance,
                           target.z + std::cos(yaw_) * cp * cameraDistance);
        CameraComponent& cam = GetCamera();
        cam.Eye = eye;
        XMFLOAT3 at(target.x - eye.x, target.y - eye.y, target.z - eye.z);
        const float l = std::sqrt(at.x * at.x + at.y * at.y + at.z * at.z);
        cam.At = XMFLOAT3(at.x / l, at.y / l, at.z / l);
        cam.Up = XMFLOAT3(0, 1, 0);
        cam.SetDirty();
        (void)dt;
    }

    st::InputComponent* input_ = nullptr;
    int idMove_ = -1, idSprint_ = -1, idJump_ = -1;
    Entity idle_ = INVALID_ENTITY, walk_ = INVALID_ENTITY, run_ = INVALID_ENTITY, current_ = INVALID_ENTITY;
    float yaw_ = 0.0f, pitch_ = 0.2f;
    bool camInit_ = false;
    int hook_ = -1;
    XMFLOAT3 holdPos_ = XMFLOAT3(0, 0, 0);
    bool holdInit_ = false, released_ = false, onTerrain_ = false;
    float holdTime_ = 0.0f;
};

} // namespace st

using StCharacterComponent = st::StCharacter;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StCharacterComponent, "stCharacter", "Character")
