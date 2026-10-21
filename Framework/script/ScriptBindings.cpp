// ScriptBindings.cpp - the engine API Taipan scripts see, in Unity's vocabulary.
//
//	GameObject   name, transform, GetComponent("Rigidbody"), AddComponent(...),
//	             GameObject.Find, GameObject.CreatePrimitive; Instantiate(), Destroy()
//	Transform    position / localPosition, rotation / localRotation, eulerAngles,
//	             localScale, forward / right / up, parent, Translate, Rotate, RotateAround,
//	             LookAt, SetParent, GetChild, TransformPoint, InverseTransformPoint, ...
//	Rigidbody    velocity, angularVelocity, position, rotation, mass, isKinematic, drag,
//	             AddForce(f, ForceMode.Impulse), AddForceAtPosition, AddRelativeForce/Torque, ...
//	Light        color, intensity, range, spotAngle, shadows
//	Component    any native component by its NCI name: enabled + its NCA_ parameters
//	Time         deltaTime, time, fixedDeltaTime, unscaledDeltaTime, frameCount
//	Input        GetKey/Down/Up("space"), GetMouseButton*, GetAxis("Horizontal"),
//	             GetButton("Jump") - the game's action maps win over the defaults -
//	             mousePosition, mouseScrollDelta
//	Debug        Log / LogWarning / LogError(anything), DrawLine, DrawRay
//	Physics      Raycast(origin, direction, maxDistance) -> RaycastHit?
// Vector2/3, Quaternion, Color, Mathf, Space and ForceMode come from the bridge
// (libs/TaipanBridge/StTaipanMath.cpp).
//
// The 0.1 names (Entity, scene.find, log.info, self.translate...) are still registered,
// so scripts written against them keep running.
//
// Handles. Every class here is an entity id plus a kind tag in the top byte, so a
// GameObject and its Transform are different handles (Taipan caches handles by pointer)
// and every call re-resolves the id against the current scene: a removed entity is a
// catchable script error, never a dangling pointer. Components of C++ native classes
// are their instance pointer, checked against the scene's instance table on each use.
#include "script/ScriptSystem.h"
#include "script/NetBindings.h"
#include "scene/TemplatePool.h"
#include "ui/UISystem.h"
#include "ui/UIRml.h"
#include "scene/PlayControl.h"

#include "input/InputComponent.h"
#include "input/InputSystem.h"
#include "stApp.h"
#include "stNativeComponent.h"
#include "wiBacklog.h"
#include "wiInput.h"
#include "wiPhysics.h"
#include "render/api/Renderer.h"
#include "wiScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace st {

namespace {

namespace tb = st::taipan;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

// ---------------------------------------------------------------------------
// handles
// ---------------------------------------------------------------------------
enum class Kind : uint64_t { GameObject = 0, Transform = 1, Rigidbody = 2, Light = 3 };
constexpr uint64_t kIdMask = (1ull << 56) - 1;

const char* const kGameObject = "GameObject";
const char* const kTransform = "Transform";
const char* const kRigidbody = "Rigidbody";
const char* const kLight = "Light";
const char* const kComponent = "Component";

void* Handle (Kind k, Entity e) {
    return reinterpret_cast<void*>((static_cast<uint64_t>(k) << 56) | (e & kIdMask));
}
Entity EntityOf (void* self) {
    return static_cast<Entity>(reinterpret_cast<uintptr_t>(self) & kIdMask);
}
tb::Value ObjectValue (Kind k, Entity e) {
    static const char* names[] = { kGameObject, kTransform, kRigidbody, kLight };
    return e == INVALID_ENTITY ? tb::Value::Null() : tb::Value::Object(names[(int)k], Handle(k, e));
}

wi::scene::Scene& SceneRef () { return wi::scene::GetScene(); }

bool Exists (Entity e) {
    wi::scene::Scene& s = SceneRef();
    return e != INVALID_ENTITY && (s.transforms.Contains(e) || s.names.Contains(e));
}

void FailMissing (tb::Call* call, Entity e, const char* what) {
    std::string msg = "entity " + std::to_string(e) + (Exists(e) ? " has no " : " was destroyed (no ") + what +
                      (Exists(e) ? "" : ")");
    tb::Fail(call, msg.c_str());
}

wi::scene::TransformComponent* TransformOf (tb::Call* call, void* self) {
    const Entity e = EntityOf(self);
    wi::scene::TransformComponent* t = SceneRef().transforms.GetComponent(e);
    if (!t) FailMissing(call, e, "Transform");
    return t;
}

wi::scene::RigidBodyPhysicsComponent* RigidbodyOf (tb::Call* call, void* self) {
    const Entity e = EntityOf(self);
    wi::scene::RigidBodyPhysicsComponent* r = SceneRef().rigidbodies.GetComponent(e);
    if (!r) FailMissing(call, e, "Rigidbody");
    return r;
}

wi::scene::LightComponent* LightOf (tb::Call* call, void* self) {
    const Entity e = EntityOf(self);
    wi::scene::LightComponent* l = SceneRef().lights.GetComponent(e);
    if (!l) FailMissing(call, e, "Light");
    return l;
}

// Native components: the instance pointer IS the handle; it is only trusted after being
//	found in the scene's instance table again.
std::unordered_map<void*, Entity> componentOwners;

wi::scene::NativeComponent* ComponentOf (tb::Call* call, void* self) {
    auto it = componentOwners.find(self);
    if (it != componentOwners.end()) {
        auto& table = SceneRef().nativeComponents.instances;
        auto inst = table.find(it->second);
        if (inst != table.end())
            for (auto& i : inst->second)
                if (i.component.get() == self) return i.component.get();
    }
    tb::Fail(call, "this component was removed");
    return nullptr;
}

// ---------------------------------------------------------------------------
// conversions
// ---------------------------------------------------------------------------
tb::Vec3 V (const XMFLOAT3& v) { return tb::Vec3{ v.x, v.y, v.z }; }
XMFLOAT3 X (const tb::Vec3& v) { return XMFLOAT3(v.x, v.y, v.z); }
tb::Quat Q (const XMFLOAT4& q) { return tb::Quat{ q.x, q.y, q.z, q.w }; }
XMFLOAT4 X (const tb::Quat& q) { return XMFLOAT4(q.x, q.y, q.z, q.w); }

// ---------------------------------------------------------------------------
// transform math: world <-> local through the parent
// ---------------------------------------------------------------------------
Entity ParentOf (Entity e) {
    const wi::scene::HierarchyComponent* h = SceneRef().hierarchy.GetComponent(e);
    return h ? h->parentID : INVALID_ENTITY;
}

wi::scene::TransformComponent* ParentTransform (Entity e) {
    const Entity p = ParentOf(e);
    return p == INVALID_ENTITY ? nullptr : SceneRef().transforms.GetComponent(p);
}

// Rebuild this transform's world matrix NOW, so a read right after a write sees it.
//	(Its children catch up in the scene's own transform pass.)
void Refresh (Entity e, wi::scene::TransformComponent& t) {
    if (wi::scene::TransformComponent* p = ParentTransform(e)) t.UpdateTransform_Parented(*p);
    else t.UpdateTransform();
}

XMFLOAT3 WorldPosition (const wi::scene::TransformComponent& t) { return t.GetPosition(); }

XMFLOAT4 WorldRotation (const wi::scene::TransformComponent& t) {
    XMVECTOR s, r, p;
    XMMatrixDecompose(&s, &r, &p, XMLoadFloat4x4(&t.world));
    XMFLOAT4 q;
    XMStoreFloat4(&q, r);
    return q;
}

XMFLOAT3 WorldScale (const wi::scene::TransformComponent& t) {
    XMVECTOR s, r, p;
    XMMatrixDecompose(&s, &r, &p, XMLoadFloat4x4(&t.world));
    XMFLOAT3 v;
    XMStoreFloat3(&v, s);
    return v;
}

void SetLocalPosition (Entity e, wi::scene::TransformComponent& t, const XMFLOAT3& local) {
    // As a Translate by the difference: the large-world bookkeeping inside
    //	TransformComponent stays the mutator's job.
    t.Translate(XMFLOAT3(local.x - t.translation_local.x, local.y - t.translation_local.y,
                         local.z - t.translation_local.z));
    Refresh(e, t);
}

void SetWorldPosition (Entity e, wi::scene::TransformComponent& t, const XMFLOAT3& world) {
    XMFLOAT3 local = world;
    if (wi::scene::TransformComponent* p = ParentTransform(e)) {
        const XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&p->world));
        XMStoreFloat3(&local, XMVector3Transform(XMLoadFloat3(&world), inv));
    }
    SetLocalPosition(e, t, local);
}

void SetLocalRotation (Entity e, wi::scene::TransformComponent& t, const XMFLOAT4& q) {
    XMStoreFloat4(&t.rotation_local, XMQuaternionNormalize(XMLoadFloat4(&q)));
    t.SetDirty();
    Refresh(e, t);
}

void SetWorldRotation (Entity e, wi::scene::TransformComponent& t, const XMFLOAT4& world) {
    XMFLOAT4 local = world;
    if (wi::scene::TransformComponent* p = ParentTransform(e)) {
        // world = parent * local  =>  local = parent^-1 * world
        const tb::Quat pinv = Q([&] {
            XMFLOAT4 pr = WorldRotation(*p);
            XMFLOAT4 out;
            XMStoreFloat4(&out, XMQuaternionInverse(XMLoadFloat4(&pr)));
            return out;
        }());
        local = X(tb::QuaternionMultiply(pinv, Q(world)));
    }
    SetLocalRotation(e, t, local);
}

// ---------------------------------------------------------------------------
// GameObject
// ---------------------------------------------------------------------------
void GoGetName (tb::Call* call, void* self, void*) {
    const wi::scene::NameComponent* n = SceneRef().names.GetComponent(EntityOf(self));
    tb::ReturnString(call, n ? n->name.c_str() : "");
}

void GoSetName (tb::Call* call, void* self, void*) {
    const char* value = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    const Entity e = EntityOf(self);
    if (!Exists(e)) { tb::Fail(call, "cannot rename a destroyed GameObject"); return; }
    wi::scene::NameComponent* n = SceneRef().names.GetComponent(e);
    if (!n) n = &SceneRef().names.Create(e);
    n->name = value;
}

void GoGetId (tb::Call* call, void* self, void*) { tb::ReturnInt(call, (int64_t)EntityOf(self)); }

void GoGetTransform (tb::Call* call, void* self, void*) {
    const Entity e = EntityOf(self);
    if (!SceneRef().transforms.Contains(e)) { FailMissing(call, e, "Transform"); return; }
    tb::Return(call, ObjectValue(Kind::Transform, e));
}

void GoIsValid (tb::Call* call, void* self, void*) { tb::ReturnBool(call, Exists(EntityOf(self))); }

