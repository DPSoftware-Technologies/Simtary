#pragma once
// Action-map input model, in the shape Unity's Input System uses.
//
// The old st::InputSystem keymap is one flat namespace of actions with one binding
// list each. That is enough for one player walking around and runs out immediately
// past that: there is no way to say "these bindings belong to gameplay and those to
// the menu", no way to give two players their own pads, no way for an action to be a
// Vector2 rather than a float, and no way for a component to ask for input WITHOUT
// reaching into a global singleton.
//
// The model here is four nouns:
//
//	Registry    every map the game registered. Built once at startup from
//	            st::App::OnKeyRegister(). This is the equivalent of Unity's
//	            .inputactions asset - it is DATA, and nothing in it reads a device.
//	ActionMap   a named group of actions: "Player", "UI", "Vehicle". Maps are
//	            switched, not merged, so the menu's Submit and gameplay's Fire can
//	            both sit on the same button without fighting.
//	Action      a named input with a CONTROL TYPE (Button / Axis / Vector2) and an
//	            ACTION TYPE (Button / Value / PassThrough), plus its bindings.
//	Binding     one physical control feeding the action, optionally through a
//	            processor (scale, invert) and optionally as one PART of a composite
//	            (the four keys of a WASD 2D vector are four bindings on one action).
//
// Evaluation lives in ActionRuntime, which is per PLAYER, not global: it holds one
// ActionState per action of one map for one player index. st::InputComponent owns one
// and that is what a scene's components read, so input arrives the way every other
// per-entity thing does instead of through a singleton.
//
// Reading a Vector2 action, one candidate WINS rather than every binding summing.
// Unity calls this control disambiguation and it is the difference between "stick
// half over plus W held" reading as 1.5 and reading as 1: each binding group produces
// a candidate, and the largest magnitude is the action's value. A composite counts as
// one group, so WASD is one candidate and the stick is another.
//
// Nothing here reads a device directly except EvaluateAction(); every device read goes
// through the frame snapshot BeginFrame() takes, so a runtime can be evaluated from a
// job thread without racing SDL.

#include "wiInput.h"

#include <string>
#include <vector>

namespace st::input {

// Action type: what the action MEANS, independent of its control's shape.
//	Button      : a press. Down/Pressed/Released are the interesting reads.
//	Value       : a continuous magnitude with disambiguation - one control drives it.
//	PassThrough : like Value but with no disambiguation and no clamping; every
//	              binding is summed and handed over as-is. For raw pointer deltas.
enum class ActionType : uint8_t { Button, Value, PassThrough };

// Control type: the SHAPE of the value.
enum class ControlType : uint8_t { Button, Axis, Vector2 };

// Which physical device a binding belongs to. Used for gating (the UI can own the
//	keyboard without owning the pad) and for the "last device used" readout that a
//	control-prompt HUD switches on.
enum class DeviceClass : uint8_t { Keyboard, Mouse, Gamepad, Touch, Count };

// What produces the number.
enum class Source : uint8_t {
	Button,        // a wi::input::BUTTON - keyboard key, mouse button or gamepad button
	GamepadAxis,   // one axis of a stick, or one trigger
	GamepadStick,  // both axes of a stick, as a Vector2
	MouseDelta,    // pointer motion this frame, as a Vector2 (pixels)
	MouseScroll,   // wheel delta this frame
	TouchPosition, // primary touch position (logical pixels), as a Vector2
	TouchDelta,    // primary touch motion this frame, as a Vector2
	TouchPress,    // any touch down
};

enum class GamepadAxis : uint8_t { LeftX, LeftY, RightX, RightY, TriggerL, TriggerR };
enum class Stick : uint8_t { Left, Right };

// Which component of the action's value a binding feeds.
//	Value        : the binding drives the whole thing. A scalar source fills x; a
//	               Vector2 source fills both.
//	X / Y        : a SCALAR source drives one component of a Vector2 action.
//	Up/Down/Left/Right : the binding is one arm of a 2D composite (WASD). All four
//	               arms of an action combine into a single candidate.
enum class Part : uint8_t { Value, X, Y, Up, Down, Left, Right };

// One physical control feeding one action.
struct Binding {
	Source      source = Source::Button;
	// Source::Button
	wi::input::BUTTON button = wi::input::BUTTON_NONE;
	// Source::GamepadAxis
	GamepadAxis axis = GamepadAxis::LeftX;
	// Source::GamepadStick
	Stick       stick = Stick::Left;

	Part  part   = Part::Value;
	float scale  = 1.0f;   // processor: multiply
	bool  invert = false;  // processor: negate (applied after scale)

	// Control scheme this binding belongs to ("Gamepad", "Keyboard&Mouse", ...).
	//	Free-form and purely descriptive - it labels the binding in the DevUI tree and
	//	lets a game filter by scheme later. Empty means "any scheme".
	std::string scheme;

