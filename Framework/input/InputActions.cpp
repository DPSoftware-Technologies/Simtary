#include "input/InputActions.h"

#include <algorithm>
#include <cmath>

namespace st::input {

// ---------------------------------------------------------------------------
// Frame snapshot
// ---------------------------------------------------------------------------
namespace {

// Everything a binding can read that is NOT a straight wi::input query, latched once
// per frame on the main thread. An ActionRuntime is evaluated from a job thread
// (st::InputComponent does its sampling in the barriered Compute stage), and SDL's
// touch list and the relative-mouse delta are single-consumer reads - taking them
// per binding would both race and double-consume.
struct FrameSnapshot {
	XMFLOAT2 pointerDelta = XMFLOAT2(0, 0);
	float    scroll       = 0.0f;
	bool     touchDown    = false;
	XMFLOAT2 touchPos     = XMFLOAT2(0, 0);
	XMFLOAT2 touchDelta   = XMFLOAT2(0, 0);
	XMFLOAT2 touchPrev    = XMFLOAT2(0, 0);
	bool     touchWasDown = false;
};

FrameSnapshot frame;
DeviceGate    gate;

float ClampUnit(float v) { return std::max(-1.0f, std::min(1.0f, v)); }

// Read one gamepad axis, already deadzoned and rescaled by the backend.
float ReadGamepadAxis(GamepadAxis a, int player) {
	using namespace wi::input;
	switch (a) {
	case GamepadAxis::LeftX:    return GetAnalog(GAMEPAD_ANALOG_THUMBSTICK_L, player).x;
	case GamepadAxis::LeftY:    return GetAnalog(GAMEPAD_ANALOG_THUMBSTICK_L, player).y;
	case GamepadAxis::RightX:   return GetAnalog(GAMEPAD_ANALOG_THUMBSTICK_R, player).x;
	case GamepadAxis::RightY:   return GetAnalog(GAMEPAD_ANALOG_THUMBSTICK_R, player).y;
	case GamepadAxis::TriggerL: return GetAnalog(GAMEPAD_ANALOG_TRIGGER_L, player).x;
	case GamepadAxis::TriggerR: return GetAnalog(GAMEPAD_ANALOG_TRIGGER_R, player).x;
	}
	return 0.0f;
}

XMFLOAT2 ReadStick(Stick s, int player) {
	const XMFLOAT4 v = wi::input::GetAnalog(
		s == Stick::Left ? wi::input::GAMEPAD_ANALOG_THUMBSTICK_L
		                 : wi::input::GAMEPAD_ANALOG_THUMBSTICK_R, player);
	return XMFLOAT2(v.x, v.y);
}

// One binding's contribution, before the part is applied. `.x` for a scalar source,
// both components for a Vector2 source.
XMFLOAT2 ReadRaw(const Binding& b, int player) {
	switch (b.source) {
	case Source::Button:
		// wi::input::Down takes the player index for gamepad buttons and ignores it
		// for keyboard/mouse, which is exactly the behaviour wanted here.
		return XMFLOAT2(wi::input::Down(b.button, player) ? 1.0f : 0.0f, 0.0f);
	case Source::GamepadAxis:
		return XMFLOAT2(ReadGamepadAxis(b.axis, player), 0.0f);
	case Source::GamepadStick:
		return ReadStick(b.stick, player);
	case Source::MouseDelta:
		return frame.pointerDelta;
	case Source::MouseScroll:
		return XMFLOAT2(frame.scroll, 0.0f);
	case Source::TouchPosition:
		return frame.touchPos;
	case Source::TouchDelta:
		return frame.touchDelta;
	case Source::TouchPress:
		return XMFLOAT2(frame.touchDown ? 1.0f : 0.0f, 0.0f);
	}
	return XMFLOAT2(0, 0);
}

// A candidate is one binding group's proposed value for the action. A composite's
// four arms produce ONE candidate between them; every other binding produces its own.
struct Candidate {
	XMFLOAT2 value = XMFLOAT2(0, 0);
	DeviceClass device = DeviceClass::Keyboard;
	bool used = false;
};

float Magnitude(const XMFLOAT2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

} // namespace

DeviceGate& Gate() { return gate; }

void BeginFrame(float /*dt*/, const XMFLOAT2& pointerDelta, float scrollDelta) {
	frame.pointerDelta = pointerDelta;
	frame.scroll       = scrollDelta;

	const wi::vector<wi::input::Touch>& touches = wi::input::GetTouches();
	const bool down = !touches.empty();
	const XMFLOAT2 pos = down ? touches[0].pos : frame.touchPrev;

	// Delta only across two frames that both had a touch: the jump from "no touch" to
	// "touch here" is a teleport, not a drag, and feeding it to a look action throws
	// the camera across the world on every tap.
	frame.touchDelta = (down && frame.touchWasDown)
		? XMFLOAT2(pos.x - frame.touchPrev.x, pos.y - frame.touchPrev.y)
		: XMFLOAT2(0, 0);
	frame.touchPos     = pos;
	frame.touchPrev    = pos;
	frame.touchDown    = down;
	frame.touchWasDown = down;
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------
DeviceClass Binding::Device() const {
	switch (source) {
	case Source::GamepadAxis:
	case Source::GamepadStick:
		return DeviceClass::Gamepad;
	case Source::MouseDelta:
	case Source::MouseScroll:
		return DeviceClass::Mouse;
	case Source::TouchPosition:
	case Source::TouchDelta:
	case Source::TouchPress:
		return DeviceClass::Touch;
	case Source::Button:
		break;
	}
	if (wi::input::IsGamepadButton(button))
		return DeviceClass::Gamepad;
	if (button >= wi::input::MOUSE_BUTTON_LEFT && button <= wi::input::MOUSE_SCROLL_AS_BUTTON_DOWN)
		return DeviceClass::Mouse;
	return DeviceClass::Keyboard;
}

std::string Binding::Describe() const {
	std::string body;
	switch (source) {
	case Source::Button:        body = ButtonName(button); break;
	case Source::GamepadAxis:
		switch (axis) {
		case GamepadAxis::LeftX:    body = "Left Stick X"; break;
		case GamepadAxis::LeftY:    body = "Left Stick Y"; break;
		case GamepadAxis::RightX:   body = "Right Stick X"; break;
		case GamepadAxis::RightY:   body = "Right Stick Y"; break;
		case GamepadAxis::TriggerL: body = "Left Trigger"; break;
		case GamepadAxis::TriggerR: body = "Right Trigger"; break;
		}
		break;
	case Source::GamepadStick:  body = (stick == Stick::Left) ? "Left Stick" : "Right Stick"; break;
	case Source::MouseDelta:    body = "Delta"; break;
	case Source::MouseScroll:   body = "Scroll"; break;
	case Source::TouchPosition: body = "Primary Touch Position"; break;
	case Source::TouchDelta:    body = "Primary Touch Drag"; break;
	case Source::TouchPress:    body = "Primary Touch/Tap"; break;
	}

	switch (part) {
	case Part::Up:    body += " (Up)"; break;
	case Part::Down:  body += " (Down)"; break;
	case Part::Left:  body += " (Left)"; break;
	case Part::Right: body += " (Right)"; break;
	case Part::X:     body += " -> X"; break;
	case Part::Y:     body += " -> Y"; break;
	case Part::Value: break;
	}
	if (invert)
		body += " (inverted)";

	body += " [";
	body += ToString(Device());
	body += "]";
	return body;
}

// ---------------------------------------------------------------------------
// ActionMap / Registry
// ---------------------------------------------------------------------------
int ActionMap::IndexOf(const std::string& action) const {
	for (int i = 0; i < (int)actions.size(); ++i) {
		if (actions[i].name == action)
			return i;
	}
	return -1;
}
const Action* ActionMap::Find(const std::string& action) const {
	const int i = IndexOf(action);
	return i < 0 ? nullptr : &actions[i];
}

MapBuilder Registry::Map(const std::string& name) {
	const int i = MapIndex(name);
	if (i >= 0)
		return MapBuilder(&maps_[i]);
	maps_.push_back(ActionMap{});
	maps_.back().name = name;
	return MapBuilder(&maps_.back());
}
int Registry::MapIndex(const std::string& name) const {
	for (int i = 0; i < (int)maps_.size(); ++i) {
		if (maps_[i].name == name)
			return i;
	}
	return -1;
}
ActionMap* Registry::FindMap(const std::string& name) {
	const int i = MapIndex(name);
	return i < 0 ? nullptr : &maps_[i];
}
const ActionMap* Registry::FindMap(const std::string& name) const {
	const int i = MapIndex(name);
	return i < 0 ? nullptr : &maps_[i];
}
void Registry::RemoveMap(const std::string& name) {
	const int i = MapIndex(name);
	if (i >= 0)
		maps_.erase(maps_.begin() + i);
}
ActionId Registry::Resolve(const std::string& map, const std::string& action) const {
	ActionId id;
	id.map = MapIndex(map);
	if (id.map >= 0)
		id.action = maps_[id.map].IndexOf(action);
	return id;
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------
ActionBuilder MapBuilder::Action(const std::string& name, ControlType control) {
	const int existing = map_->IndexOf(name);
	if (existing >= 0)
		return ActionBuilder(&map_->actions[existing]);

	map_->actions.push_back(st::input::Action{});
	st::input::Action& a = map_->actions.back();
	a.name = name;
	a.controlType = control;
	// A Button control is a Button action; anything with a magnitude is a Value.
	//	Matches Unity's default when you pick a control type in the asset editor.
	a.actionType = (control == ControlType::Button) ? ActionType::Button : ActionType::Value;
	return ActionBuilder(&a);
}

ActionBuilder& ActionBuilder::push(Binding b) {
	b.scheme = scheme_;
	action_->bindings.push_back(std::move(b));
	return *this;
}
ActionBuilder& ActionBuilder::Scheme(const char* name) { scheme_ = name ? name : ""; return *this; }
ActionBuilder& ActionBuilder::PressPoint(float p) { action_->pressPoint = p; return *this; }
ActionBuilder& ActionBuilder::Type(ActionType t) { action_->actionType = t; return *this; }

ActionBuilder& ActionBuilder::Button(wi::input::BUTTON b, float scale, Part part) {
	Binding x; x.source = Source::Button; x.button = b; x.scale = scale; x.part = part;
	return push(x);
}
ActionBuilder& ActionBuilder::Key(char c, float scale, Part part) {
	return Button((wi::input::BUTTON)(unsigned char)c, scale, part);
}
ActionBuilder& ActionBuilder::StickBinding(Stick s, float scale) {
	Binding x; x.source = Source::GamepadStick; x.stick = s; x.scale = scale;
	return push(x);
}
ActionBuilder& ActionBuilder::Axis(GamepadAxis a, float scale, Part part) {
	Binding x; x.source = Source::GamepadAxis; x.axis = a; x.scale = scale; x.part = part;
	return push(x);
}
ActionBuilder& ActionBuilder::MouseDelta(float scale) {
	Binding x; x.source = Source::MouseDelta; x.scale = scale;
	return push(x);
}
ActionBuilder& ActionBuilder::MouseScroll(float scale, Part part) {
	Binding x; x.source = Source::MouseScroll; x.scale = scale; x.part = part;
	return push(x);
}
ActionBuilder& ActionBuilder::TouchDelta(float scale) {
	Binding x; x.source = Source::TouchDelta; x.scale = scale;
	return push(x);
}
ActionBuilder& ActionBuilder::TouchPosition(float scale) {
	Binding x; x.source = Source::TouchPosition; x.scale = scale;
	return push(x);
}
ActionBuilder& ActionBuilder::TouchPress() {
	Binding x; x.source = Source::TouchPress;
	return push(x);
}
ActionBuilder& ActionBuilder::Composite2D(wi::input::BUTTON up, wi::input::BUTTON down,
                                          wi::input::BUTTON left, wi::input::BUTTON right) {
	Button(up,    1.0f, Part::Up);
	Button(down,  1.0f, Part::Down);
	Button(left,  1.0f, Part::Left);
	Button(right, 1.0f, Part::Right);
	return *this;
}
ActionBuilder& ActionBuilder::Composite2D(char up, char down, char left, char right) {
	return Composite2D((wi::input::BUTTON)(unsigned char)up, (wi::input::BUTTON)(unsigned char)down,
	                   (wi::input::BUTTON)(unsigned char)left, (wi::input::BUTTON)(unsigned char)right);
}
ActionBuilder& ActionBuilder::Composite1D(wi::input::BUTTON positive, wi::input::BUTTON negative) {
	Button(positive, 1.0f, Part::Right);
	Button(negative, 1.0f, Part::Left);
	return *this;
}
ActionBuilder& ActionBuilder::Composite1D(char positive, char negative) {
	return Composite1D((wi::input::BUTTON)(unsigned char)positive,
	                   (wi::input::BUTTON)(unsigned char)negative);
}
ActionBuilder& ActionBuilder::Inverted() {
	if (!action_->bindings.empty())
		action_->bindings.back().invert = true;
	return *this;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
ActionState EvaluateAction(const Action& action, int playerIndex, float dt,
                           const ActionState& previous, bool ignoreGate) {
	ActionState out;

	// One candidate per binding group. The composite arms share candidates[0]; every
	//	other binding gets its own. Reserving the composite slot up front keeps the
	//	loop branch-free about which slot an arm belongs to.
	Candidate composite;
	std::vector<Candidate> candidates;
	candidates.reserve(action.bindings.size());

	for (const Binding& b : action.bindings) {
		const DeviceClass device = b.Device();
		if (!ignoreGate && gate[device])
			continue; // the UI owns this device this frame

		XMFLOAT2 raw = ReadRaw(b, playerIndex);
		const float sign = b.invert ? -b.scale : b.scale;
		raw.x *= sign;
		raw.y *= sign;

		switch (b.part) {
		case Part::Value: {
			Candidate c;
			c.device = device;
			c.used = true;
			if (action.controlType == ControlType::Vector2) {
				c.value = raw;
			} else {
				// A Vector2 source feeding a scalar action: take x. That is what a
				// stick bound to a 1D action means.
				c.value = XMFLOAT2(raw.x, 0.0f);
			}
			candidates.push_back(c);
			break;
		}
		case Part::X:
		case Part::Y: {
			Candidate c;
			c.device = device;
			c.used = true;
			if (b.part == Part::X) c.value = XMFLOAT2(raw.x, 0.0f);
			else                   c.value = XMFLOAT2(0.0f, raw.x);
			candidates.push_back(c);
			break;
		}
		// Composite arms accumulate into ONE candidate, so a full-tilt stick and a
		//	held key never add up past full deflection.
		case Part::Up:    composite.value.y += raw.x; composite.used = true; composite.device = device; break;
		case Part::Down:  composite.value.y -= raw.x; composite.used = true; composite.device = device; break;
		case Part::Left:  composite.value.x -= raw.x; composite.used = true; composite.device = device; break;
		case Part::Right: composite.value.x += raw.x; composite.used = true; composite.device = device; break;
		}
	}

	if (composite.used) {
		composite.value.x = ClampUnit(composite.value.x);
		composite.value.y = ClampUnit(composite.value.y);
		// Diagonal on a key composite is (1,1) - magnitude 1.41, which walks faster
		//	diagonally than forwards. Unity normalizes the composite for exactly this.
		const float m = Magnitude(composite.value);
		if (m > 1.0f) {
			composite.value.x /= m;
			composite.value.y /= m;
		}
		candidates.push_back(composite);
	}

	if (action.actionType == ActionType::PassThrough) {
		// No disambiguation, no clamp: sum everything and hand it over. This is what a
		//	raw pointer delta wants - it is in pixels and clamping it to 1 would be
		//	nonsense.
		for (const Candidate& c : candidates) {
			out.value.x += c.value.x;
			out.value.y += c.value.y;
			if (Magnitude(c.value) > 0.0f)
				out.lastDevice = c.device;
		}
	} else {
		// Control disambiguation: the loudest candidate wins outright.
		float best = -1.0f;
		for (const Candidate& c : candidates) {
			const float m = Magnitude(c.value);
			if (m > best) {
				best = m;
				out.value = c.value;
				out.lastDevice = c.device;
			}
		}
		if (action.controlType != ControlType::Vector2) {
			out.value.x = ClampUnit(out.value.x);
			out.value.y = 0.0f;
		} else {
			const float m = Magnitude(out.value);
			if (m > 1.0f) {
				out.value.x /= m;
				out.value.y /= m;
			}
		}
	}

	// Digital edges. A Vector2 action still has them - "is the stick pushed at all" -
	//	measured on the magnitude, which is what makes "stick as a menu direction" work.
	const float magnitude = (action.controlType == ControlType::Vector2)
		? Magnitude(out.value)
		: std::fabs(out.value.x);
	out.down     = magnitude >= action.pressPoint;
	out.pressed  = out.down && !previous.down;
	out.released = !out.down && previous.down;
	out.heldTime = out.down ? previous.heldTime + dt : 0.0f;
	if (!out.down && candidates.empty())
		out.lastDevice = previous.lastDevice;

	return out;
}

// ---------------------------------------------------------------------------
// ActionRuntime
// ---------------------------------------------------------------------------
void ActionRuntime::Bind(const Registry* registry, const std::string& mapName, int playerIndex) {
	const int newIndex = (registry != nullptr) ? registry->MapIndex(mapName) : -1;
	const bool sameMap = (registry_ == registry && mapIndex_ == newIndex && mapName_ == mapName);

	registry_    = registry;
	mapName_     = mapName;
	mapIndex_    = newIndex;
	playerIndex_ = playerIndex;

	const size_t want = (mapIndex_ >= 0) ? registry->Maps()[mapIndex_].actions.size() : 0;
	if (!sameMap || states_.size() != want) {
		states_.assign(want, ActionState{});
	}
}

const ActionMap* ActionRuntime::Map() const {
	if (registry_ == nullptr || mapIndex_ < 0)
		return nullptr;
	return &registry_->Maps()[mapIndex_];
}

void ActionRuntime::Evaluate(float dt, bool ignoreGate) {
	const ActionMap* map = Map();
	if (map == nullptr)
		return;
	// A map edited at runtime (the DevUI tree adds a binding) can change size under a
	//	bound runtime; resize before indexing rather than after crashing.
	if (states_.size() != map->actions.size())
		states_.assign(map->actions.size(), ActionState{});

	for (size_t i = 0; i < map->actions.size(); ++i) {
		states_[i] = EvaluateAction(map->actions[i], playerIndex_, dt, states_[i], ignoreGate);
	}
}

int ActionRuntime::Index(const std::string& action) const {
	const ActionMap* map = Map();
	return map == nullptr ? -1 : map->IndexOf(action);
}

const char* ActionRuntime::NameAt(int index) const {
	const ActionMap* map = Map();
	if (map == nullptr || index < 0 || index >= (int)map->actions.size())
		return "";
	return map->actions[index].name.c_str();
}

const ActionState& ActionRuntime::State(int index) const {
	// A missing action reads as "nothing is happening" rather than as a crash: a scene
	//	whose map has not been registered yet is a normal state during a transition.
	static const ActionState empty;
	if (index < 0 || index >= (int)states_.size())
		return empty;
	return states_[index];
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
const char* ToString(ActionType t) {
	switch (t) {
	case ActionType::Button:      return "Button";
	case ActionType::Value:       return "Value";
	case ActionType::PassThrough: return "Pass Through";
	}
	return "?";
}
const char* ToString(ControlType t) {
	switch (t) {
	case ControlType::Button:  return "Button";
	case ControlType::Axis:    return "Axis";
	case ControlType::Vector2: return "Vector 2";
	}
	return "?";
}
const char* ToString(DeviceClass d) {
	switch (d) {
	case DeviceClass::Keyboard: return "Keyboard";
	case DeviceClass::Mouse:    return "Mouse";
	case DeviceClass::Gamepad:  return "Gamepad";
	case DeviceClass::Touch:    return "Touchscreen";
	case DeviceClass::Count:    break;
	}
	return "?";
}

std::string ButtonName(wi::input::BUTTON b) {
	using namespace wi::input;
	switch (b) {
	case BUTTON_NONE:                 return "(none)";
	case MOUSE_BUTTON_LEFT:           return "Left Button";
	case MOUSE_BUTTON_RIGHT:          return "Right Button";
	case MOUSE_BUTTON_MIDDLE:         return "Middle Button";
	case MOUSE_SCROLL_AS_BUTTON_UP:   return "Scroll Up";
	case MOUSE_SCROLL_AS_BUTTON_DOWN: return "Scroll Down";

	case GAMEPAD_BUTTON_UP:    return "D-Pad Up";
	case GAMEPAD_BUTTON_LEFT:  return "D-Pad Left";
	case GAMEPAD_BUTTON_DOWN:  return "D-Pad Down";
	case GAMEPAD_BUTTON_RIGHT: return "D-Pad Right";
	case GAMEPAD_BUTTON_1:     return "X / Square";
	case GAMEPAD_BUTTON_2:     return "A / Cross";
	case GAMEPAD_BUTTON_3:     return "B / Circle";
	case GAMEPAD_BUTTON_4:     return "Y / Triangle";
	case GAMEPAD_BUTTON_5:     return "LB / L1";
	case GAMEPAD_BUTTON_6:     return "RB / R1";
	case GAMEPAD_BUTTON_7:     return "L3";
	case GAMEPAD_BUTTON_8:     return "R3";
	case GAMEPAD_BUTTON_9:     return "Back / Share";
	case GAMEPAD_BUTTON_10:    return "Start / Options";
	case GAMEPAD_ANALOG_TRIGGER_L_AS_BUTTON: return "LT / L2";
	case GAMEPAD_ANALOG_TRIGGER_R_AS_BUTTON: return "RT / R2";
	case GAMEPAD_ANALOG_THUMBSTICK_L_AS_BUTTON_UP:    return "Left Stick Up";
	case GAMEPAD_ANALOG_THUMBSTICK_L_AS_BUTTON_DOWN:  return "Left Stick Down";
	case GAMEPAD_ANALOG_THUMBSTICK_L_AS_BUTTON_LEFT:  return "Left Stick Left";
	case GAMEPAD_ANALOG_THUMBSTICK_L_AS_BUTTON_RIGHT: return "Left Stick Right";
	case GAMEPAD_ANALOG_THUMBSTICK_R_AS_BUTTON_UP:    return "Right Stick Up";
	case GAMEPAD_ANALOG_THUMBSTICK_R_AS_BUTTON_DOWN:  return "Right Stick Down";
	case GAMEPAD_ANALOG_THUMBSTICK_R_AS_BUTTON_LEFT:  return "Right Stick Left";
	case GAMEPAD_ANALOG_THUMBSTICK_R_AS_BUTTON_RIGHT: return "Right Stick Right";

	case KEYBOARD_BUTTON_UP:        return "Up Arrow";
	case KEYBOARD_BUTTON_DOWN:      return "Down Arrow";
	case KEYBOARD_BUTTON_LEFT:      return "Left Arrow";
	case KEYBOARD_BUTTON_RIGHT:     return "Right Arrow";
	case KEYBOARD_BUTTON_SPACE:     return "Space";
	case KEYBOARD_BUTTON_LSHIFT:    return "Left Shift";
	case KEYBOARD_BUTTON_RSHIFT:    return "Right Shift";
	case KEYBOARD_BUTTON_ENTER:     return "Enter";
	case KEYBOARD_BUTTON_ESCAPE:    return "Escape";
	case KEYBOARD_BUTTON_TAB:       return "Tab";
	case KEYBOARD_BUTTON_LCONTROL:  return "Left Ctrl";
	case KEYBOARD_BUTTON_RCONTROL:  return "Right Ctrl";
	case KEYBOARD_BUTTON_ALT:       return "Alt";
	case KEYBOARD_BUTTON_ALTGR:     return "AltGr";
	case KEYBOARD_BUTTON_BACKSPACE: return "Backspace";
	case KEYBOARD_BUTTON_DELETE:    return "Delete";
	case KEYBOARD_BUTTON_HOME:      return "Home";
	case KEYBOARD_BUTTON_INSERT:    return "Insert";
	case KEYBOARD_BUTTON_PAGEUP:    return "Page Up";
	case KEYBOARD_BUTTON_PAGEDOWN:  return "Page Down";
	case KEYBOARD_BUTTON_TILDE:     return "`";
	default: break;
	}

	if (b >= KEYBOARD_BUTTON_F1 && b <= KEYBOARD_BUTTON_F12) {
		return "F" + std::to_string(1 + (int)(b - KEYBOARD_BUTTON_F1));
	}
	if (b >= KEYBOARD_BUTTON_NUMPAD0 && b <= KEYBOARD_BUTTON_NUMPAD9) {
		return "Numpad " + std::to_string((int)(b - KEYBOARD_BUTTON_NUMPAD0));
	}
	// Letters and digits are stored as their ASCII code (see BUTTON's DIGIT/CHARACTER
	//	range starts), so a printable code IS the key name.
	if (b >= (int)'0' && b <= (int)'9') return std::string(1, (char)b);
	if (b >= (int)'A' && b <= (int)'Z') return std::string(1, (char)b);

	return "Button " + std::to_string((int)b);
}

} // namespace st::input