// GetComponent("Rigidbody") -> any: typed at the use site, Unity-style:
//	Rigidbody rb = self.GetComponent("Rigidbody")
tb::Value FindComponent (Entity e, const std::string& type) {
    wi::scene::Scene& s = SceneRef();
    if (type == kTransform && s.transforms.Contains(e)) return ObjectValue(Kind::Transform, e);
    if (type == kRigidbody && s.rigidbodies.Contains(e)) return ObjectValue(Kind::Rigidbody, e);
    if (type == kLight && s.lights.Contains(e)) return ObjectValue(Kind::Light, e);
    if (type == kGameObject && Exists(e)) return ObjectValue(Kind::GameObject, e);
    auto inst = s.nativeComponents.instances.find(e);
    if (inst != s.nativeComponents.instances.end()) {
        for (auto& i : inst->second) {
            if (i.name == type && i.component) {
                componentOwners[i.component.get()] = e;
                return tb::Value::Object(kComponent, i.component.get());
            }
        }
    }
    return tb::Value::Null();
}

void GoGetComponent (tb::Call* call, void* self, void*) {
    const std::string type = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    tb::Return(call, FindComponent(EntityOf(self), type));
}

void GoAddComponent (tb::Call* call, void* self, void*) {
    const std::string type = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    const Entity e = EntityOf(self);
    if (!Exists(e)) { tb::Fail(call, "cannot add a component to a destroyed GameObject"); return; }
    wi::scene::Scene& s = SceneRef();
    if (type == kRigidbody) {
        if (!s.rigidbodies.Contains(e)) s.rigidbodies.Create(e);
    } else if (type == kLight) {
        if (!s.lights.Contains(e)) s.lights.Create(e);
    } else if (wi::scene::AttachNativeComponent(s, e, type) < 0) {
        tb::Fail(call, ("no component named '" + type + "'").c_str());
        return;
    } else {
        // A native component is built on the next reconcile, so there is no instance yet.
        tb::Return(call, tb::Value::Null());
        return;
    }
    tb::Return(call, FindComponent(e, type));
}

void GoFind (tb::Call* call, void*) {
    const Entity e = SceneRef().Entity_FindByName(tb::ArgString(call, 0));
    tb::Return(call, ObjectValue(Kind::GameObject, e));
}

void GoCreatePrimitive (tb::Call* call, void*) {
    const std::string kind = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    wi::scene::Scene& s = SceneRef();
    Entity e = INVALID_ENTITY;
    if (kind == "Cube")        e = s.Entity_CreateCube(kind);
    else if (kind == "Sphere") e = s.Entity_CreateSphere(kind);
    else if (kind == "Plane")  e = s.Entity_CreatePlane(kind);
    else { tb::Fail(call, ("unknown primitive '" + kind + "' (Cube, Sphere, Plane)").c_str()); return; }
    tb::Return(call, ObjectValue(Kind::GameObject, e));
}

void Instantiate (tb::Call* call, void*) {
    const Entity original = EntityOf(tb::ArgObject(call, 0, kGameObject));
    if (tb::Failed(call)) return;
    if (!Exists(original)) { tb::Fail(call, "Instantiate: the original was destroyed"); return; }
    wi::scene::Scene& s = SceneRef();
    const Entity copy = s.Entity_Duplicate(original);
    if (wi::scene::TransformComponent* t = s.transforms.GetComponent(copy)) {
        if (tb::ArgCount(call) > 1 && tb::ArgType(call, 1) == tb::Type::Vector3)
            SetWorldPosition(copy, *t, X(tb::ArgVector3(call, 1)));
        if (tb::ArgCount(call) > 2 && tb::ArgType(call, 2) == tb::Type::Quaternion)
            SetWorldRotation(copy, *t, X(tb::ArgQuaternion(call, 2)));
    }
    tb::Return(call, ObjectValue(Kind::GameObject, copy));
}

void Destroy (tb::Call* call, void*) {
    void* h = tb::ArgObject(call, 0, kGameObject);
    if (tb::Failed(call) || !h) return;
    const Entity e = EntityOf(h);
    if (Exists(e)) SceneRef().Entity_Remove(e); // deferred by the scene while components run
}

// ---------------------------------------------------------------------------
// Pool - st::TemplatePool by template path ("templates/bullet.sttd")
// ---------------------------------------------------------------------------
st::TemplatePool* PoolArg (tb::Call* call) {
    const std::string path = tb::ArgString(call, 0);
    if (tb::Failed(call)) return nullptr;
    std::string error;
    st::TemplatePool* pool = st::TemplatePools::Get().Acquire(path, &error);
    if (pool == nullptr) tb::Fail(call, ("Pool: " + error).c_str());
    return pool;
}

void PoolPrewarm (tb::Call* call, void*) {
    st::TemplatePool* pool = PoolArg(call);
    if (pool == nullptr) return;
    const int64_t count = tb::ArgInt(call, 1);
    if (tb::Failed(call)) return;
    tb::ReturnInt(call, pool->Prewarm(SceneRef(), (uint32_t)std::max<int64_t>(0, count)));
}

void PoolSpawn (tb::Call* call, void*) {
    st::TemplatePool* pool = PoolArg(call);
    if (pool == nullptr) return;
    XMFLOAT3 position(0, 0, 0);
    XMFLOAT4 rotation(0, 0, 0, 1);
    if (tb::ArgCount(call) > 1 && tb::ArgType(call, 1) == tb::Type::Vector3) position = X(tb::ArgVector3(call, 1));
    if (tb::ArgCount(call) > 2 && tb::ArgType(call, 2) == tb::Type::Quaternion) rotation = X(tb::ArgQuaternion(call, 2));
    tb::Return(call, ObjectValue(Kind::GameObject, pool->Spawn(SceneRef(), position, rotation)));
}

void PoolRelease (tb::Call* call, void*) {
    void* h = tb::ArgObject(call, 0, kGameObject);
    if (tb::Failed(call) || !h) { tb::ReturnBool(call, false); return; }
    const Entity e = EntityOf(h);
    st::TemplatePool* pool = st::TemplatePools::Get().Owner(e);
    tb::ReturnBool(call, pool != nullptr && pool->Release(SceneRef(), e));
}

void PoolActive (tb::Call* call, void*) {
    st::TemplatePool* pool = PoolArg(call);
    if (pool == nullptr) return;
    tb::ReturnInt(call, pool->Stats().active);
}

void PoolParked (tb::Call* call, void*) {
    st::TemplatePool* pool = PoolArg(call);
    if (pool == nullptr) return;
    tb::ReturnInt(call, pool->Stats().parked);
}

// ---------------------------------------------------------------------------
// UI (Framework/ui) - elements are GameObjects; the component decides what applies.
// ---------------------------------------------------------------------------

Entity UIArg (tb::Call* call) {
    void* h = tb::ArgObject(call, 0, kGameObject);
    return h ? EntityOf(h) : INVALID_ENTITY;
}

void UISetText (tb::Call* call, void*) {
    st::ui::UITextComponent* t = st::ui::Texts(SceneRef()).GetComponent(UIArg(call));
    if (t) t->text = tb::ArgText(call, 1);
    tb::ReturnBool(call, t != nullptr);
}

void UIGetText (tb::Call* call, void*) {
    const st::ui::UITextComponent* t = st::ui::Texts(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnString(call, t ? t->text.c_str() : "");
}

void UISetVisible (tb::Call* call, void*) {
    st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    if (r) r->SetHidden(!tb::ArgBool(call, 1));
    tb::ReturnBool(call, r != nullptr);
}

void UIIsVisible (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnBool(call, r && r->_visible);
}

void UISetColor (tb::Call* call, void*) {
    const tb::Rgba c = tb::ArgColor(call, 1);
    const XMFLOAT4 color(c.r, c.g, c.b, c.a);
    const Entity e = UIArg(call);
    bool any = false;
    if (st::ui::UIImageComponent* i = st::ui::Images(SceneRef()).GetComponent(e)) { i->color = color; any = true; }
    else if (st::ui::UITextComponent* t = st::ui::Texts(SceneRef()).GetComponent(e)) { t->color = color; any = true; }
    tb::ReturnBool(call, any);
}

void UISetFill (tb::Call* call, void*) {
    st::ui::UIImageComponent* i = st::ui::Images(SceneRef()).GetComponent(UIArg(call));
    if (i) i->fillAmount = (float)tb::ArgFloat(call, 1);
    tb::ReturnBool(call, i != nullptr);
}

void UISetOpacity (tb::Call* call, void*) {
    st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    if (r) r->opacity = (float)tb::ArgFloat(call, 1);
    tb::ReturnBool(call, r != nullptr);
}

void UISetPosition (tb::Call* call, void*) {
    st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    if (r) r->position = XMFLOAT2((float)tb::ArgFloat(call, 1), (float)tb::ArgFloat(call, 2));
    tb::ReturnBool(call, r != nullptr);
}

void UISetSize (tb::Call* call, void*) {
    st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    if (r) r->size = XMFLOAT2((float)tb::ArgFloat(call, 1), (float)tb::ArgFloat(call, 2));
    tb::ReturnBool(call, r != nullptr);
}

void UIGetValue (tb::Call* call, void*) {
    const st::ui::UISliderComponent* s = st::ui::Sliders(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnFloat(call, s ? s->value : 0.0);
}

void UISetValue (tb::Call* call, void*) {
    st::ui::UISliderComponent* s = st::ui::Sliders(SceneRef()).GetComponent(UIArg(call));
    if (s) s->value = std::clamp((float)tb::ArgFloat(call, 1), std::min(s->minValue, s->maxValue),
                                 std::max(s->minValue, s->maxValue));
    tb::ReturnBool(call, s != nullptr);
}

void UIIsOn (tb::Call* call, void*) {
    const st::ui::UIToggleComponent* t = st::ui::Toggles(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnBool(call, t && t->IsOn());
}

void UISetOn (tb::Call* call, void*) {
    st::ui::UIToggleComponent* t = st::ui::Toggles(SceneRef()).GetComponent(UIArg(call));
    if (t) t->SetOn(tb::ArgBool(call, 1));
    tb::ReturnBool(call, t != nullptr);
}

void UIClicked (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::UISystem::Get().WasClicked(UIArg(call)));
}

void UISetInteractable (tb::Call* call, void*) {
    const Entity e = UIArg(call);
    const bool on = tb::ArgBool(call, 1);
    bool any = false;
    if (auto* b = st::ui::Buttons(SceneRef()).GetComponent(e)) { b->SetInteractable(on); any = true; }
    if (auto* t = st::ui::Toggles(SceneRef()).GetComponent(e)) {
        if (on) t->_flags |= st::ui::UIToggleComponent::INTERACTABLE; else t->_flags &= ~st::ui::UIToggleComponent::INTERACTABLE;
        any = true;
    }
    if (auto* s = st::ui::Sliders(SceneRef()).GetComponent(e)) {
        if (on) s->_flags |= st::ui::UISliderComponent::INTERACTABLE; else s->_flags &= ~st::ui::UISliderComponent::INTERACTABLE;
        any = true;
    }
    tb::ReturnBool(call, any);
}

void UIPointerOver (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::UISystem::Get().PointerOverUI());
}

void UIDocSetRml (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::RmlHost::Get().SetInnerRml(UIArg(call), tb::ArgText(call, 1), tb::ArgText(call, 2)));
}