	DeviceClass Device() const;
	// "Left Stick [Gamepad]", "W [Keyboard]" - the row text in the DevUI action tree.
	std::string Describe() const;
};

struct Action {
	std::string  name;
	ActionType   actionType  = ActionType::Button;
	ControlType  controlType = ControlType::Button;
	// Magnitude at which a Value action counts as pressed. Unity's "press point".
	float        pressPoint  = 0.5f;
	std::vector<Binding> bindings;
};

struct ActionMap {
	std::string name;
	std::vector<Action> actions;

	int           IndexOf(const std::string& action) const;
	const Action* Find(const std::string& action) const;
};

// Fluent builder for one action. Every method appends a binding and returns *this, so
//	an action reads as one statement:
//
//		player.Action("Move", ControlType::Vector2)
//		      .Stick(Stick::Left)
//		      .Composite2D('W', 'S', 'A', 'D');
class ActionBuilder {
public:
	explicit ActionBuilder(Action* action) : action_(action) {}

	// Tag every binding added AFTER this call with a control scheme name.
	ActionBuilder& Scheme(const char* name);
	// Magnitude at which a Value action reads as pressed (default 0.5).
	ActionBuilder& PressPoint(float p);
	// Override the action type the control type implied.
	ActionBuilder& Type(ActionType t);

	// A single button: keyboard key, mouse button or gamepad button.
	//	`part` matters only for a Vector2 action - use Composite2D for WASD.
	ActionBuilder& Button(wi::input::BUTTON b, float scale = 1.0f, Part part = Part::Value);
	// Convenience for a letter or digit key: Key('W'), Key('5').
	ActionBuilder& Key(char c, float scale = 1.0f, Part part = Part::Value);

	// Both axes of a gamepad stick, as a Vector2.
	ActionBuilder& StickBinding(Stick s, float scale = 1.0f);
	// One gamepad axis (a stick axis or a trigger) as a scalar.
	ActionBuilder& Axis(GamepadAxis a, float scale = 1.0f, Part part = Part::Value);

	// Pointer motion this frame, in pixels, as a Vector2. Zero unless the cursor is
	//	captured - st::InputSystem owns SDL relative mouse mode and publishes the delta.
	ActionBuilder& MouseDelta(float scale = 1.0f);
	ActionBuilder& MouseScroll(float scale = 1.0f, Part part = Part::Value);

	// Touch. Position is in logical pixels; delta is motion since the last frame.
	ActionBuilder& TouchDelta(float scale = 1.0f);
	ActionBuilder& TouchPosition(float scale = 1.0f);
	ActionBuilder& TouchPress();

	// A 2D vector built from four buttons (Unity's "2D Vector" composite). The four
	//	arms combine into ONE candidate, so holding W and pushing the stick does not
	//	add up to more than full deflection.
	ActionBuilder& Composite2D(wi::input::BUTTON up, wi::input::BUTTON down,
	                           wi::input::BUTTON left, wi::input::BUTTON right);
	// Same, for letter keys: Composite2D('W','S','A','D').
	ActionBuilder& Composite2D(char up, char down, char left, char right);
	// A signed axis from two buttons.
	ActionBuilder& Composite1D(wi::input::BUTTON positive, wi::input::BUTTON negative);
	ActionBuilder& Composite1D(char positive, char negative);

	// Negate the binding added last. `.Axis(GamepadAxis::RightY).Inverted()`.
	ActionBuilder& Inverted();

	Action* Get() const { return action_; }

private:
	ActionBuilder& push(Binding b);
	Action*     action_ = nullptr;
	std::string scheme_;
};

// Fluent builder for one map.
class MapBuilder {
public:
	explicit MapBuilder(ActionMap* map) : map_(map) {}

	// Create (or return) an action in this map. The action type defaults from the
	//	control type: a Button control is a Button action, everything else is a Value.
	ActionBuilder Action(const std::string& name, ControlType control = ControlType::Button);

	ActionMap* Get() const { return map_; }

private:
	ActionMap* map_ = nullptr;
};

// A resolved (map, action) pair. Look one up once in Start() and read it every frame
//	rather than hashing a string per read.
struct ActionId {
	int map = -1;
	int action = -1;
	bool Valid() const { return map >= 0 && action >= 0; }
};

// Every action map the game registered. Data only - it reads no devices.
class Registry {
public:
	// Create the map if it does not exist, then hand back a builder for it.
	MapBuilder Map(const std::string& name);

	int              MapIndex(const std::string& name) const;
	ActionMap*       FindMap(const std::string& name);
	const ActionMap* FindMap(const std::string& name) const;
	ActionId         Resolve(const std::string& map, const std::string& action) const;

