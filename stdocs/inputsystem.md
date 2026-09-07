# Input System

Action maps, in the shape Unity's Input System uses. The project declares **named
actions** grouped into **named maps** once at startup; input then arrives on an
**entity** through the `stInput` native component, and the components that need it read
it as a local sibling instead of reaching into a global.

- Model: `Framework/input/InputActions.{h,cpp}`
- Component: `Framework/input/InputComponent.{h,cpp}` (`"stInput"`)
- Owner / gating: `Framework/input/InputSystem.{h,cpp}`
- Registration hook: `st::App::OnKeyRegister` (`Framework/stApp.h`)
- DevUI: `Simtary > Input Actions`, and `Simtary > Gamepad Analog` for the raw signal
- The older flat keymap is still supported — see [legacyinput.md](legacyinput.md)

---

## 1. The model

Four nouns, nested:

```
Registry ───> ActionMap ───> Action ───> Binding
every map     "Player"       "Move"      Left Stick        [Gamepad]
the game      "UI"           Vector2     W / S / A / D     [Keyboard]  (one composite)
registered                               Primary Touch     [Touchscreen]
```

| Noun | What it is |
|------|------------|
| `st::input::Registry` | Every map the game registered. Pure data — it reads no devices. The equivalent of Unity's `.inputactions` asset |
| `st::input::ActionMap` | A named group of actions: `"Player"`, `"UI"`, `"Vehicle"`. Maps are **switched, not merged** |
| `st::input::Action` | A named input with an **action type** and a **control type**, plus its bindings |
| `st::input::Binding` | One physical control feeding the action, optionally through processors, optionally as one **part** of a composite |
| `st::input::ActionRuntime` | The evaluated state of one map, for one player index. One per `stInput` component |

### Action type

What the action *means*.

| `st::input::ActionType` | Behaviour |
|---|---|
| `Button` | A press. `Down` / `Pressed` / `Released` are the interesting reads |
| `Value` | A continuous magnitude, with control disambiguation (§4) and clamping |
| `PassThrough` | Like `Value` but no disambiguation and **no clamping** — every binding is summed and handed over as-is. For raw pointer deltas, which are in pixels |

### Control type

The *shape* of the value.

| `st::input::ControlType` | Read with | Stored in |
|---|---|---|
| `Button` | `Down` / `Pressed` / `Released` | `value.x` (0 or 1) |
| `Axis` | `Value(id)` → `float` | `value.x`, clamped to `[-1, 1]` |
| `Vector2` | `Vector2(id)` → `XMFLOAT2` | `value`, magnitude clamped to 1 |

Picking a control type sets the action type for you: a `Button` control is a `Button`
action, everything else is a `Value` action. Override with `.Type(...)`.

### Binding sources

| `st::input::Source` | Produces | Device class |
|---|---|---|
| `Button` | 0 or 1 from a `wi::input::BUTTON` | derived: keyboard, mouse or gamepad |
| `GamepadAxis` | one stick axis or one trigger | Gamepad |
| `GamepadStick` | both axes of a stick, as a Vector2 | Gamepad |
| `MouseDelta` | pointer motion this frame, in **pixels**, as a Vector2 | Mouse |
| `MouseScroll` | wheel delta this frame | Mouse |
| `TouchPress` | 1 while any finger is down | Touchscreen |
| `TouchPosition` | primary touch position, logical pixels | Touchscreen |
| `TouchDelta` | primary touch motion this frame | Touchscreen |

A `wi::input::BUTTON` covers keyboard keys, mouse buttons and gamepad buttons in one
enum, including the "analog as button" entries
(`GAMEPAD_ANALOG_TRIGGER_R_AS_BUTTON`, `GAMEPAD_ANALOG_THUMBSTICK_L_AS_BUTTON_UP`, …).

---

## 2. Registering the keybinds

`st::App::OnKeyRegister(st::input::Registry&)` runs **once** during `Initialize()`,
before `RegisterScenes` and before the startup scene loads — so a scene's `stInput`
components find their map already built.

```cpp
#include "stApp.h"

class Milistry : public st::App {
protected:
    void OnKeyRegister (st::input::Registry& input) override {
        using namespace st::input;
        input.Clear();                        // start from empty (see §2.2)

        MapBuilder player = input.Map("Player");

        player.Action("Move", ControlType::Vector2)
            .Scheme("Gamepad").StickBinding(Stick::Left)
            .Scheme("Keyboard&Mouse").Composite2D('W', 'S', 'A', 'D');

        player.Action("Look", ControlType::Vector2)
            .Type(ActionType::PassThrough)    // pixel delta - do not clamp it to 1
            .Scheme("Gamepad").StickBinding(Stick::Right)
            .Scheme("Keyboard&Mouse").MouseDelta();

        player.Action("Fire")
            .Scheme("Keyboard&Mouse").Button(wi::input::MOUSE_BUTTON_LEFT)
            .Scheme("Gamepad").Button(wi::input::GAMEPAD_ANALOG_TRIGGER_R_AS_BUTTON)
            .Scheme("Touch").TouchPress();

        player.Action("Aim", ControlType::Axis)   // the analog pull, not a press
            .Scheme("Gamepad").Axis(GamepadAxis::TriggerL);

        MapBuilder ui = input.Map("UI");
        ui.Action("Cancel")
            .Button(wi::input::KEYBOARD_BUTTON_ESCAPE)
            .Button(wi::input::GAMEPAD_BUTTON_3);
    }
};
```