void UIDocSetClass (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::RmlHost::Get().SetClass(UIArg(call), tb::ArgText(call, 1), tb::ArgText(call, 2),
                                                         tb::ArgBool(call, 3)));
}

void UIDocSetProperty (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::RmlHost::Get().SetProperty(UIArg(call), tb::ArgText(call, 1), tb::ArgText(call, 2),
                                                            tb::ArgText(call, 3)));
}

void UIDocSetAttribute (tb::Call* call, void*) {
    tb::ReturnBool(call, st::ui::RmlHost::Get().SetAttribute(UIArg(call), tb::ArgText(call, 1), tb::ArgText(call, 2),
                                                             tb::ArgText(call, 3)));
}

void UIDocGetValue (tb::Call* call, void*) {
    const std::string v = st::ui::RmlHost::Get().GetValue(UIArg(call), tb::ArgText(call, 1));
    tb::ReturnString(call, v.c_str());
}

// --- reading back what the setters set ------------------------------------------------

void UIGetPosition (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnVector2(call, r ? tb::Vec2{ r->position.x, r->position.y } : tb::Vec2{});
}

void UIGetSize (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnVector2(call, r ? tb::Vec2{ r->size.x, r->size.y } : tb::Vec2{});
}

// Where it is on the game image this frame (pixels, top left 0,0), after anchors and layouts.
void UIGetScreenPosition (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnVector2(call, r ? tb::Vec2{ r->_pixels.x, r->_pixels.y } : tb::Vec2{});
}

void UIGetScreenSize (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnVector2(call, r ? tb::Vec2{ r->_pixels.z, r->_pixels.w } : tb::Vec2{});
}

void UIGetColor (tb::Call* call, void*) {
    const Entity e = UIArg(call);
    XMFLOAT4 c(1, 1, 1, 1);
    if (const st::ui::UIImageComponent* i = st::ui::Images(SceneRef()).GetComponent(e)) c = i->color;
    else if (const st::ui::UITextComponent* t = st::ui::Texts(SceneRef()).GetComponent(e)) c = t->color;
    tb::ReturnColor(call, tb::Rgba{ c.x, c.y, c.z, c.w });
}

void UIGetOpacity (tb::Call* call, void*) {
    const st::ui::UIRectComponent* r = st::ui::Rects(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnFloat(call, r ? r->opacity : 0.0);
}

void UIGetFill (tb::Call* call, void*) {
    const st::ui::UIImageComponent* i = st::ui::Images(SceneRef()).GetComponent(UIArg(call));
    tb::ReturnFloat(call, i ? i->fillAmount : 0.0);
}

void UIIsHovered (tb::Call* call, void*) { tb::ReturnBool(call, st::ui::UISystem::Get().IsHovered(UIArg(call))); }
void UIIsPressed (tb::Call* call, void*) { tb::ReturnBool(call, st::ui::UISystem::Get().IsPressed(UIArg(call))); }

// --- the tree, in UI order (sibling `order`, then creation) -----------------------------

std::vector<Entity> UIChildren (Entity parent) {
    wi::scene::Scene& s = SceneRef();
    auto& rects = st::ui::Rects(s);
    std::vector<Entity> out;
    if (parent == INVALID_ENTITY) return out;
    for (size_t i = 0; i < s.hierarchy.GetCount(); ++i) {
        const Entity e = s.hierarchy.GetEntity(i);
        if (s.hierarchy[i].parentID == parent && rects.Contains(e)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [&rects](Entity a, Entity b) {
        const int oa = rects.GetComponent(a)->order, ob = rects.GetComponent(b)->order;
        return oa != ob ? oa < ob : a < b;
    });
    return out;
}

void UIGetParent (tb::Call* call, void*) {
    const Entity e = UIArg(call);
    const Entity p = ParentOf(e);
    tb::Return(call, ObjectValue(Kind::GameObject, st::ui::Rects(SceneRef()).Contains(p) ? p : INVALID_ENTITY));
}

void UIChildCount (tb::Call* call, void*) { tb::ReturnInt(call, (int64_t)UIChildren(UIArg(call)).size()); }

void UIGetChild (tb::Call* call, void*) {
    const std::vector<Entity> kids = UIChildren(UIArg(call));
    const int64_t i = tb::ArgInt(call, 1);
    tb::Return(call, ObjectValue(Kind::GameObject, i >= 0 && i < (int64_t)kids.size() ? kids[(size_t)i] : INVALID_ENTITY));
}

// A descendant of `root` by name, breadth first - scene.find is global, this is one panel's.
void UIFind (tb::Call* call, void*) {
    const Entity root = UIArg(call);
    const std::string name = tb::ArgString(call, 1);
    if (tb::Failed(call)) return;
    wi::scene::Scene& s = SceneRef();
    std::vector<Entity> queue = UIChildren(root);
    for (size_t i = 0; i < queue.size() && i < 4096; ++i) {
        const wi::scene::NameComponent* n = s.names.GetComponent(queue[i]);
        if (n && n->name == name) { tb::Return(call, ObjectValue(Kind::GameObject, queue[i])); return; }
        for (Entity c : UIChildren(queue[i])) queue.push_back(c);
    }
    tb::Return(call, tb::Value::Null());
}

// Moves `e` under `parent` (a UI element), last among its new siblings.
void UISetParent (tb::Call* call, void*) {
    const Entity e = UIArg(call);
    void* h = tb::ArgObject(call, 1, kGameObject);
    if (tb::Failed(call)) return;
    const Entity parent = h ? EntityOf(h) : INVALID_ENTITY;
    wi::scene::Scene& s = SceneRef();
    auto& rects = st::ui::Rects(s);
    st::ui::UIRectComponent* r = rects.GetComponent(e);
    if (r == nullptr || !rects.Contains(parent) || parent == e) { tb::ReturnBool(call, false); return; }
    for (Entity p = parent; p != INVALID_ENTITY; p = ParentOf(p))
        if (p == e) { tb::ReturnBool(call, false); return; }   // into its own child
    int last = -1;
    for (Entity c : UIChildren(parent)) last = std::max(last, rects.GetComponent(c)->order);
    s.Component_Detach(e);
    s.Component_Attach(e, parent, true);
    r = rects.GetComponent(e);
    r->order = last + 1;
    tb::ReturnBool(call, true);
}

// --- creating and destroying ------------------------------------------------------------

void UICreate (tb::Call* call, void*) {
    std::string type = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    Entity parent = INVALID_ENTITY;
    if (tb::ArgCount(call) > 1 && tb::ArgType(call, 1) == tb::Type::Object) parent = EntityOf(tb::ArgObject(call, 1, kGameObject));
    const std::string name = tb::ArgCount(call) > 2 ? tb::ArgString(call, 2) : "";
    if (tb::Failed(call)) return;
    // "Vertical Layout", "VerticalLayout" and "verticallayout" are the same preset.
    auto key = [](std::string t) {
        std::string k;
        for (char c : t) if (c != ' ' && c != '_') k += (char)std::tolower((unsigned char)c);
        return k;
    };
    const std::string want = key(type);
    for (uint32_t i = 0; i <= (uint32_t)st::ui::Preset::Grid; ++i) {
        const st::ui::Preset preset = (st::ui::Preset)i;
        if (key(st::ui::PresetName(preset)) != want) continue;
        if (parent != INVALID_ENTITY && !st::ui::Rects(SceneRef()).Contains(parent)) {
            tb::Fail(call, "UI.Create: the parent is not a UI element");
            return;
        }
        tb::Return(call, ObjectValue(Kind::GameObject, st::ui::Create(SceneRef(), preset, parent, name)));
        return;
    }
    tb::Fail(call, ("UI.Create: unknown type '" + type + "' (Canvas, World Canvas, Panel, Image, Text, Button, "
                    "Toggle, Slider, Document, Vertical Layout, Horizontal Layout, Grid)").c_str());
}

// The element and everything under it.
void UIDestroy (tb::Call* call, void*) {
    const Entity e = UIArg(call);
    if (!st::ui::Rects(SceneRef()).Contains(e)) { tb::ReturnBool(call, false); return; }
    SceneRef().Entity_Remove(e, true);
    tb::ReturnBool(call, true);
}

// --- events from code -------------------------------------------------------------------
// UI.On(button, "click", self, "OnPlay"): `event` on `element` calls func handler on the
//	scripts of `target`. Events: click, value (toggle bool / slider float), enter, exit,
//	down, up. Gone when the element or the target's script is.

void UIOn (tb::Call* call, void*) {
    const Entity element = UIArg(call);
    const std::string event = tb::ArgString(call, 1);
    void* t = tb::ArgObject(call, 2, kGameObject);
    const std::string handler = tb::ArgString(call, 3);
    if (tb::Failed(call)) return;
    if (!st::ui::Rects(SceneRef()).Contains(element)) { tb::ReturnBool(call, false); return; }
    if (!st::ui::UISystem::Get().Subscribe(element, event, t ? EntityOf(t) : INVALID_ENTITY, handler)) {
        tb::Fail(call, ("UI.On: unknown event '" + event + "' (click, value, enter, exit, down, up)").c_str());
        return;
    }
    tb::ReturnBool(call, true);
}

void UIOff (tb::Call* call, void*) {
    const Entity element = UIArg(call);
    const std::string event = tb::ArgCount(call) > 1 ? tb::ArgString(call, 1) : "";
    void* t = tb::ArgCount(call) > 2 ? tb::ArgObject(call, 2, kGameObject) : nullptr;
    const std::string handler = tb::ArgCount(call) > 3 ? tb::ArgString(call, 3) : "";
    if (tb::Failed(call)) return;
    tb::ReturnInt(call, st::ui::UISystem::Get().Unsubscribe(element, event, t ? EntityOf(t) : INVALID_ENTITY, handler));
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------
#define ST_TRANSFORM_GET(NAME, EXPR)                                                   \
    void NAME (tb::Call* call, void* self, void*) {                                    \
        if (wi::scene::TransformComponent* t = TransformOf(call, self)) { EXPR; }      \
    }

ST_TRANSFORM_GET(TrGetPosition, tb::ReturnVector3(call, V(WorldPosition(*t))))
ST_TRANSFORM_GET(TrGetLocalPosition, tb::ReturnVector3(call, V(t->translation_local)))
ST_TRANSFORM_GET(TrGetRotation, tb::ReturnQuaternion(call, Q(WorldRotation(*t))))
ST_TRANSFORM_GET(TrGetLocalRotation, tb::ReturnQuaternion(call, Q(t->rotation_local)))
ST_TRANSFORM_GET(TrGetEuler, tb::ReturnVector3(call, tb::QuaternionToEuler(Q(WorldRotation(*t)))))
ST_TRANSFORM_GET(TrGetLocalEuler, tb::ReturnVector3(call, tb::QuaternionToEuler(Q(t->rotation_local))))
ST_TRANSFORM_GET(TrGetLocalScale, tb::ReturnVector3(call, V(t->scale_local)))
ST_TRANSFORM_GET(TrGetLossyScale, tb::ReturnVector3(call, V(WorldScale(*t))))
ST_TRANSFORM_GET(TrGetForward, tb::ReturnVector3(call, V(t->GetForward())))
ST_TRANSFORM_GET(TrGetRight, tb::ReturnVector3(call, V(t->GetRight())))
ST_TRANSFORM_GET(TrGetUp, tb::ReturnVector3(call, V(t->GetUp())))
#undef ST_TRANSFORM_GET

void TrSetPosition (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetWorldPosition(EntityOf(self), *t, X(v));
}

void TrSetLocalPosition (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetLocalPosition(EntityOf(self), *t, X(v));
}

void TrSetRotation (tb::Call* call, void* self, void*) {
    const tb::Quat q = tb::ArgQuaternion(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetWorldRotation(EntityOf(self), *t, X(q));
}

void TrSetLocalRotation (tb::Call* call, void* self, void*) {
    const tb::Quat q = tb::ArgQuaternion(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetLocalRotation(EntityOf(self), *t, X(q));
}

void TrSetEuler (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetWorldRotation(EntityOf(self), *t, X(tb::EulerToQuaternion(v)));
}

void TrSetLocalEuler (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) SetLocalRotation(EntityOf(self), *t, X(tb::EulerToQuaternion(v)));
}

void TrSetLocalScale (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) {
        t->scale_local = X(v);
        t->SetDirty();
        Refresh(EntityOf(self), *t);
    }
}

void TrGetGameObject (tb::Call* call, void* self, void*) { tb::Return(call, ObjectValue(Kind::GameObject, EntityOf(self))); }

void TrGetName (tb::Call* call, void* self, void* user) { GoGetName(call, self, user); }

void TrGetParent (tb::Call* call, void* self, void*) {
    const Entity p = ParentOf(EntityOf(self));
    tb::Return(call, SceneRef().transforms.Contains(p) ? ObjectValue(Kind::Transform, p) : tb::Value::Null());
}

void TrGetChildCount (tb::Call* call, void* self, void*) {
    const Entity e = EntityOf(self);
    const auto& h = SceneRef().hierarchy;
    int64_t n = 0;
    for (size_t i = 0; i < h.GetCount(); ++i)
        if (h[i].parentID == e) ++n;
    tb::ReturnInt(call, n);
}

void TrGetChild (tb::Call* call, void* self, void*) {
    const int64_t index = tb::ArgInt(call, 0);
    if (tb::Failed(call)) return;
    const Entity e = EntityOf(self);
    const auto& h = SceneRef().hierarchy;
    int64_t n = 0;
    for (size_t i = 0; i < h.GetCount(); ++i) {
        if (h[i].parentID != e) continue;
        if (n++ == index) { tb::Return(call, ObjectValue(Kind::Transform, h.GetEntity(i))); return; }
    }
    tb::Return(call, tb::Value::Null());
}

void TrSetParent (tb::Call* call, void* self, void*) {
    const Entity e = EntityOf(self);
    void* parent = tb::ArgObject(call, 0, kTransform);
    if (tb::Failed(call)) return;
    wi::scene::Scene& s = SceneRef();
    if (!Exists(e)) { FailMissing(call, e, "Transform"); return; }
    // Unity's default: the world pose stays where it is.
    if (parent) s.Component_Attach(e, EntityOf(parent));
    else s.Component_Detach(e);
}

void TrTranslate (tb::Call* call, void* self, void*) {
    tb::Vec3 v = tb::ArgVector3(call, 0);
    const int64_t space = tb::ArgCount(call) > 1 ? tb::ArgInt(call, 1) : 0;
    if (tb::Failed(call)) return;
    auto* t = TransformOf(call, self);
    if (!t) return;
    if (space == 0) v = tb::RotateVector(Q(WorldRotation(*t)), v); // Space.Self: along its own axes
    const XMFLOAT3 p = WorldPosition(*t);
    SetWorldPosition(EntityOf(self), *t, XMFLOAT3(p.x + v.x, p.y + v.y, p.z + v.z));
}

void TrRotate (tb::Call* call, void* self, void*) {
    const tb::Vec3 euler = tb::ArgVector3(call, 0);
    const int64_t space = tb::ArgCount(call) > 1 ? tb::ArgInt(call, 1) : 0;
    if (tb::Failed(call)) return;
    auto* t = TransformOf(call, self);
    if (!t) return;
    const tb::Quat r = tb::EulerToQuaternion(euler);
    if (space == 0) SetLocalRotation(EntityOf(self), *t, X(tb::QuaternionMultiply(Q(t->rotation_local), r)));
    else            SetWorldRotation(EntityOf(self), *t, X(tb::QuaternionMultiply(r, Q(WorldRotation(*t)))));
}

void TrRotateAround (tb::Call* call, void* self, void*) {
    const tb::Vec3 point = tb::ArgVector3(call, 0);
    const tb::Vec3 axis = tb::ArgVector3(call, 1);
    const double angle = tb::ArgFloat(call, 2);
    if (tb::Failed(call)) return;
    auto* t = TransformOf(call, self);
    if (!t) return;
    const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len < 1e-6f) return;
    const float h = (float)angle * 0.5f * 0.01745329252f, sn = std::sin(h) / len;
    const tb::Quat r{ axis.x * sn, axis.y * sn, axis.z * sn, std::cos(h) };
    const XMFLOAT3 p = WorldPosition(*t);
    const tb::Vec3 off = tb::RotateVector(r, tb::Vec3{ p.x - point.x, p.y - point.y, p.z - point.z });
    SetWorldPosition(EntityOf(self), *t, XMFLOAT3(point.x + off.x, point.y + off.y, point.z + off.z));
    SetWorldRotation(EntityOf(self), *t, X(tb::QuaternionMultiply(r, Q(WorldRotation(*t)))));
}

void TrLookAt (tb::Call* call, void* self, void*) {
    const tb::Vec3 target = tb::ArgVector3(call, 0);
    const tb::Vec3 up = (tb::ArgCount(call) > 1 && tb::ArgType(call, 1) == tb::Type::Vector3)
                            ? tb::ArgVector3(call, 1) : tb::Vec3{ 0, 1, 0 };
    if (tb::Failed(call)) return;
    auto* t = TransformOf(call, self);
    if (!t) return;
    const XMFLOAT3 p = WorldPosition(*t);
    const tb::Vec3 dir{ target.x - p.x, target.y - p.y, target.z - p.z };
    SetWorldRotation(EntityOf(self), *t, X(tb::LookRotation(dir, up)));
}

void TrTransformPoint (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) {
        XMFLOAT3 out;
        const XMFLOAT3 p(v.x, v.y, v.z);
        XMStoreFloat3(&out, XMVector3Transform(XMLoadFloat3(&p), XMLoadFloat4x4(&t->world)));
        tb::ReturnVector3(call, V(out));
    }
}

void TrInverseTransformPoint (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) {
        XMFLOAT3 out;
        const XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&t->world));
        const XMFLOAT3 p(v.x, v.y, v.z);
        XMStoreFloat3(&out, XMVector3Transform(XMLoadFloat3(&p), inv));
        tb::ReturnVector3(call, V(out));
    }
}