	std::vector<ActionMap>&       Maps()       { return maps_; }
	const std::vector<ActionMap>& Maps() const { return maps_; }

	void Clear() { maps_.clear(); }
	// Drop one map. A project that wants to replace a framework default rather than
	//	extend it removes it first - Map() returns the EXISTING map when the name is
	//	already taken, and Action() the existing action, so registering over the top
	//	appends bindings instead of replacing them.
	void RemoveMap(const std::string& name);
	bool Empty() const { return maps_.empty(); }

private:
	std::vector<ActionMap> maps_;
};

// What one action reads this frame.
struct ActionState {
	XMFLOAT2 value    = XMFLOAT2(0, 0); // x for Button/Axis, both for Vector2
	bool     down     = false;
	bool     pressed  = false;          // went down this frame
	bool     released = false;          // went up this frame
	float    heldTime = 0.0f;           // seconds held, 0 while up
	DeviceClass lastDevice = DeviceClass::Keyboard; // which device last drove it
};

// Which device classes are suppressed this frame. st::InputSystem publishes this once
//	per frame from the ImGui capture flags and its own editor gates; the evaluator only
//	reads it. Kept here rather than in InputSystem so InputActions has no dependency on
//	it and the two can be used apart.
struct DeviceGate {
	bool suppressed[(int)DeviceClass::Count] = {};
	bool operator[](DeviceClass d) const { return suppressed[(int)d]; }
};
DeviceGate& Gate();

// Snapshot the frame's device state. Call once per frame on the MAIN thread, before
//	any runtime is evaluated: it latches the pointer delta, the wheel and the touch
//	list so an ActionRuntime can be evaluated from a worker thread without touching SDL.
//	`pointerDelta` comes from st::InputSystem, which is the single owner of SDL relative
//	mouse mode and therefore the only thing that knows whether the delta is meaningful.
void BeginFrame(float dt, const XMFLOAT2& pointerDelta, float scrollDelta);

// One player's live view of one map. Evaluate() once per frame, then read.
class ActionRuntime {
public:
	// Point at a map. Safe to call every frame; it only does work when something
	//	changed. A name that is not registered leaves the runtime invalid and every
	//	read returns a zeroed state, which is what a scene should see when its map is
	//	missing rather than a crash.
	void Bind(const Registry* registry, const std::string& mapName, int playerIndex);

	// `ignoreGate` evaluates every binding even while st::InputSystem has the device
	//	class suppressed. Only a diagnostic reader wants it: a panel whose whole job is
	//	to SHOW what a device is doing goes blank otherwise, because focusing that panel
	//	is itself what raises the editor's capture. Game runtimes leave it false.
	void Evaluate(float dt, bool ignoreGate = false);

	bool Valid() const { return registry_ != nullptr && mapIndex_ >= 0; }
	const std::string& MapName() const { return mapName_; }
	int  PlayerIndex() const { return playerIndex_; }
	int  Count() const { return (int)states_.size(); }

	// -1 when the action is not in this map.
	int Index(const std::string& action) const;
	const char* NameAt(int index) const;

	// Reads. The string forms hash a name per call; resolve once with Index() and use
	//	the int forms in anything that runs every frame.
	const ActionState& State(int index) const;
	const ActionState& State(const std::string& action) const { return State(Index(action)); }

	bool     Down(int i) const     { return State(i).down; }
	bool     Pressed(int i) const  { return State(i).pressed; }
	bool     Released(int i) const { return State(i).released; }
	float    Value(int i) const    { return State(i).value.x; }
	XMFLOAT2 Vector2(int i) const  { return State(i).value; }

	bool     Down(const std::string& a) const     { return State(a).down; }
	bool     Pressed(const std::string& a) const  { return State(a).pressed; }
	bool     Released(const std::string& a) const { return State(a).released; }
	float    Value(const std::string& a) const    { return State(a).value.x; }
	XMFLOAT2 Vector2(const std::string& a) const  { return State(a).value; }

	const ActionMap* Map() const;

private:
	const Registry* registry_ = nullptr;
	std::string     mapName_;
	int             mapIndex_   = -1;
	int             playerIndex_ = 0;
	std::vector<ActionState> states_;
};

// Evaluate one action for one player against the current frame snapshot. Exposed so a
//	tool (the DevUI action tree) can show a live value without owning a runtime.
ActionState EvaluateAction(const Action& action, int playerIndex, float dt,
                           const ActionState& previous, bool ignoreGate = false);

// Display helpers for tooling.
const char* ToString(ActionType t);
const char* ToString(ControlType t);
const char* ToString(DeviceClass d);
// Human-readable name for a wi::input::BUTTON ("W", "Left Mouse", "Gamepad A").
std::string ButtonName(wi::input::BUTTON b);

} // namespace st::input