### 2.1 Builder reference

`Registry::Map(name)` returns a `MapBuilder`; `MapBuilder::Action(name, controlType)`
returns an `ActionBuilder`. Every `ActionBuilder` method appends a binding and returns
`*this`, so one action is one statement.

| Call | Adds |
|---|---|
| `.Button(BUTTON b, scale = 1, part = Value)` | one keyboard / mouse / gamepad button |
| `.Key('W', scale = 1, part = Value)` | the same, for a letter or digit key |
| `.StickBinding(Stick::Left \| Right, scale = 1)` | a whole stick, as a Vector2 |
| `.Axis(GamepadAxis a, scale = 1, part = Value)` | one gamepad axis or trigger |
| `.MouseDelta(scale = 1)` | pointer delta, as a Vector2 |
| `.MouseScroll(scale = 1, part = Value)` | wheel delta |
| `.TouchPress()` / `.TouchPosition(scale)` / `.TouchDelta(scale)` | touch |
| `.Composite2D(up, down, left, right)` | four buttons as one 2D vector (BUTTONs **or** chars) |
| `.Composite1D(positive, negative)` | two buttons as one signed axis |
| `.Inverted()` | negates the binding added last |
| `.Scheme("Gamepad")` | tags every binding added **after** it |
| `.PressPoint(0.5f)` | magnitude at which a Value action reads as pressed |
| `.Type(ActionType::PassThrough)` | overrides the type the control type implied |

`GamepadAxis` is `LeftX`, `LeftY`, `RightX`, `RightY`, `TriggerL`, `TriggerR`.

`Part` says which component of the value a binding feeds:

| `Part` | Meaning |
|---|---|
| `Value` | the binding drives the whole thing (default) |
| `X` / `Y` | a **scalar** source drives one component of a Vector2 action |
| `Up` / `Down` / `Left` / `Right` | one arm of a 2D composite; all four combine into **one** candidate |

### 2.2 Defaults, extending vs replacing

`InputSystem::LoadDefaults()` seeds a working `"Player"` and `"UI"` map **before** the
hook runs, so a project that registers nothing still has something to attach an
`stInput` to.

`Map()` and `Action()` return the **existing** entry when the name is already taken, so
re-registering over a default **appends** bindings rather than replacing them.

- Extend the defaults → just name the same map and action.
- Start from scratch → `input.Clear()` as the first line.
- Replace one map → `input.RemoveMap("Player")` first.

The defaults are `Move`, `Look`, `Sprint`, `Jump`, `Fire`, `Aim` (map `"Player"`) and
`Navigate`, `Submit`, `Cancel`, `Scroll` (map `"UI"`).

---

## 3. Reading it — the `stInput` component

Attach `"stInput"` to the entity that should receive input. It is a normal native
component, so it is attached from metadata and editable in the Wicked Editor:

```
NCI_0             = "stInput"   (string)
NCA_0_actionMap   = "Player"    (string)
NCA_0_playerIndex = 0           (int)
```

| Parameter | Meaning |
|---|---|
| `actionMap` | which registered map this entity listens to |
| `playerIndex` | which gamepad slot. Keyboard and mouse ignore it |

Then any component on the same entity reads it as a sibling:

```cpp
#include "input/InputComponent.h"

struct PlayerMove : wi::scene::NativeComponent {
    st::InputComponent* input = nullptr;
    int moveId = -1, jumpId = -1, aimId = -1;   // resolve the names ONCE

    void Start () override {
        input  = GetComponent<st::InputComponent>();
        if (input == nullptr) return;
        moveId = input->Find("Move");
        jumpId = input->Find("Jump");
        aimId  = input->Find("Aim");
    }

    void Update (float dt) override {
        if (input == nullptr) return;
        const XMFLOAT2 move = input->Vector2(moveId);   // x = strafe, y = forward
        const float    aim  = input->Value(aimId);      // 0..1 trigger pull
        if (input->Pressed(jumpId)) {
            // jump
        }
    }
};
```

### Reads

| Method | Returns |
|---|---|
| `Find(name)` | the action's index in the active map, or `-1` |
| `Down(id)` | held this frame |
| `Pressed(id)` | went down **this** frame |
| `Released(id)` | went up **this** frame |
| `Value(id)` | `float` — `value.x` |
| `Vector2(id)` | `XMFLOAT2` |
| `HeldTime(id)` | seconds held, 0 while up |
| `LastDevice(id)` | `DeviceClass` that last drove it — what a control-prompt HUD switches on |