void TrTransformDirection (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) tb::ReturnVector3(call, tb::RotateVector(Q(WorldRotation(*t)), v));
}

void TrInverseTransformDirection (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* t = TransformOf(call, self)) {
        const tb::Quat q = Q(WorldRotation(*t));
        tb::ReturnVector3(call, tb::RotateVector(tb::Quat{ -q.x, -q.y, -q.z, q.w }, v));
    }
}

// ---------------------------------------------------------------------------
// Rigidbody
// ---------------------------------------------------------------------------
void RbGetVelocity (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnVector3(call, V(wi::physics::GetVelocity(*r)));
}

void RbSetVelocity (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) wi::physics::SetLinearVelocity(*r, X(v));
}

void RbGetMass (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnFloat(call, r->mass);
}

void RbSetMass (tb::Call* call, void* self, void*) {
    const double m = tb::ArgFloat(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) r->mass = (float)m;
}

void RbGetKinematic (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnBool(call, r->IsKinematic());
}

void RbSetKinematic (tb::Call* call, void* self, void*) {
    const bool k = tb::ArgBool(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) r->SetKinematic(k);
}

void RbGetDrag (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnFloat(call, r->damping_linear);
}

void RbSetDrag (tb::Call* call, void* self, void*) {
    const double d = tb::ArgFloat(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) r->damping_linear = (float)d;
}

void RbGetAngularDrag (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnFloat(call, r->damping_angular);
}

void RbSetAngularDrag (tb::Call* call, void* self, void*) {
    const double d = tb::ArgFloat(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) r->damping_angular = (float)d;
}

void ApplyForceMode (wi::scene::RigidBodyPhysicsComponent& r, const tb::Vec3& f, int64_t mode);

void RbAddForce (tb::Call* call, void* self, void*) {
    const tb::Vec3 f = tb::ArgVector3(call, 0);
    const int64_t mode = tb::ArgCount(call) > 1 ? tb::ArgInt(call, 1) : 0;
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) ApplyForceMode(*r, f, mode);
}

void RbAddTorque (tb::Call* call, void* self, void*) {
    const tb::Vec3 f = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) wi::physics::ApplyTorque(*r, X(f));
}

void RbMovePosition (tb::Call* call, void* self, void*) {
    const tb::Vec3 p = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) wi::physics::SetPosition(*r, X(p));
}

// The SIMULATED pose, not the transform: inside FixedUpdate the transform still holds the
//	interpolated render pose, while these are what the next physics step starts from.
void RbGetPosition (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnVector3(call, V(wi::physics::GetPosition(*r)));
}

void RbGetRotation (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnQuaternion(call, Q(wi::physics::GetRotation(*r)));
}

void RbSetRotation (tb::Call* call, void* self, void*) {
    const tb::Quat q = tb::ArgQuaternion(call, 0);
    if (tb::Failed(call)) return;
    auto* r = RigidbodyOf(call, self);
    if (!r) return;
    XMFLOAT4 n = X(q);
    XMStoreFloat4(&n, XMQuaternionNormalize(XMLoadFloat4(&n)));
    wi::physics::SetPositionAndRotation(*r, wi::physics::GetPosition(*r), n);
}

