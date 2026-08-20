# Native Components

A Unity-like C++ component model for the Simtary engine. Attach C++ classes to scene
entities ("GameObjects") and give them a full lifecycle —
`Awake` / `OnEnable` / `Start` / `FixedUpdate` / `Update` / `OnDisable` / `Destroy`.
Attachment is **data-driven through the engine `MetadataComponent`**, so everything
stays editable in the Wicked Editor and is serialized with the scene.

- Engine source: `Simtary/stNativeComponent.{h,cpp,_inl.h}`
- Example: `src/components/ExampleNativeComponents.cpp`
- Debug UI: `src/sysui/imnativecomponents.{h,cpp}` (see §7)

---

## 1. The metadata convention

Every attachment lives in an entity's `MetadataComponent` as typed key/value pairs. Two key
shapes are used:

| Key | Type | Meaning |
|-----|------|---------|
| `NCI_<LocalID>` | string | **N**ative **C**omponent **I**mport — attach the component named by the value |
| `NCA_<LocalID>_<ArgName>` | bool / int / float / string | **N**ative **C**omponent **A**rgument — a parameter for that instance |
| `NCE_<LocalID>` | bool | **N**ative **C**omponent **E**nabled — `false` disables the instance (default `true` when absent). See §2.1 |

- `<LocalID>` is any integer. It lets you **stack the same component** several times on one
  entity (`NCI_0`, `NCI_1`, …). Each LocalID is one instance.
- `<ArgName>` is whatever your component reads.
- The component name is **not** part of the `NCA_` key — `<LocalID>` already identifies which
  component the argument belongs to.

### Example (matches the editor screenshot)

```
NCI_0          = "MyClass1"   (string)
NCA_0_Args1    = "Hello"      (string)
NCA_0_Args2    = 123          (int)
NCA_0_Args3    = 0.542        (float)
NCA_0_Args4    = true         (bool)
```

This attaches one `MyClass1` (LocalID `0`) and hands it four parameters.

You can set these keys two ways:
- In the **Wicked Editor**, on the entity's Metadata panel (Preset = Custom, then add rows).
- In **code**, via the `MetadataComponent`:

```cpp
Entity e = scene.Entity_CreateObject("crate");
MetadataComponent& meta = scene.metadatas.Create(e);
meta.string_values.set("NCI_0", "Spinner");
meta.float_values.set("NCA_0_speed", 90.0f);
meta.string_values.set("NCA_0_axis", "y");
```

---

## 2. Writing a component

Derive from `wi::scene::NativeComponent` and override the lifecycle methods. None are required;
override only what you need.

```cpp
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiMath.h"

using namespace wi::scene;

struct Spinner : NativeComponent
{
    float speed = 45.0f;   // degrees/second (default if no NCA_ arg present)
    char  axis  = 'y';

    void Start() override
    {
        Bind(speed, "speed");                 // reads NCA_<id>_speed
        std::string a = GetString("axis", "y");
        axis = a.empty() ? 'y' : a[0];
    }

    void Update(float dt) override
    {
        TransformComponent* t = GetComponent<TransformComponent>(); // engine component, same entity
        if (t == nullptr) return;
        const float r = wi::math::DegreesToRadians(speed) * dt;
        t->RotateRollPitchYaw(XMFLOAT3(0, r, 0));
    }

    void Destroy() override {}                 // optional cleanup
};
ST_REGISTER_NATIVE_COMPONENT(Spinner)          // see §4
```

### Lifecycle

Call order per instance:

```
Awake → OnEnable → Start → (FixedUpdate*, Update) every frame → OnDisable → Destroy
```

| Method | When |
|--------|------|
| `Awake()`           | once, the first frame the instance exists — **before** `OnEnable`/`Start`, and **regardless of the enabled flag**. Use for self-setup that doesn't depend on other components being started |
| `OnEnable()`        | each time the instance goes disabled → enabled (including the first time it is seen while enabled, right after `Awake`) |
| `Start()`           | once, before the first `Update`, **only while enabled** (also fires on a `dt == 0` load tick). An instance created disabled defers `Start` until first enabled |
| `FixedUpdate(fdt)`  | 0..N times per frame on a fixed 60 Hz step (`FIXED_DT = 1/60`), only while enabled. Frame-rate independent — use for physics-style stepping |
| `Update(dt)`        | every frame while enabled and `dt > 0` |
| `OnDisable()`       | each time the instance goes enabled → disabled (and once before `Destroy` if it was still enabled) |
| `Destroy()`         | once, when the attachment is removed: `NCI_` key deleted/changed, entity removed (`Entity_Remove`), or scene cleared (`Scene::Clear`) |

