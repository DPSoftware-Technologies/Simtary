#pragma once
// Centralized input system for Milistry.
//	A small action-mapping layer on top of wi::input. Game code asks for named
//	*actions* ("MoveX", "Jump", "LookX", ...) instead of hard-coded keys, so the
//	bindings live in one place and can be re-bound at runtime (keyboard / mouse /
//	gamepad button, or a gamepad analog axis). It also owns the SDL relative-mouse
//	(FPS look) state so there is a single owner of cursor capture + per-frame delta.
//
//	Lifecycle: InputSystem::Get().LoadDefaults() once at startup, then Update(dt)
//	exactly once per frame (st::App::Update, after wi::Application::Update so
//	wi::input is fresh, before the scene update so components read current state).
//
//	Querying:
//		Down/Pressed/Released(action) — digital, treats any bound button (and any
//		                                bound analog past a threshold) as the trigger.
//		Axis(action)                  — float in [-1,1]: sum of positive bindings
//		                                minus negative bindings (digital = 1, analog
//		                                = value*scale).
//		MoveVector()/LookVector()     — convenience 2D reads built from the default map.
//
//	Source gating: keyboard sources are ignored while ImGui owns the keyboard or the
//	window is unfocused; mouse-button sources are ignored while ImGui owns the mouse.
//	Gamepad sources are never gated. Relative-mouse delta (MouseDelta) is gated only
//	by capture state, so FPS look keeps working with an ImGui window open.

#include "wiInput.h"   // wi::input::BUTTON / GAMEPAD_ANALOG + DirectXMath (XMFLOAT2)

#include <string>
#include <vector>
#include <unordered_map>

namespace st {

// One bound source feeding an action: either a digital button (analog == None) or a
// gamepad analog axis (button == BUTTON_NONE). `scale` weights an analog source and
// also lets a digital source contribute a custom magnitude to an axis (default 1).
struct InputBinding {
	enum class Analog { None, ThumbLX, ThumbLY, ThumbRX, ThumbRY, TriggerL, TriggerR };

	wi::input::BUTTON button = wi::input::BUTTON_NONE;
	Analog            analog = Analog::None;
	float             scale  = 1.0f;

	static InputBinding Button(wi::input::BUTTON b)              { InputBinding x; x.button = b; return x; }
	static InputBinding Stick(Analog a, float scale = 1.0f)      { InputBinding x; x.analog = a; x.scale = scale; return x; }
	bool IsAnalog() const { return analog != Analog::None; }
};

// An action is the union of a digital trigger set (`positive` bindings used for
// Down/Pressed/Released) and a signed axis (positive - negative). A pure button
// action only fills `positive` with buttons; a pure axis fills positive/negative.
struct InputAction {
	std::vector<InputBinding> positive;
	std::vector<InputBinding> negative;
};

class InputSystem {
public:
	static InputSystem& Get();

	// Call once per frame (st::App::Update), after wi::Application::Update.
	void Update(float dt);

	// ── keybinding configuration (re-bindable at runtime) ──────────────────
	void LoadDefaults();                                   // Milistry's default keymap
	void ClearAction(const std::string& action);
	void BindButton(const std::string& action, wi::input::BUTTON b, bool negative = false);
	void BindAnalog(const std::string& action, InputBinding::Analog a, float scale = 1.0f, bool negative = false);
	const InputAction* Find(const std::string& action) const;

	// ── queries ────────────────────────────────────────────────────────────
	bool  Down(const std::string& action) const;     // any bound source active
	bool  Pressed(const std::string& action) const;  // a bound button went down this frame
	bool  Released(const std::string& action) const; // a bound button went up this frame
	float Axis(const std::string& action) const;      // [-1,1], positive - negative

	XMFLOAT2 MoveVector() const; // (MoveX, MoveY) from the default move actions
	XMFLOAT2 LookVector() const; // (LookX, LookY) analog stick look (mouse handled via MouseDelta)

	// ── FPS mouse look (single owner of SDL relative-mouse mode) ────────────
	void     SetMouseCaptured(bool captured);
	bool     IsMouseCaptured() const { return mouseCaptured_; }
	XMFLOAT2 MouseDelta() const { return mouseDelta_; } // pixels since last frame (0 when not captured)

private:
	InputSystem() = default;

	bool buttonActive(const InputBinding& b) const;      // gated digital read (Down)
	bool buttonPressed(const InputBinding& b) const;     // gated digital edge (down)
	bool buttonReleased(const InputBinding& b) const;    // gated digital edge (up)
	float analogValue(InputBinding::Analog a) const;     // raw analog read (no gating)
	float bindingAxis(const InputBinding& b) const;      // signed contribution for Axis()
	bool gated(const InputBinding& b) const;             // true => ignore this source this frame

	std::unordered_map<std::string, InputAction> actions_;

	XMFLOAT2 mouseDelta_   = XMFLOAT2(0, 0);
	bool     mouseCaptured_ = false;
	bool     relativeActive_ = false; // mirrors SDL_SetRelativeMouseMode
	bool     keyboardSuspended_ = false; // ImGui owns keyboard / window unfocused
	bool     mouseSuspended_    = false; // ImGui owns mouse
};

} // namespace st