void RbGetAngularVelocity (tb::Call* call, void* self, void*) {
    if (auto* r = RigidbodyOf(call, self)) tb::ReturnVector3(call, V(wi::physics::GetAngularVelocity(*r)));
}

void RbSetAngularVelocity (tb::Call* call, void* self, void*) {
    const tb::Vec3 v = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self)) wi::physics::SetAngularVelocity(*r, X(v));
}

// ForceMode as AddForce: Force, Acceleration (mass-independent), Impulse, VelocityChange.
void ApplyForceMode (wi::scene::RigidBodyPhysicsComponent& r, const tb::Vec3& f, int64_t mode) {
    const float k = (mode == 1 || mode == 3) ? r.mass : 1.0f;
    const XMFLOAT3 v(f.x * k, f.y * k, f.z * k);
    if (mode == 2 || mode == 3) wi::physics::ApplyImpulse(r, v);
    else                        wi::physics::ApplyForce(r, v);
}

void RbAddRelativeForce (tb::Call* call, void* self, void*) {
    const tb::Vec3 f = tb::ArgVector3(call, 0);
    const int64_t mode = tb::ArgCount(call) > 1 ? tb::ArgInt(call, 1) : 0;
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self))
        ApplyForceMode(*r, tb::RotateVector(Q(wi::physics::GetRotation(*r)), f), mode);
}

void RbAddForceAtPosition (tb::Call* call, void* self, void*) {
    const tb::Vec3 f = tb::ArgVector3(call, 0);
    const tb::Vec3 at = tb::ArgVector3(call, 1);
    const int64_t mode = tb::ArgCount(call) > 2 ? tb::ArgInt(call, 2) : 0;
    if (tb::Failed(call)) return;
    auto* r = RigidbodyOf(call, self);
    if (!r) return;
    const float k = (mode == 1 || mode == 3) ? r->mass : 1.0f;
    const XMFLOAT3 v(f.x * k, f.y * k, f.z * k);
    if (mode == 2 || mode == 3) wi::physics::ApplyImpulseAt(*r, v, X(at), false);
    else                        wi::physics::ApplyForceAt(*r, v, X(at), false);
}

void RbAddRelativeTorque (tb::Call* call, void* self, void*) {
    const tb::Vec3 t = tb::ArgVector3(call, 0);
    if (tb::Failed(call)) return;
    if (auto* r = RigidbodyOf(call, self))
        wi::physics::ApplyTorque(*r, X(tb::RotateVector(Q(wi::physics::GetRotation(*r)), t)));
}

void CompGetGameObject (tb::Call* call, void* self, void*) { tb::Return(call, ObjectValue(Kind::GameObject, EntityOf(self))); }
void CompGetTransform (tb::Call* call, void* self, void*) { tb::Return(call, ObjectValue(Kind::Transform, EntityOf(self))); }

// ---------------------------------------------------------------------------
// Light
// ---------------------------------------------------------------------------
void LtGetColor (tb::Call* call, void* self, void*) {
    if (auto* l = LightOf(call, self)) tb::ReturnColor(call, tb::Rgba{ l->color.x, l->color.y, l->color.z, 1.0f });
}

void LtSetColor (tb::Call* call, void* self, void*) {
    const tb::Rgba c = tb::ArgColor(call, 0);
    if (tb::Failed(call)) return;
    if (auto* l = LightOf(call, self)) l->color = XMFLOAT3(c.r, c.g, c.b);
}

void LtGetIntensity (tb::Call* call, void* self, void*) { if (auto* l = LightOf(call, self)) tb::ReturnFloat(call, l->intensity); }
void LtSetIntensity (tb::Call* call, void* self, void*) {
    const double v = tb::ArgFloat(call, 0);
    if (!tb::Failed(call)) if (auto* l = LightOf(call, self)) l->intensity = (float)v;
}
void LtGetRange (tb::Call* call, void* self, void*) { if (auto* l = LightOf(call, self)) tb::ReturnFloat(call, l->range); }
void LtSetRange (tb::Call* call, void* self, void*) {
    const double v = tb::ArgFloat(call, 0);
    if (!tb::Failed(call)) if (auto* l = LightOf(call, self)) l->range = (float)v;
}
// Unity's spotAngle is the FULL cone in degrees; the engine stores the half angle in radians.
void LtGetSpotAngle (tb::Call* call, void* self, void*) {
    if (auto* l = LightOf(call, self)) tb::ReturnFloat(call, l->outerConeAngle * 2.0f * 57.2957795f);
}
void LtSetSpotAngle (tb::Call* call, void* self, void*) {
    const double v = tb::ArgFloat(call, 0);
    if (!tb::Failed(call)) if (auto* l = LightOf(call, self)) l->outerConeAngle = (float)v * 0.5f * 0.01745329252f;
}
void LtGetShadows (tb::Call* call, void* self, void*) { if (auto* l = LightOf(call, self)) tb::ReturnBool(call, l->IsCastingShadow()); }
void LtSetShadows (tb::Call* call, void* self, void*) {
    const bool v = tb::ArgBool(call, 0);
    if (!tb::Failed(call)) if (auto* l = LightOf(call, self)) l->SetCastShadow(v);
}

// ---------------------------------------------------------------------------
// Component (any native component, by its NCI_ name)
// ---------------------------------------------------------------------------
void NcGetEnabled (tb::Call* call, void* self, void*) {
    if (auto* c = ComponentOf(call, self)) tb::ReturnBool(call, c->IsEnabled());
}
void NcSetEnabled (tb::Call* call, void* self, void*) {
    const bool v = tb::ArgBool(call, 0);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) c->SetEnabled(v);
}
void NcGetName (tb::Call* call, void* self, void*) {
    if (auto* c = ComponentOf(call, self)) tb::ReturnString(call, c->componentName.c_str());
}
void NcGetGameObject (tb::Call* call, void* self, void*) {
    if (auto* c = ComponentOf(call, self)) tb::Return(call, ObjectValue(Kind::GameObject, c->entity));
}
void NcGetFloat (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const double def = tb::ArgCount(call) > 1 ? tb::ArgFloat(call, 1) : 0.0;
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) tb::ReturnFloat(call, c->GetFloat(key, (float)def));
}
void NcSetFloat (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const double v = tb::ArgFloat(call, 1);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) c->SetFloat(key, (float)v);
}
void NcGetInt (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const int64_t def = tb::ArgCount(call) > 1 ? tb::ArgInt(call, 1) : 0;
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) tb::ReturnInt(call, c->GetInt(key, (int)def));
}
void NcSetInt (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const int64_t v = tb::ArgInt(call, 1);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) c->SetInt(key, (int)v);
}
void NcGetBool (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const bool def = tb::ArgCount(call) > 1 && tb::ArgBool(call, 1);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) tb::ReturnBool(call, c->GetBool(key, def));
}
void NcSetBool (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const bool v = tb::ArgBool(call, 1);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) c->SetBool(key, v);
}
void NcGetString (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const std::string def = tb::ArgCount(call) > 1 ? tb::ArgString(call, 1) : "";
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) tb::ReturnString(call, c->GetString(key, def).c_str());
}
void NcSetString (tb::Call* call, void* self, void*) {
    const std::string key = tb::ArgString(call, 0);
    const std::string v = tb::ArgString(call, 1);
    if (!tb::Failed(call)) if (auto* c = ComponentOf(call, self)) c->SetString(key, v);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
// Unity KeyCode names, lower-case ("space", "left shift", "f1", "a", "0", "keypad5").
wi::input::BUTTON KeyFromName (std::string name) {
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    using namespace wi::input;
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return (BUTTON)(CHARACTER_RANGE_START + (c - 'a'));
        if (c >= '0' && c <= '9') return (BUTTON)(DIGIT_RANGE_START + (c - '0'));
    }
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit((unsigned char)name[1])) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return (BUTTON)(KEYBOARD_BUTTON_F1 + (n - 1));
    }
    if (name.rfind("keypad", 0) == 0 && name.size() == 7 && std::isdigit((unsigned char)name[6]))
        return (BUTTON)(KEYBOARD_BUTTON_NUMPAD0 + (name[6] - '0'));
    static const std::unordered_map<std::string, BUTTON> named = {
        { "space", KEYBOARD_BUTTON_SPACE }, { "return", KEYBOARD_BUTTON_ENTER }, { "enter", KEYBOARD_BUTTON_ENTER },
        { "escape", KEYBOARD_BUTTON_ESCAPE }, { "tab", KEYBOARD_BUTTON_TAB }, { "backspace", KEYBOARD_BUTTON_BACKSPACE },
        { "delete", KEYBOARD_BUTTON_DELETE }, { "insert", KEYBOARD_BUTTON_INSERT }, { "home", KEYBOARD_BUTTON_HOME },
        { "page up", KEYBOARD_BUTTON_PAGEUP }, { "page down", KEYBOARD_BUTTON_PAGEDOWN },
        { "up", KEYBOARD_BUTTON_UP }, { "down", KEYBOARD_BUTTON_DOWN }, { "left", KEYBOARD_BUTTON_LEFT },
        { "right", KEYBOARD_BUTTON_RIGHT }, { "left shift", KEYBOARD_BUTTON_LSHIFT },
        { "right shift", KEYBOARD_BUTTON_RSHIFT }, { "left ctrl", KEYBOARD_BUTTON_LCONTROL },
        { "right ctrl", KEYBOARD_BUTTON_RCONTROL }, { "left alt", KEYBOARD_BUTTON_ALT },
        { "right alt", KEYBOARD_BUTTON_ALTGR }, { "alt", KEYBOARD_BUTTON_ALT }, { "`", KEYBOARD_BUTTON_TILDE },
        { "mouse 0", MOUSE_BUTTON_LEFT }, { "mouse 1", MOUSE_BUTTON_RIGHT }, { "mouse 2", MOUSE_BUTTON_MIDDLE },
    };
    auto it = named.find(name);
    return it == named.end() ? BUTTON_NONE : it->second;
}

wi::input::BUTTON MouseButton (int64_t i) {
    return i == 0 ? wi::input::MOUSE_BUTTON_LEFT : i == 1 ? wi::input::MOUSE_BUTTON_RIGHT
         : i == 2 ? wi::input::MOUSE_BUTTON_MIDDLE : wi::input::BUTTON_NONE;
}

// The game's own action map ("Player", or the first one registered) - so
//	Input.GetButton("Jump") is whatever the game bound Jump to.
st::input::ActionRuntime actions;

bool HasAction (const std::string& name) { return actions.Valid() && actions.Index(name) >= 0; }

