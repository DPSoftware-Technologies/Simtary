# Legacy Input (built-in keymap)

The original `st::InputSystem` keymap: **one flat namespace of named actions**, read
from a singleton. It predates the action-map system and is kept working unchanged,
because scenes and components are written against it.

- Source: `Framework/input/InputSystem.{h,cpp}`
- **New code should use [inputsystem.md](inputsystem.md) instead** — action maps, per
  entity, with control types and a player index.

Both halves run side by side over the same devices and do not interfere.

---

## 1. What it is

An action is a name with two lists of bindings — `positive` and `negative`. A binding is
either a digital button (`wi::input::BUTTON`) or one gamepad analog axis, with a `scale`.

```
"MoveX"   positive: D, ThumbLX        negative: A
"Sprint"  positive: LShift, RB/R1
```

There are no maps, no control types, no player index, and no per-entity ownership —
everything is one global list read through `st::InputSystem::Get()`.

---

## 2. Setting it up

`InputSystem::LoadDefaults()` installs the built-in keymap during `st::App::Initialize`.
Rebind from anywhere after that:

```cpp
#include "input/InputSystem.h"

st::InputSystem& in = st::InputSystem::Get();

in.ClearAction("Fire");
in.BindButton("Fire", wi::input::MOUSE_BUTTON_LEFT);
in.BindButton("Fire", wi::input::GAMEPAD_ANALOG_TRIGGER_R_AS_BUTTON);

in.BindButton("MoveX", (wi::input::BUTTON)'D');
in.BindButton("MoveX", (wi::input::BUTTON)'A', /*negative*/ true);
in.BindAnalog("MoveX", st::InputBinding::Analog::ThumbLX);
```

| Method | Does |
|---|---|
| `LoadDefaults()` | install the built-in keymap (called for you at startup) |
| `ClearAction(name)` | drop every binding on an action |
| `BindButton(name, button, negative = false)` | add a digital binding |
| `BindAnalog(name, analog, scale = 1, negative = false)` | add an analog-axis binding |
| `Find(name)` | the `InputAction*`, or `nullptr` |

`InputBinding::Analog` is `ThumbLX`, `ThumbLY`, `ThumbRX`, `ThumbRY`, `TriggerL`,
`TriggerR`.

### The default keymap

| Action | Bindings |
|---|---|
| `MoveX` | `D` / `A` (negative), left stick X |
| `MoveY` | `W` / `S` (negative), left stick Y |
| `LookX` | right stick X |
| `LookY` | right stick Y |
| `Sprint` | `LShift`, RB / R1 |
| `Jump` | `Space`, A / cross |
| `ReleaseCursor` | `Escape` |
| `CaptureCursor` | left mouse button |
| `ToggleInteractive` | `I` |
| `LookDrag` | right mouse button |

---

## 3. Reading it

```cpp
st::InputSystem& in = st::InputSystem::Get();

const XMFLOAT2 move = in.MoveVector();     // (MoveX, MoveY)
const bool     run  = in.Down("Sprint");
const bool     jump = in.Pressed("Jump");
const float    x    = in.Axis("MoveX");    // [-1, 1]
```

| Method | Returns |
|---|---|
| `Down(action)` | any bound source active (an analog source counts past 0.5) |
| `Pressed(action)` | a bound **button** went down this frame |
| `Released(action)` | a bound **button** went up this frame |
| `Axis(action)` | `float` in `[-1, 1]`: sum of positive bindings minus negative |
| `MoveVector()` | `(Axis("MoveX"), Axis("MoveY"))` |
| `LookVector()` | `(Axis("LookX"), Axis("LookY"))` |

> `Pressed` / `Released` ignore analog bindings — an axis has no edge event. Only the
> digital half of an action reports edges.

Reads are evaluated **on demand**, inside the call, not cached per frame.

---

## 4. Mouse capture and gating

This half of `InputSystem` is **not** legacy — it is the single owner of SDL relative
mouse mode and of the device gating both systems use, and the action-map system depends
on it. Keep using it.

```cpp
in.SetMouseCaptured(true);          // FPS look: hide + lock the cursor
const XMFLOAT2 d = in.MouseDelta(); // pixels since last frame, 0 when not captured
```

| Method | Purpose |
|---|---|
| `SetMouseCaptured(bool)` / `IsMouseCaptured()` | FPS cursor lock. One owner, so a scene and the editor cannot fight over it |
| `MouseDelta()` | pointer delta in pixels; zero unless captured |
| `SetUIMouseLook(bool)` | developer tooling drives a camera — gates all game keyboard/mouse and takes the cursor |
| `SetUIMouseConfined(bool)` | confine a **visible** cursor to the window (gizmo drags); does not gate input |
| `SetUIInputCapture(bool)` | Editor mode's hard gate — the game receives no keyboard, mouse **or** gamepad |
| `SetGameViewportInput(kb, mouse)` | hands input back to the game inside the editor's Game Viewport, bypassing ImGui's `WantCapture*` |

Source gating, for the legacy actions:

| Source | Ignored when |
|---|---|
| Keyboard | window unfocused, or ImGui wants the keyboard |
| Mouse button | ImGui wants the mouse |
| Gamepad (button and analog) | never — except under `SetUIInputCapture` |

While the cursor is captured the player is driving the game, not typing, so
keyboard/mouse reach the game regardless of `WantCapture*`.

---

## 5. Migrating

Roughly:

| Legacy | Action maps |
|---|---|
| `in.MoveVector()` | `input->Vector2(moveId)` — one `Vector2` action, not two scalar axes |
| `in.Down("Sprint")` | `input->Down(sprintId)` |
| `in.Axis("MoveX")` | one `ControlType::Axis` action |
| `BindButton` / `BindAnalog` at runtime | declared in `st::App::OnKeyRegister` |
| one global player | one `stInput` component per player entity |

The two are **not** semantically identical, which is why the legacy path was not
reimplemented on top of the new evaluator:

- `Axis()` **sums** positive minus negative. A `Value` action picks the single loudest
  candidate instead (§4 of [inputsystem.md](inputsystem.md)). Stick plus key reads 1.5
  in the old system and 1.0 in the new one.
- `MoveVector()` is two independent axes, so a keyboard diagonal is magnitude 1.41. A
  `Vector2` composite is normalized to 1.

Migrate per component and check the feel, rather than swapping wholesale.