Every read has a `const std::string&` overload too (`input->Down("Jump")`). It walks
the map's action list per call — fine for a one-off, use `Find()` + the `int` form in
anything that runs every frame.

An action that is not in the map, or a map that is not registered, reads as a zeroed
state rather than crashing. That is a normal condition during a scene transition.

### Switching maps

```cpp
input->SwitchMap("UI");
```

Takes effect on the next frame. The new map's states start clean, so a button held
across the switch does not arrive as a fresh `Pressed` in the new map.

### Two players

Two entities, two `stInput`, two `playerIndex` values. Nothing else changes — this is
the whole reason input lives on the entity rather than in a singleton.

---

## 4. Rules worth knowing

### Control disambiguation — bindings do not sum

Each binding group proposes a **candidate** value; the one with the largest magnitude
wins outright. A composite counts as **one** group, so all four WASD keys are a single
candidate.

Without this, stick half over (0.5) plus `W` held (1.0) would read as 1.5. Unity calls
it control disambiguation; `PassThrough` actions opt out of it and sum instead.

### Composites are normalized

A key composite at full diagonal is `(1, 1)` — magnitude 1.41, which would walk 41 %
faster diagonally than forwards. Composites are normalized to magnitude ≤ 1.

### Sampling runs in `Compute()`, not `Update()`

`stInput` evaluates in the **barriered** `Compute` stage. Every component's `Compute`
finishes before *any* component's `Update` starts, so a sibling reading in `Update`
always sees a fully evaluated, stable frame.

> Sampling in `Update` would race whichever component the job system happened to run
> first. This is the same reason `Compute` exists at all — see
> [components.cpp.md](components.cpp.md) §2.2.

### The frame snapshot

`InputSystem::Update` latches the pointer delta, the wheel and the touch list **once**,
on the main thread, via `st::input::BeginFrame`. Bindings read that copy.

SDL's relative-mouse delta is a single-consumer read — it is cleared when read — so a
binding that called `SDL_GetRelativeMouseState` itself would both race and steal the
delta from every other reader. `st::InputSystem` is the single owner of SDL relative
mouse mode, which is why it is the thing that publishes the delta.

`MouseDelta` bindings therefore read **zero unless the cursor is captured**
(`InputSystem::SetMouseCaptured(true)`).

### Device gating

The UI can own the keyboard without owning the pad. `InputSystem::Update` publishes a
per-device-class table (`st::input::Gate()`) each frame from the ImGui capture flags,
window focus, and the editor's hard input capture; the evaluator checks it per binding.

| Device class | Suppressed when |
|---|---|
| Keyboard | window unfocused, ImGui wants the keyboard, or developer tooling owns input |
| Mouse | ImGui wants the mouse, or developer tooling owns input |
| Gamepad | only by the editor's hard capture — an ImGui panel never takes the pad |
| Touchscreen | same as mouse |

The action model knows nothing about ImGui; it just reads the table.

---

## 5. DevUI

**`Simtary > Input Actions`** — three panes, the same layout as Unity's asset editor:

| Pane | Shows |
|---|---|
| Action Maps | every registered map |
| Actions | the selected map's actions as a tree, each action's bindings underneath, with a live dot that lights while the action is down |
| Action Properties | action type, control type, press point, live value bars, `down` / `pressed` / `released` / held time, last device, and a per-binding table |

Binding **structure** is registered in code, so it is not editable there — there would
be nowhere to persist it. Per-binding `scale` and `invert` **are** editable and live,
which is what tuning a look sensitivity actually needs. Those edits are not persisted.

`Preview player` picks which gamepad slot the live column reads.

**`Simtary > Gamepad Analog`** — the layer below: raw vs. deadzoned stick traces, the
2D stick gate with the deadzone ring drawn, per-axis jitter/drift verdicts, frame-time
spikes, and live deadzone sliders. Use it when the question is "is the *device* fine",
before asking whether the binding is.

---

## 6. Notes / gotchas

- **`OnKeyRegister` runs before scenes.** A map registered later (from `OnInitialize`,
  or a scene's `Load`) still works — `stInput` re-binds itself when its map appears —
  but nothing reads that action until it does.
- **`Look` should be `PassThrough`.** A mouse delta is in pixels; a `Value` action would
  clamp it to 1 and throw away everything past one pixel of motion.
- **A stick bound to an `Axis` or `Button` action contributes its `x` only.** Bind the
  axis you want (`.Axis(GamepadAxis::LeftY)`) rather than the whole stick.
- **`Pressed`/`Released` are per runtime.** Two `stInput` components on the same map and
  player each track their own edges, so both see the press. That is intentional.
- **Vector2 actions have digital edges too**, measured on the magnitude against
  `pressPoint` — which is what makes "stick as a menu direction" work.
- **The scheme string is descriptive only.** It labels the binding in the DevUI and is
  there for a future "current control scheme" filter; nothing gates on it yet.
- **Runtime binding edits are not saved.** The registry is code, not an asset. Rebinding
  UI that persists to `PlayerPrefs` is not built yet.