template <bool (*Query)(wi::input::BUTTON, int)>
void KeyQuery (tb::Call* call, void*) {
    const std::string name = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    const wi::input::BUTTON b = KeyFromName(name);
    if (b == wi::input::BUTTON_NONE) { tb::Fail(call, ("unknown key '" + name + "'").c_str()); return; }
    tb::ReturnBool(call, Query(b, 0));
}

template <bool (*Query)(wi::input::BUTTON, int)>
void MouseQuery (tb::Call* call, void*) {
    const int64_t i = tb::ArgInt(call, 0);
    if (!tb::Failed(call)) tb::ReturnBool(call, Query(MouseButton(i), 0));
}

bool KeyDown (wi::input::BUTTON b, int p) { return wi::input::Down(b, p); }
bool KeyPress (wi::input::BUTTON b, int p) { return wi::input::Press(b, p); }
bool KeyRelease (wi::input::BUTTON b, int p) { return wi::input::Release(b, p); }

// Unity's default buttons, used when the game's map has no action of that name.
wi::input::BUTTON DefaultButton (const std::string& name) {
    if (name == "Jump")   return wi::input::KEYBOARD_BUTTON_SPACE;
    if (name == "Fire1")  return wi::input::MOUSE_BUTTON_LEFT;
    if (name == "Fire2")  return wi::input::MOUSE_BUTTON_RIGHT;
    if (name == "Fire3")  return wi::input::MOUSE_BUTTON_MIDDLE;
    if (name == "Submit") return wi::input::KEYBOARD_BUTTON_ENTER;
    if (name == "Cancel") return wi::input::KEYBOARD_BUTTON_ESCAPE;
    return wi::input::BUTTON_NONE;
}

template <int Edge> // 0 held, 1 pressed, 2 released
void ButtonQuery (tb::Call* call, void*) {
    const std::string name = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    if (HasAction(name)) {
        tb::ReturnBool(call, Edge == 0 ? actions.Down(name) : Edge == 1 ? actions.Pressed(name) : actions.Released(name));
        return;
    }
    const wi::input::BUTTON b = DefaultButton(name);
    if (b == wi::input::BUTTON_NONE) { tb::Fail(call, ("no input button or action named '" + name + "'").c_str()); return; }
    tb::ReturnBool(call, Edge == 0 ? wi::input::Down(b) : Edge == 1 ? wi::input::Press(b) : wi::input::Release(b));
}

float DefaultAxis (const std::string& name, bool& known) {
    known = true;
    using namespace wi::input;
    auto key = [](BUTTON b) { return Down(b) ? 1.0f : 0.0f; };
    const XMFLOAT4 stick = GetAnalog(GAMEPAD_ANALOG_THUMBSTICK_L);
    if (name == "Horizontal") {
        const float k = key((BUTTON)(CHARACTER_RANGE_START + ('d' - 'a'))) + key(KEYBOARD_BUTTON_RIGHT)
                      - key((BUTTON)(CHARACTER_RANGE_START + ('a' - 'a'))) - key(KEYBOARD_BUTTON_LEFT);
        return std::clamp(k + stick.x, -1.0f, 1.0f);
    }
    if (name == "Vertical") {
        const float k = key((BUTTON)(CHARACTER_RANGE_START + ('w' - 'a'))) + key(KEYBOARD_BUTTON_UP)
                      - key((BUTTON)(CHARACTER_RANGE_START + ('s' - 'a'))) - key(KEYBOARD_BUTTON_DOWN);
        return std::clamp(k + stick.y, -1.0f, 1.0f);
    }
    const MouseState& m = GetMouseState();
    if (name == "Mouse X") return m.delta_position.x * 0.1f;
    if (name == "Mouse Y") return -m.delta_position.y * 0.1f; // up is positive, as in Unity
    if (name == "Mouse ScrollWheel") return m.delta_wheel * 0.1f;
    known = false;
    return 0.0f;
}

void GetAxis (tb::Call* call, void*) {
    const std::string name = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    if (HasAction(name)) { tb::ReturnFloat(call, actions.Value(name)); return; }
    bool known = false;
    const float v = DefaultAxis(name, known);
    if (!known) { tb::Fail(call, ("no input axis or action named '" + name + "'").c_str()); return; }
    tb::ReturnFloat(call, v);
}

void GetAxis2 (tb::Call* call, void*) {
    const std::string name = tb::ArgString(call, 0);
    if (tb::Failed(call)) return;
    if (HasAction(name)) {
        const XMFLOAT2 v = actions.Vector2(name);
        tb::ReturnVector2(call, tb::Vec2{ v.x, v.y });
        return;
    }
    tb::Fail(call, ("no Vector2 action named '" + name + "' (register it in OnKeyRegister)").c_str());
}

// ---------------------------------------------------------------------------
// Debug / Physics
// ---------------------------------------------------------------------------
void DebugLog (tb::Call* call, void*) { wi::backlog::post(std::string("[script] ") + tb::ArgText(call, 0)); }
void DebugWarn (tb::Call* call, void*) {
    wi::backlog::post(std::string("[script] ") + tb::ArgText(call, 0), wi::backlog::LogLevel::Warning);
}
void DebugError (tb::Call* call, void*) {
    wi::backlog::post(std::string("[script] ") + tb::ArgText(call, 0), wi::backlog::LogLevel::Error);
}

void DrawLineImpl (tb::Call* call, const tb::Vec3& a, const tb::Vec3& b, int colorArg) {
    const tb::Rgba c = (tb::ArgCount(call) > colorArg && tb::ArgType(call, colorArg) == tb::Type::Color)
                           ? tb::ArgColor(call, colorArg) : tb::Rgba{ 1, 1, 1, 1 };
    if (tb::Failed(call)) return;
    st::render::DebugLine(X(a), X(b), XMFLOAT4(c.r, c.g, c.b, c.a));
}

void DebugDrawLine (tb::Call* call, void*) {
    const tb::Vec3 a = tb::ArgVector3(call, 0), b = tb::ArgVector3(call, 1);
    if (!tb::Failed(call)) DrawLineImpl(call, a, b, 2);
}

void DebugDrawRay (tb::Call* call, void*) {
    const tb::Vec3 a = tb::ArgVector3(call, 0), d = tb::ArgVector3(call, 1);
    if (!tb::Failed(call)) DrawLineImpl(call, a, tb::Vec3{ a.x + d.x, a.y + d.y, a.z + d.z }, 2);
}

void PhysicsRaycast (tb::Call* call, void*) {
    const tb::Vec3 o = tb::ArgVector3(call, 0);
    tb::Vec3 d = tb::ArgVector3(call, 1);
    const float maxDistance = tb::ArgCount(call) > 2 ? (float)tb::ArgFloat(call, 2) : 1e30f;
    if (tb::Failed(call)) return;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-6f) { tb::Return(call, tb::Value::Null()); return; }
    d = tb::Vec3{ d.x / len, d.y / len, d.z / len };
    const wi::primitive::Ray ray(X(o), X(d), 0.0f, maxDistance);

    // Colliders first (what Unity's Physics.Raycast means), then render meshes, so a
    //	scene with no physics bodies still answers.
    const wi::physics::RayIntersectionResult ph = wi::physics::Intersects(SceneRef(), ray);
    if (ph.IsValid()) {
        const float dist = std::sqrt((ph.position.x - o.x) * (ph.position.x - o.x) +
                                     (ph.position.y - o.y) * (ph.position.y - o.y) +
                                     (ph.position.z - o.z) * (ph.position.z - o.z));
        if (dist <= maxDistance) {
            tb::Return(call, tb::Value::RaycastHit(V(ph.position), V(ph.normal), dist, Handle(Kind::GameObject, ph.entity)));
            return;
        }
    }
    const wi::scene::Scene::RayIntersectionResult mesh = SceneRef().Intersects(ray);
    if (mesh.entity != INVALID_ENTITY && mesh.distance <= maxDistance) {
        tb::Return(call, tb::Value::RaycastHit(V(mesh.position), V(mesh.normal), mesh.distance,
                                               Handle(Kind::GameObject, mesh.entity)));
        return;
    }
    tb::Return(call, tb::Value::Null());
}

// ---------------------------------------------------------------------------
// 0.1 API, kept so existing scripts run (Entity, scene.find, log.*, lowercase methods)
// ---------------------------------------------------------------------------
void LegacyLog (tb::Call* call, void*) { DebugLog(call, nullptr); }
void LegacyWarn (tb::Call* call, void*) { DebugWarn(call, nullptr); }
void LegacyError (tb::Call* call, void*) { DebugError(call, nullptr); }

void LegacyX (tb::Call* call, void* self, void*) { if (auto* t = TransformOf(call, self)) tb::ReturnFloat(call, t->GetPosition().x); }
void LegacyY (tb::Call* call, void* self, void*) { if (auto* t = TransformOf(call, self)) tb::ReturnFloat(call, t->GetPosition().y); }
void LegacyZ (tb::Call* call, void* self, void*) { if (auto* t = TransformOf(call, self)) tb::ReturnFloat(call, t->GetPosition().z); }

bool Arg3 (tb::Call* call, XMFLOAT3& out) {
    out.x = (float)tb::ArgFloat(call, 0);
    out.y = (float)tb::ArgFloat(call, 1);
    out.z = (float)tb::ArgFloat(call, 2);
    return !tb::Failed(call);
}

void LegacyTranslate (tb::Call* call, void* self, void*) {
    XMFLOAT3 v;
    if (Arg3(call, v)) if (auto* t = TransformOf(call, self)) { t->Translate(v); Refresh(EntityOf(self), *t); }
}
void LegacySetPosition (tb::Call* call, void* self, void*) {
    XMFLOAT3 v;
    if (Arg3(call, v)) if (auto* t = TransformOf(call, self)) SetLocalPosition(EntityOf(self), *t, v);
}
void LegacyRotate (tb::Call* call, void* self, void*) {
    XMFLOAT3 v;
    if (Arg3(call, v)) if (auto* t = TransformOf(call, self)) { t->RotateRollPitchYaw(v); Refresh(EntityOf(self), *t); }
}
void LegacySetScale (tb::Call* call, void* self, void*) {
    XMFLOAT3 v;
    if (Arg3(call, v)) if (auto* t = TransformOf(call, self)) { t->scale_local = v; t->SetDirty(); Refresh(EntityOf(self), *t); }
}

st::InputComponent* InputOf (tb::Call* call, void* self) {
    const Entity e = EntityOf(self);
    wi::scene::NativeComponent* c = SceneRef().nativeComponents.Get(e, wi::scene::GetNativeTypeID<st::InputComponent>());
    if (!c) FailMissing(call, e, "stInput component");
    return static_cast<st::InputComponent*>(c);
}
void LegacyInputDown (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnBool(call, in->Down(a));
}
void LegacyInputPressed (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnBool(call, in->Pressed(a));
}
void LegacyInputReleased (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnBool(call, in->Released(a));
}
void LegacyInputValue (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnFloat(call, in->Value(a));
}
void LegacyInputX (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnFloat(call, in->Vector2(a).x);
}
void LegacyInputY (tb::Call* call, void* self, void*) {
    const std::string a = tb::ArgString(call, 0);
    if (auto* in = InputOf(call, self)) tb::ReturnFloat(call, in->Vector2(a).y);
}

