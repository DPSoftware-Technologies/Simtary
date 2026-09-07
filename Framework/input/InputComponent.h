#pragma once
// The "stInput" native component - Simtary's equivalent of Unity's PlayerInput.
//
// Input arrives on an ENTITY instead of out of a singleton. Attach this to whatever
// the player drives, point it at one of the action maps the project registered in
// st::App::OnKeyRegister(), and every other component on that entity reads it as a
// local sibling:
//
//     NCI_0             = "stInput"
//     NCA_0_actionMap   = "Player"
//     NCA_0_playerIndex = 0
//
//     struct PlayerMove : wi::scene::NativeComponent {
//         st::InputComponent* input = nullptr;
//         int moveId = -1, jumpId = -1;             // resolve the names ONCE
//         void Start() override {
//             input  = GetComponent<st::InputComponent>();
//             moveId = input ? input->Find("Move") : -1;
//             jumpId = input ? input->Find("Jump") : -1;
//         }
//         void Update(float dt) override {
//             if (input == nullptr) return;
//             XMFLOAT2 move = input->Vector2(moveId);
//             if (input->Pressed(jumpId)) { ... }
//         }
//     };
//
// Why this is better than reading st::InputSystem directly:
//
//	- Two players are two entities with two stInput components and two playerIndex
//	  values. A singleton has exactly one player by construction.
//	- Switching context is SwitchMap("UI") on one component, not a pile of
//	  if(!menuOpen) checks spread through the scene.
//	- A component that reads input declares that by asking for a sibling, so an entity
//	  that should not be player-controlled simply has no stInput on it and its logic
//	  goes quiet - no global to remember to gate.
//
// Threading: sampling happens in Compute(), NOT Update(). Compute is a barriered
// stage - every component's Compute finishes before any component's Update starts -
// so a sibling reading this in its Update always sees a fully evaluated, stable frame.
// Sampling in Update would race whichever sibling happened to run first.

#include "input/InputActions.h"
#include "stNativeComponent.h"

#include <string>

namespace st {

struct InputComponent : wi::scene::NativeComponent {
	// bound from NCA_<localID>_<name>
	std::string actionMap  = "Player"; // which registered map this entity listens to
	int         playerIndex = 0;       // which gamepad slot; keyboard/mouse ignore it

	// Resolve an action name to an index in the active map. -1 when the map does not
	//	have it. Call once in Start() and use the int reads below every frame.
	int Find(const std::string& action) const { return runtime_.Index(action); }

	// Reads by index (cheap - use these in Update).
	bool     Down(int id) const     { return runtime_.Down(id); }
	bool     Pressed(int id) const  { return runtime_.Pressed(id); }
	bool     Released(int id) const { return runtime_.Released(id); }
	float    Value(int id) const    { return runtime_.Value(id); }
	XMFLOAT2 Vector2(int id) const  { return runtime_.Vector2(id); }
	float    HeldTime(int id) const { return runtime_.State(id).heldTime; }

	// Reads by name (convenient - each one walks the map's action list).
	bool     Down(const std::string& a) const     { return runtime_.Down(a); }
	bool     Pressed(const std::string& a) const  { return runtime_.Pressed(a); }
	bool     Released(const std::string& a) const { return runtime_.Released(a); }
	float    Value(const std::string& a) const    { return runtime_.Value(a); }
	XMFLOAT2 Vector2(const std::string& a) const  { return runtime_.Vector2(a); }
	float    HeldTime(const std::string& a) const { return runtime_.State(a).heldTime; }

	// Which device last drove this action - what a control-prompt HUD switches on.
	st::input::DeviceClass LastDevice(int id) const { return runtime_.State(id).lastDevice; }

	// Swap the active map ("Player" -> "UI"). Takes effect on the next Compute; the
	//	new map's states start clean, so a button held across the switch does not
	//	arrive as a fresh Pressed in the new map.
	void SwitchMap(const std::string& name);

	const st::input::ActionRuntime& Runtime() const { return runtime_; }

	void Start() override;
	void Compute(float dt) override;
	void DrawDebug() override;
	void DescribeParams(wi::vector<NativeParam>& out) override;

private:
	st::input::ActionRuntime runtime_;
	std::string boundMap_;   // what runtime_ is currently pointed at
	int         boundPlayer_ = -1;
};

} // namespace st