`Destroy()` does **not** fire when a `Scene` is merely destructed without `Clear()`/removal — tie
real teardown to explicit removal. None of the methods are required; override only what you need.

> `FixedUpdate` is clamped to `MAX_FIXED_STEPS = 8` catch-up steps per frame to avoid the
> "spiral of death" on a frame hitch; backlog beyond that is dropped.

### 2.1 Enabling / disabling

An instance is enabled/disabled via the metadata key `NCE_<LocalID>` (bool). When the key is
**absent the instance is enabled** (default `true`). Disabling gates `Start`/`FixedUpdate`/`Update`
and fires the `OnEnable`/`OnDisable` edges. `Awake` and `Destroy` run regardless.

Read/toggle it from code (the base builds the `NCE_<id>` key for you):

```cpp
bool IsEnabled() const;        // reads NCE_<localID> (default true)
void SetEnabled(bool value);   // writes NCE_<localID> -> persisted, editable in the editor
```

`IsEnabled()` is the source of truth, re-read every frame, so toggling the key in the editor (or
via `SetEnabled`) flips the instance live and fires the matching `OnEnable`/`OnDisable`.

```cpp
void Update(float dt) override
{
    if (healthDepleted)
        SetEnabled(false);     // -> OnDisable() next tick, Update stops being called
}
```

### Context available inside a component

Set by the engine before `Start()`:

```cpp
Scene*     scene;          // owning scene
Entity     entity;         // owning entity ("GameObject")
int        localID;        // the <LocalID> from NCI_<LocalID>
std::string componentName; // registered name (NCI_ value)
```

---

## 3. Reading parameters

Parameters come from the entity's metadata under `NCA_<localID>_<name>`. The base
class builds that key for you — you just pass the `<name>`.

```cpp
bool        GetBool  (const std::string& name, bool def = false) const;
int         GetInt   (const std::string& name, int def = 0) const;
float       GetFloat (const std::string& name, float def = 0.0f) const;
std::string GetString(const std::string& name, const std::string& def = "") const;
bool        HasParam (const std::string& name) const;   // any type present?
```

If the key is missing, the default is returned (so omitting an `NCA_` row keeps your field's
initializer).

### `Bind` — one-liner field binding

`Bind(field, "name")` picks the right getter from the field's type and assigns it, using the
field's current value as the default:

```cpp
float       speed = 1.0f;   Bind(speed, "speed");   // = GetFloat("speed", speed)
int         count = 3;      Bind(count, "count");   // = GetInt("count", count)
bool        on    = false;  Bind(on,    "on");      // = GetBool("on", on)
std::string tag   = "x";    Bind(tag,   "tag");     // = GetString("tag", tag)
```

Supported field types: `bool`, integral, `enum`, floating-point, `std::string`. Call it from
`Start()`.

> Params are read on demand from metadata, so editing a value in the editor and reading it again
> (e.g. re-calling `Bind` from `Update`) picks up the new value live. By default most components
> bind once in `Start()`.

---

## 4. Registering a component

Registration maps the metadata string (`NCI_` value) to your C++ type. Put the macro in a `.cpp`
file at file scope; it self-registers at static-init time.

```cpp
ST_REGISTER_NATIVE_COMPONENT(Spinner)               // metadata name == type name ("Spinner")
ST_REGISTER_NATIVE_COMPONENT_AS(MyType, "Display")  // metadata name differs from type name
```

If an `NCI_` value names a component that was never registered, the engine logs a warning
(`wi::backlog`, Warning level) and skips it:

```
NativeComponent: 'Foo' (NCI_0) is not registered. Did you add ST_REGISTER_NATIVE_COMPONENT(Foo)?
```

> The `.cpp` containing the macro must actually be linked into the final executable. In Milistry,
> `src/**/*.cpp` is globbed into the `Milistry` target, so dropping a file under `src/` is enough.

---

## 5. Talking to other components — `GetComponent`

All lookups are on the **same entity** ("GameObject").

```cpp
template<typename T> T* GetComponent();          // engine OR native component
template<typename T> T* GetComponentByID(int id);// native only: a specific LocalID
template<typename T> void GetComponents(wi::vector<T*>& out); // native only: all of type T
```

`GetComponent<T>()` resolves both worlds:

- **Engine component** (e.g. `TransformComponent`, `MeshComponent`, `LightComponent`,
  `CameraComponent`, `MaterialComponent`, `ObjectComponent`, …): returns the component attached to
  this entity, or `nullptr`.
- **Native component** (any `T` deriving `NativeComponent`): returns the first native instance of
  that type on this entity, or `nullptr`.

```cpp
void Start() override
{
    if (TransformComponent* t = GetComponent<TransformComponent>()) { /* engine */ }
    if (Spinner* s = GetComponent<Spinner>())                       { /* native */ }
}
```

### Stacked components

When the same component is attached multiple times (different LocalIDs), pick one by id or grab
them all:

```cpp
Spinner* first = GetComponentByID<Spinner>(0);   // the NCI_0 instance
wi::vector<Spinner*> all;
GetComponents<Spinner>(all);                     // every Spinner on this entity
```

During `Start()`, every instance for the entity is already constructed (creation happens before any
`Start`), so cross-component lookups in `Start()` are safe regardless of LocalID order.

> If `GetComponent<T>()` is called with a `T` that is neither a `NativeComponent` subclass nor a
> mapped engine type, you get a compile error. Add a mapping line in `GetEngineComponentPtr`
> (`Simtary/stNativeComponent_inl.h`) to expose more engine component types.

---

## 6. How it runs (engine integration)

- Runtime state lives in `Scene::nativeComponents` (a `NativeComponentManager`). It is **not
  serialized** — it is rebuilt from the (serialized) `MetadataComponent` every `Update`.
- `Scene::RunNativeComponentUpdateSystem` runs once per `Scene::Update`, right after the Lua script
  system. Each tick it:
  1. **Prunes** instances whose `NCI_` key was removed or changed → fires `OnDisable()` (if enabled) then `Destroy()`.
  2. **Creates** instances for new `NCI_` keys → fills context, leaves them un-awoken/unstarted.
  3. **Drives the lifecycle** on every instance: `Awake()` (once) → enable/disable edge from
     `NCE_<id>` (`OnEnable`/`OnDisable`) → if enabled: `Start()` (once) → `FixedUpdate()` for each
     accumulated fixed step → `Update(dt)` (when `dt > 0`). The fixed-step accumulator is shared
     across all instances (one advance per frame).
- The system is **single-threaded** on purpose: your `Update` may freely touch the scene.
- `Entity_Remove` and `Scene::Clear` fire `Destroy()` on the affected instances.
- `Scene::Merge` moves native instances from the merged scene into the target (entities keep their
  IDs through a merge). This is why `Start()` runs exactly once across a `LoadModel` + `Merge`,
  even though `LoadModel` updates a temp scene before merging.

---

## 7. Debug / inspector UI

A component can expose an inspector by overriding `DrawDebug()` and drawing ImGui widgets bound to
its **live members**. It is called while an ImGui window is already open — do **not** `Begin()`/`End()`.

```cpp
#include "imgui.h"
struct Spinner : NativeComponent
{
    float speed = 45.0f;
    char  axis  = 'y';
    // ... Start/Update ...
    void DrawDebug() override
    {
        ImGui::DragFloat("speed (deg/s)", &speed, 1.0f);
        int cur = (axis == 'x') ? 0 : (axis == 'z') ? 2 : 1;
        if (ImGui::Combo("axis", &cur, "x\0y\0z\0"))
            axis = "xyz"[cur];
    }
};
```

Editing a widget mutates the live member, so the change takes effect on the next `Update()`. It is
**not** written back to metadata, so it is **not persisted** on scene save — for a persisted tweak,
also write the matching `NCA_`/`NCE_` key (e.g. `SetEnabled`, or `scene->metadatas...set(...)`).
The base `DrawDebug()` does nothing, so a component appears in the inspector only if it opts in.

### Drawing the whole scene's components

`src/sysui/imnativecomponents.h` walks every live instance and calls each one's `DrawDebug()`,
grouped per entity, with an `enabled` checkbox (writes `NCE_<id>` via `SetEnabled`) per instance.

```cpp
#include "sysui/imnativecomponents.h"

void Scene2::OnGUI()
{
    ImGui::Begin("Scene Menu");
    // ...
    ImGui::SeparatorText("Native Components");
    NativeComponentsGUI(wi::scene::GetScene()); // widgets into the current window (no Begin/End)
    ImGui::End();
}
```

| Function | Use |
|----------|-----|
| `NativeComponentsGUI(scene)`         | emit the tree into the **current** ImGui window |
| `NativeComponentsWindow(scene, p_open)` | open a standalone `"Native Components"` window and draw the tree inside |