// ---------------------------------------------------------------------------
// per-frame statics
// ---------------------------------------------------------------------------
double sceneTime = 0.0, unscaledTime = 0.0;
int64_t frameCount = 0;

void SetFloat (tb::VM* vm, const char* cls, const char* name, double v) { tb::SetStatic(vm, cls, name, tb::Value::Float(v)); }

} // namespace

void UpdateEngineStatics (tb::VM* vm, float sceneDt, float realDt) {
    // Time.time is the transport's scene clock, which starts again at 0 on every Play.
    sceneTime = st::PlayControl::Get().SceneTime();
    unscaledTime += realDt;
    ++frameCount;
    SetFloat(vm, "Time", "deltaTime", sceneDt);
    SetFloat(vm, "Time", "time", sceneTime);
    SetFloat(vm, "Time", "unscaledDeltaTime", realDt);
    SetFloat(vm, "Time", "unscaledTime", unscaledTime);
    SetFloat(vm, "Time", "timeScale", realDt > 0.0f ? sceneDt / realDt : 1.0f);
    tb::SetStatic(vm, "Time", "frameCount", tb::Value::Int(frameCount));

    // The game's action map, evaluated once per frame for Input.GetButton / GetAxis.
	st::input::Registry& reg = st::InputSystem::Get().Actions();
    if (!actions.Valid() && !reg.Maps().empty()) {
        const std::string map = reg.FindMap("Player") ? std::string("Player") : reg.Maps().front().name;
        actions.Bind(&reg, map, 0);
    }
    if (actions.Valid()) actions.Evaluate(realDt);

    const wi::input::MouseState& m = wi::input::GetMouseState();
    tb::SetStatic(vm, "Input", "mousePosition", tb::Value::Vector3(tb::Vec3{ m.position.x, m.position.y, 0.0f }));
    tb::SetStatic(vm, "Input", "mouseScrollDelta", tb::Value::Vector2(tb::Vec2{ 0.0f, m.delta_wheel }));
}

void SetFixedDeltaTime (tb::VM* vm, float fixedDt) {
    // Inside FixedUpdate Unity's Time.deltaTime IS the fixed step; stScript swaps it in.
    SetFloat(vm, "Time", "deltaTime", fixedDt);
}