---

## 8. Notes / gotchas

- **RTTI is off** engine-wide (`/GR-`, `-fno-rtti`). Type identity uses `GetNativeTypeID<T>()`
  (address-based), not `typeid`/`dynamic_cast`. This is internal; you don't call it directly.
- **Exceptions are off** (`/EHsc-`, `-fno-exceptions`). Don't throw from lifecycle methods.
- **No serialization of component state.** Anything you want persisted must come from / go to
  metadata (or other engine components). Instances are recreated from metadata after load.
- **Enabled state persists, runtime state does not.** `NCE_<id>` is metadata, so an instance
  disabled in the editor stays disabled across save/load. After load a disabled instance has not
  yet run `Start`; it does so the first time it is enabled. `Awake` runs once regardless of enabled.
- **`FixedUpdate` is global-stepped.** All instances share one fixed accumulator (advanced once per
  frame), so a disabled instance does not bank up steps to fire later when re-enabled.
- One component per `LocalID`. Reusing a `LocalID` for a different `NCI_` value swaps the
  component (old one gets `Destroy()`, new one gets `Start()`).
- The `NCI_` value must match the **registered** name, not necessarily the C++ type name (see
  `ST_REGISTER_NATIVE_COMPONENT_AS`). `NCA_` keys carry no component name, only the LocalID.

---

## 9. Full example

`src/components/ExampleNativeComponents.cpp`:

```cpp
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiMath.h"
#include "wiBacklog.h"
#include "imgui.h"

using namespace wi::scene;

struct Spinner : NativeComponent
{
    float speed = 45.0f;
    char  axis  = 'y';

    void Awake() override     { wilog("[Spinner] Awake localID=%d", localID); }
    void OnEnable() override   { wilog("[Spinner] OnEnable localID=%d", localID); }
    void OnDisable() override  { wilog("[Spinner] OnDisable localID=%d", localID); }

    void Start() override
    {
        Bind(speed, "speed");
        std::string a = GetString("axis", "y");
        axis = a.empty() ? 'y' : a[0];
    }
    void Update(float dt) override
    {
        TransformComponent* t = GetComponent<TransformComponent>();
        if (!t) return;
        const float r = wi::math::DegreesToRadians(speed) * dt;
        switch (axis)
        {
        case 'x': t->RotateRollPitchYaw(XMFLOAT3(r, 0, 0)); break;
        case 'z': t->RotateRollPitchYaw(XMFLOAT3(0, 0, r)); break;
        default:  t->RotateRollPitchYaw(XMFLOAT3(0, r, 0)); break;
        }
    }
    void DrawDebug() override
    {
        ImGui::DragFloat("speed (deg/s)", &speed, 1.0f);
        int cur = (axis == 'x') ? 0 : (axis == 'z') ? 2 : 1;
        if (ImGui::Combo("axis", &cur, "x\0y\0z\0")) axis = "xyz"[cur];
    }
};
ST_REGISTER_NATIVE_COMPONENT(Spinner)

struct MyClass1 : NativeComponent
{
    std::string Args1 = "default";
    int   Args2 = 0;
    float Args3 = 0.0f;
    bool  Args4 = false;
    int   fixedTicks = 0;

    void Awake() override      { wilog("[MyClass1] Awake localID=%d", localID); }
    void OnEnable() override    { wilog("[MyClass1] OnEnable localID=%d", localID); }
    void OnDisable() override   { wilog("[MyClass1] OnDisable localID=%d", localID); }
    void FixedUpdate(float) override { ++fixedTicks; } // ~60/s while enabled

    void Start() override
    {
        Bind(Args1, "Args1");
        Bind(Args2, "Args2");
        Bind(Args3, "Args3");
        Bind(Args4, "Args4");
        if (Spinner* s = GetComponent<Spinner>())
            wilog("[MyClass1] found Spinner at %.1f deg/s", s->speed);
    }
    void DrawDebug() override
    {
        ImGui::DragInt("Args2", &Args2);
        ImGui::DragFloat("Args3", &Args3, 0.01f);
        ImGui::Checkbox("Args4", &Args4);
        ImGui::Text("fixedTicks: %d", fixedTicks);
    }
    void Destroy() override { wilog("[MyClass1] localID=%d destroyed", localID); }
};
ST_REGISTER_NATIVE_COMPONENT(MyClass1)
```

Attach from the editor (or code) and it runs — no Milistry-side wiring needed.