void RegisterEngineBindings (tb::VM* vm) {
    st::script::RegisterNetBindings(vm);   // Net.* (script/NetBindings.h)
    tb::RegisterUnityMath(vm, kGameObject);

    // statics: declared once with their kinds; UpdateEngineStatics refreshes them
    SetFloat(vm, "Time", "fixedDeltaTime", wi::scene::NativeComponentManager::FIXED_DT);
    UpdateEngineStatics(vm, 0.0f, 0.0f);

    // ---------------------------------------------------------------- GameObject
    const char* G = kGameObject;
    tb::DefineClass(vm, G, "Entity");
    tb::DefineClass(vm, G, "entity");
    tb::RegisterProperty(vm, G, "name", "str", &GoGetName, &GoSetName, nullptr);
    tb::RegisterProperty(vm, G, "transform", kTransform, &GoGetTransform, nullptr, nullptr);
    tb::RegisterProperty(vm, G, "id", "int", &GoGetId, nullptr, nullptr);
    tb::RegisterMethod(vm, G, "GetComponent", &GoGetComponent, nullptr, "(str) -> any");
    tb::RegisterMethod(vm, G, "AddComponent", &GoAddComponent, nullptr, "(str) -> any");
    tb::RegisterMethod(vm, G, "IsValid", &GoIsValid, nullptr, "() -> bool");
    tb::RegisterFunction(vm, "GameObject.Find", &GoFind, nullptr, "(str) -> GameObject?");
    tb::RegisterFunction(vm, "GameObject.CreatePrimitive", &GoCreatePrimitive, nullptr, "(str) -> GameObject");
    tb::RegisterFunction(vm, "Instantiate", &Instantiate, nullptr, "(GameObject, Vector3=, Quaternion=) -> GameObject");
    tb::RegisterFunction(vm, "Destroy", &Destroy, nullptr, "(GameObject?)");
    tb::RegisterFunction(vm, "Pool.Prewarm", &PoolPrewarm, nullptr, "(str, int) -> int");
    tb::RegisterFunction(vm, "Pool.Spawn", &PoolSpawn, nullptr, "(str, Vector3=, Quaternion=) -> GameObject?");
    tb::RegisterFunction(vm, "Pool.Release", &PoolRelease, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "Pool.Active", &PoolActive, nullptr, "(str) -> int");
    tb::RegisterFunction(vm, "Pool.Parked", &PoolParked, nullptr, "(str) -> int");

    // Game UI. Elements are GameObjects (find them with scene.find); each function does
    //	what applies to the element's components and returns false when nothing did.
    tb::RegisterFunction(vm, "UI.SetText", &UISetText, nullptr, "(GameObject?, str) -> bool");
    tb::RegisterFunction(vm, "UI.GetText", &UIGetText, nullptr, "(GameObject?) -> str");
    tb::RegisterFunction(vm, "UI.SetVisible", &UISetVisible, nullptr, "(GameObject?, bool) -> bool");
    tb::RegisterFunction(vm, "UI.IsVisible", &UIIsVisible, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.SetColor", &UISetColor, nullptr, "(GameObject?, Color) -> bool");
    tb::RegisterFunction(vm, "UI.SetFill", &UISetFill, nullptr, "(GameObject?, float) -> bool");
    tb::RegisterFunction(vm, "UI.SetOpacity", &UISetOpacity, nullptr, "(GameObject?, float) -> bool");
    tb::RegisterFunction(vm, "UI.SetPosition", &UISetPosition, nullptr, "(GameObject?, float, float) -> bool");
    tb::RegisterFunction(vm, "UI.SetSize", &UISetSize, nullptr, "(GameObject?, float, float) -> bool");
    tb::RegisterFunction(vm, "UI.GetValue", &UIGetValue, nullptr, "(GameObject?) -> float");
    tb::RegisterFunction(vm, "UI.SetValue", &UISetValue, nullptr, "(GameObject?, float) -> bool");
    tb::RegisterFunction(vm, "UI.IsOn", &UIIsOn, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.SetOn", &UISetOn, nullptr, "(GameObject?, bool) -> bool");
    tb::RegisterFunction(vm, "UI.Clicked", &UIClicked, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.SetInteractable", &UISetInteractable, nullptr, "(GameObject?, bool) -> bool");
    tb::RegisterFunction(vm, "UI.PointerOver", &UIPointerOver, nullptr, "() -> bool");
    // RmlUi documents: (document, element id, ...).
    tb::RegisterFunction(vm, "UI.DocSetRml", &UIDocSetRml, nullptr, "(GameObject?, str, str) -> bool");
    tb::RegisterFunction(vm, "UI.DocSetClass", &UIDocSetClass, nullptr, "(GameObject?, str, str, bool) -> bool");
    tb::RegisterFunction(vm, "UI.DocSetProperty", &UIDocSetProperty, nullptr, "(GameObject?, str, str, str) -> bool");
    tb::RegisterFunction(vm, "UI.DocSetAttribute", &UIDocSetAttribute, nullptr, "(GameObject?, str, str, str) -> bool");
    tb::RegisterFunction(vm, "UI.DocGetValue", &UIDocGetValue, nullptr, "(GameObject?, str) -> str");
    tb::RegisterFunction(vm, "UI.GetPosition", &UIGetPosition, nullptr, "(GameObject?) -> Vector2");
    tb::RegisterFunction(vm, "UI.GetSize", &UIGetSize, nullptr, "(GameObject?) -> Vector2");
    tb::RegisterFunction(vm, "UI.GetScreenPosition", &UIGetScreenPosition, nullptr, "(GameObject?) -> Vector2");
    tb::RegisterFunction(vm, "UI.GetScreenSize", &UIGetScreenSize, nullptr, "(GameObject?) -> Vector2");
    tb::RegisterFunction(vm, "UI.GetColor", &UIGetColor, nullptr, "(GameObject?) -> Color");
    tb::RegisterFunction(vm, "UI.GetOpacity", &UIGetOpacity, nullptr, "(GameObject?) -> float");
    tb::RegisterFunction(vm, "UI.GetFill", &UIGetFill, nullptr, "(GameObject?) -> float");
    tb::RegisterFunction(vm, "UI.IsHovered", &UIIsHovered, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.IsPressed", &UIIsPressed, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.GetParent", &UIGetParent, nullptr, "(GameObject?) -> GameObject?");
    tb::RegisterFunction(vm, "UI.ChildCount", &UIChildCount, nullptr, "(GameObject?) -> int");
    tb::RegisterFunction(vm, "UI.GetChild", &UIGetChild, nullptr, "(GameObject?, int) -> GameObject?");
    tb::RegisterFunction(vm, "UI.Find", &UIFind, nullptr, "(GameObject?, str) -> GameObject?");
    tb::RegisterFunction(vm, "UI.SetParent", &UISetParent, nullptr, "(GameObject?, GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.Create", &UICreate, nullptr, "(str, GameObject?=, str=) -> GameObject?");
    tb::RegisterFunction(vm, "UI.Destroy", &UIDestroy, nullptr, "(GameObject?) -> bool");
    tb::RegisterFunction(vm, "UI.On", &UIOn, nullptr, "(GameObject?, str, GameObject?, str) -> bool");
    tb::RegisterFunction(vm, "UI.Off", &UIOff, nullptr, "(GameObject?, str=, GameObject?=, str=) -> int");

    // ----------------------------------------------------------------- Transform
    const char* T = kTransform;
    tb::DefineClass(vm, T, nullptr);
    tb::RegisterProperty(vm, T, "position", "Vector3", &TrGetPosition, &TrSetPosition, nullptr);
    tb::RegisterProperty(vm, T, "localPosition", "Vector3", &TrGetLocalPosition, &TrSetLocalPosition, nullptr);
    tb::RegisterProperty(vm, T, "rotation", "Quaternion", &TrGetRotation, &TrSetRotation, nullptr);
    tb::RegisterProperty(vm, T, "localRotation", "Quaternion", &TrGetLocalRotation, &TrSetLocalRotation, nullptr);
    tb::RegisterProperty(vm, T, "eulerAngles", "Vector3", &TrGetEuler, &TrSetEuler, nullptr);
    tb::RegisterProperty(vm, T, "localEulerAngles", "Vector3", &TrGetLocalEuler, &TrSetLocalEuler, nullptr);
    tb::RegisterProperty(vm, T, "localScale", "Vector3", &TrGetLocalScale, &TrSetLocalScale, nullptr);
    tb::RegisterProperty(vm, T, "lossyScale", "Vector3", &TrGetLossyScale, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "forward", "Vector3", &TrGetForward, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "right", "Vector3", &TrGetRight, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "up", "Vector3", &TrGetUp, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "gameObject", kGameObject, &TrGetGameObject, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "name", "str", &TrGetName, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "parent", "Transform?", &TrGetParent, nullptr, nullptr);
    tb::RegisterProperty(vm, T, "childCount", "int", &TrGetChildCount, nullptr, nullptr);
    tb::RegisterMethod(vm, T, "GetChild", &TrGetChild, nullptr, "(int) -> Transform?");
    tb::RegisterMethod(vm, T, "SetParent", &TrSetParent, nullptr, "(Transform?)");
    tb::RegisterMethod(vm, T, "Translate", &TrTranslate, nullptr, "(Vector3, int=)");
    tb::RegisterMethod(vm, T, "Rotate", &TrRotate, nullptr, "(Vector3, int=)");
    tb::RegisterMethod(vm, T, "RotateAround", &TrRotateAround, nullptr, "(Vector3, Vector3, float)");
    tb::RegisterMethod(vm, T, "LookAt", &TrLookAt, nullptr, "(Vector3, Vector3=)");
    tb::RegisterMethod(vm, T, "TransformPoint", &TrTransformPoint, nullptr, "(Vector3) -> Vector3");
    tb::RegisterMethod(vm, T, "InverseTransformPoint", &TrInverseTransformPoint, nullptr, "(Vector3) -> Vector3");
    tb::RegisterMethod(vm, T, "TransformDirection", &TrTransformDirection, nullptr, "(Vector3) -> Vector3");
    tb::RegisterMethod(vm, T, "InverseTransformDirection", &TrInverseTransformDirection, nullptr, "(Vector3) -> Vector3");

    // ----------------------------------------------------------------- Rigidbody
    const char* R = kRigidbody;
    tb::DefineClass(vm, R, nullptr);
    tb::RegisterProperty(vm, R, "velocity", "Vector3", &RbGetVelocity, &RbSetVelocity, nullptr);
    tb::RegisterProperty(vm, R, "mass", "float", &RbGetMass, &RbSetMass, nullptr);
    tb::RegisterProperty(vm, R, "isKinematic", "bool", &RbGetKinematic, &RbSetKinematic, nullptr);
    tb::RegisterProperty(vm, R, "drag", "float", &RbGetDrag, &RbSetDrag, nullptr);
    tb::RegisterProperty(vm, R, "angularDrag", "float", &RbGetAngularDrag, &RbSetAngularDrag, nullptr);
    tb::RegisterProperty(vm, R, "gameObject", kGameObject, &CompGetGameObject, nullptr, nullptr);
    tb::RegisterProperty(vm, R, "transform", kTransform, &CompGetTransform, nullptr, nullptr);
    tb::RegisterMethod(vm, R, "AddForce", &RbAddForce, nullptr, "(Vector3, int=)");
    tb::RegisterMethod(vm, R, "AddTorque", &RbAddTorque, nullptr, "(Vector3)");
    tb::RegisterMethod(vm, R, "MovePosition", &RbMovePosition, nullptr, "(Vector3)");
    tb::RegisterProperty(vm, R, "position", "Vector3", &RbGetPosition, &RbMovePosition, nullptr);
    tb::RegisterProperty(vm, R, "rotation", "Quaternion", &RbGetRotation, &RbSetRotation, nullptr);
    tb::RegisterProperty(vm, R, "angularVelocity", "Vector3", &RbGetAngularVelocity, &RbSetAngularVelocity, nullptr);
    tb::RegisterMethod(vm, R, "MoveRotation", &RbSetRotation, nullptr, "(Quaternion)");
    tb::RegisterMethod(vm, R, "AddRelativeForce", &RbAddRelativeForce, nullptr, "(Vector3, int=)");
    tb::RegisterMethod(vm, R, "AddForceAtPosition", &RbAddForceAtPosition, nullptr, "(Vector3, Vector3, int=)");
    tb::RegisterMethod(vm, R, "AddRelativeTorque", &RbAddRelativeTorque, nullptr, "(Vector3)");

    // --------------------------------------------------------------------- Light
    const char* L = kLight;
    tb::DefineClass(vm, L, nullptr);
    tb::RegisterProperty(vm, L, "color", "Color", &LtGetColor, &LtSetColor, nullptr);
    tb::RegisterProperty(vm, L, "intensity", "float", &LtGetIntensity, &LtSetIntensity, nullptr);
    tb::RegisterProperty(vm, L, "range", "float", &LtGetRange, &LtSetRange, nullptr);
    tb::RegisterProperty(vm, L, "spotAngle", "float", &LtGetSpotAngle, &LtSetSpotAngle, nullptr);
    tb::RegisterProperty(vm, L, "shadows", "bool", &LtGetShadows, &LtSetShadows, nullptr);
    tb::RegisterProperty(vm, L, "gameObject", kGameObject, &CompGetGameObject, nullptr, nullptr);
    tb::RegisterProperty(vm, L, "transform", kTransform, &CompGetTransform, nullptr, nullptr);

    // ----------------------------------------------------------------- Component
    const char* C = kComponent;
    tb::DefineClass(vm, C, nullptr);
    tb::RegisterProperty(vm, C, "enabled", "bool", &NcGetEnabled, &NcSetEnabled, nullptr);
    tb::RegisterProperty(vm, C, "name", "str", &NcGetName, nullptr, nullptr);
    tb::RegisterProperty(vm, C, "gameObject", kGameObject, &NcGetGameObject, nullptr, nullptr);
    tb::RegisterMethod(vm, C, "GetFloat", &NcGetFloat, nullptr, "(str, float=) -> float");
    tb::RegisterMethod(vm, C, "SetFloat", &NcSetFloat, nullptr, "(str, float)");
    tb::RegisterMethod(vm, C, "GetInt", &NcGetInt, nullptr, "(str, int=) -> int");
    tb::RegisterMethod(vm, C, "SetInt", &NcSetInt, nullptr, "(str, int)");
    tb::RegisterMethod(vm, C, "GetBool", &NcGetBool, nullptr, "(str, bool=) -> bool");
    tb::RegisterMethod(vm, C, "SetBool", &NcSetBool, nullptr, "(str, bool)");
    tb::RegisterMethod(vm, C, "GetString", &NcGetString, nullptr, "(str, str=) -> str");
    tb::RegisterMethod(vm, C, "SetString", &NcSetString, nullptr, "(str, str)");

    // --------------------------------------------------------------------- Input
    tb::RegisterFunction(vm, "Input.GetKey", &KeyQuery<KeyDown>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetKeyDown", &KeyQuery<KeyPress>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetKeyUp", &KeyQuery<KeyRelease>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetMouseButton", &MouseQuery<KeyDown>, nullptr, "(int) -> bool");
    tb::RegisterFunction(vm, "Input.GetMouseButtonDown", &MouseQuery<KeyPress>, nullptr, "(int) -> bool");
    tb::RegisterFunction(vm, "Input.GetMouseButtonUp", &MouseQuery<KeyRelease>, nullptr, "(int) -> bool");
    tb::RegisterFunction(vm, "Input.GetButton", &ButtonQuery<0>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetButtonDown", &ButtonQuery<1>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetButtonUp", &ButtonQuery<2>, nullptr, "(str) -> bool");
    tb::RegisterFunction(vm, "Input.GetAxis", &GetAxis, nullptr, "(str) -> float");
    tb::RegisterFunction(vm, "Input.GetAxisRaw", &GetAxis, nullptr, "(str) -> float");
    tb::RegisterFunction(vm, "Input.GetAxis2", &GetAxis2, nullptr, "(str) -> Vector2");

    // ------------------------------------------------------------- Debug, Physics
    tb::RegisterFunction(vm, "Debug.Log", &DebugLog, nullptr, "(any)");
    tb::RegisterFunction(vm, "Debug.LogWarning", &DebugWarn, nullptr, "(any)");
    tb::RegisterFunction(vm, "Debug.LogError", &DebugError, nullptr, "(any)");
    tb::RegisterFunction(vm, "Debug.DrawLine", &DebugDrawLine, nullptr, "(Vector3, Vector3, Color=)");
    tb::RegisterFunction(vm, "Debug.DrawRay", &DebugDrawRay, nullptr, "(Vector3, Vector3, Color=)");
    tb::RegisterFunction(vm, "Physics.Raycast", &PhysicsRaycast, nullptr, "(Vector3, Vector3, float=) -> RaycastHit?");

    // ------------------------------------------------------------------ 0.1 API
    tb::RegisterFunction(vm, "scene.find", &GoFind, nullptr, "(str) -> GameObject?");
    tb::RegisterFunction(vm, "log.info", &LegacyLog, nullptr, "(any)");
    tb::RegisterFunction(vm, "log.warn", &LegacyWarn, nullptr, "(any)");
    tb::RegisterFunction(vm, "log.error", &LegacyError, nullptr, "(any)");
    tb::RegisterProperty(vm, G, "x", "float", &LegacyX, nullptr, nullptr);
    tb::RegisterProperty(vm, G, "y", "float", &LegacyY, nullptr, nullptr);
    tb::RegisterProperty(vm, G, "z", "float", &LegacyZ, nullptr, nullptr);
    tb::RegisterMethod(vm, G, "translate", &LegacyTranslate, nullptr, "(float, float, float)");
    tb::RegisterMethod(vm, G, "setPosition", &LegacySetPosition, nullptr, "(float, float, float)");
    tb::RegisterMethod(vm, G, "rotate", &LegacyRotate, nullptr, "(float, float, float)");
    tb::RegisterMethod(vm, G, "setScale", &LegacySetScale, nullptr, "(float, float, float)");
    tb::RegisterMethod(vm, G, "isValid", &GoIsValid, nullptr, "() -> bool");
    tb::RegisterMethod(vm, G, "inputDown", &LegacyInputDown, nullptr, "(str) -> bool");
    tb::RegisterMethod(vm, G, "inputPressed", &LegacyInputPressed, nullptr, "(str) -> bool");
    tb::RegisterMethod(vm, G, "inputReleased", &LegacyInputReleased, nullptr, "(str) -> bool");
    tb::RegisterMethod(vm, G, "inputValue", &LegacyInputValue, nullptr, "(str) -> float");
    tb::RegisterMethod(vm, G, "inputX", &LegacyInputX, nullptr, "(str) -> float");
    tb::RegisterMethod(vm, G, "inputY", &LegacyInputY, nullptr, "(str) -> float");
}

} // namespace st
