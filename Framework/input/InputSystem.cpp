#include "input/InputSystem.h"

#include <SDL.h>
#include "imgui.h"

#include <algorithm>
#include <cmath>

using wi::input::BUTTON;

namespace st {

InputSystem& InputSystem::Get() {
	static InputSystem instance;
	return instance;
}

// ── source classification (for gating) ─────────────────────────────────────
namespace {
	bool IsMouseButton(BUTTON b) {
		return b >= wi::input::MOUSE_BUTTON_LEFT && b <= wi::input::MOUSE_SCROLL_AS_BUTTON_DOWN;
	}
}

bool InputSystem::gated(const InputBinding& b) const {
	if (b.IsAnalog())
		return false;                       // gamepad analog: never gated
	if (wi::input::IsGamepadButton(b.button))
		return false;                       // gamepad button: never gated
	if (IsMouseButton(b.button))
		return mouseSuspended_;             // mouse: gated while ImGui owns the mouse
	return keyboardSuspended_;              // keyboard: gated while ImGui owns kb / unfocused
}

// ── analog reads ────────────────────────────────────────────────────────────
float InputSystem::analogValue(InputBinding::Analog a) const {
	using A = InputBinding::Analog;
	switch (a) {
		case A::ThumbLX:  return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_L).x;
		case A::ThumbLY:  return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_L).y;
		case A::ThumbRX:  return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_R).x;
		case A::ThumbRY:  return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_R).y;
		case A::TriggerL: return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_TRIGGER_L).x;
		case A::TriggerR: return wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_TRIGGER_R).x;
		default:          return 0.0f;
	}
}

// ── digital reads (gated) ────────────────────────────────────────────────────
bool InputSystem::buttonActive(const InputBinding& b) const {
	if (gated(b))
		return false;
	if (b.IsAnalog())
		return std::fabs(analogValue(b.analog) * b.scale) > 0.5f;
	return wi::input::Down(b.button);
}
bool InputSystem::buttonPressed(const InputBinding& b) const {
	if (gated(b) || b.IsAnalog())
		return false;
	return wi::input::Press(b.button);
}
bool InputSystem::buttonReleased(const InputBinding& b) const {
	if (gated(b) || b.IsAnalog())
		return false;
	return wi::input::Release(b.button);
}
float InputSystem::bindingAxis(const InputBinding& b) const {
	if (gated(b))
		return 0.0f;
	if (b.IsAnalog())
		return analogValue(b.analog) * b.scale;
	return wi::input::Down(b.button) ? b.scale : 0.0f;
}

// ── per-frame update ─────────────────────────────────────────────────────────
void InputSystem::Update(float /*dt*/) {
	// Source gating from ImGui + window focus. GetIO() is valid here because this is
	// called after ImguiUpdate() (NewFrame) in st::App::Update.
	//	While the cursor is captured (FPS mode) the player is driving the game, not typing
	//	into ImGui, so keyboard/mouse go to the game regardless of WantCapture* — otherwise
	//	an open debug window (ImGui keyboard nav) would silently eat WASD. Focus is still
	//	required so we don't read keys while another OS window is active.
	const ImGuiIO& io = ImGui::GetIO();
	const bool unfocused = (SDL_GetKeyboardFocus() == nullptr);
	keyboardSuspended_ = unfocused || (!mouseCaptured_ && io.WantCaptureKeyboard);
	mouseSuspended_    = !mouseCaptured_ && io.WantCaptureMouse;

	// Relative-mouse delta: only meaningful while captured. SDL accumulates motion
	// between calls, so reading once per frame here is the single consumer.
	if (relativeActive_) {
		int dx = 0, dy = 0;
		SDL_GetRelativeMouseState(&dx, &dy);
		mouseDelta_ = XMFLOAT2((float)dx, (float)dy);
	} else {
		mouseDelta_ = XMFLOAT2(0, 0);
	}
}

// ── mouse capture (single owner of SDL relative mode) ────────────────────────
void InputSystem::SetMouseCaptured(bool captured) {
	mouseCaptured_ = captured;
	if (captured && !relativeActive_) {
		SDL_SetRelativeMouseMode(SDL_TRUE);
		SDL_GetRelativeMouseState(nullptr, nullptr); // flush the first-frame jump
		relativeActive_ = true;
	} else if (!captured && relativeActive_) {
		SDL_SetRelativeMouseMode(SDL_FALSE);
		relativeActive_ = false;
		mouseDelta_ = XMFLOAT2(0, 0);
	}
}

// ── keybinding configuration ──────────────────────────────────────────────────
void InputSystem::ClearAction(const std::string& action) {
	actions_.erase(action);
}
void InputSystem::BindButton(const std::string& action, BUTTON b, bool negative) {
	InputAction& a = actions_[action];
	(negative ? a.negative : a.positive).push_back(InputBinding::Button(b));
}
void InputSystem::BindAnalog(const std::string& action, InputBinding::Analog an, float scale, bool negative) {
	InputAction& a = actions_[action];
	(negative ? a.negative : a.positive).push_back(InputBinding::Stick(an, scale));
}
const InputAction* InputSystem::Find(const std::string& action) const {
	auto it = actions_.find(action);
	return it == actions_.end() ? nullptr : &it->second;
}

void InputSystem::LoadDefaults() {
	actions_.clear();
	using A = InputBinding::Analog;

	// Horizontal move: WASD + left stick. X = strafe (right +), Y = forward (+).
	BindButton("MoveX", (BUTTON)'D');               BindButton("MoveX", (BUTTON)'A', /*negative*/true);
	BindAnalog("MoveX", A::ThumbLX);
	BindButton("MoveY", (BUTTON)'W');               BindButton("MoveY", (BUTTON)'S', /*negative*/true);
	BindAnalog("MoveY", A::ThumbLY);

	// Look (analog stick only; mouse look is read via MouseDelta()).
	BindAnalog("LookX", A::ThumbRX);
	BindAnalog("LookY", A::ThumbRY);

	// Buttons.
	BindButton("Sprint", wi::input::KEYBOARD_BUTTON_LSHIFT);
	BindButton("Sprint", wi::input::GAMEPAD_BUTTON_6);          // RB / R1
	BindButton("Jump",   wi::input::KEYBOARD_BUTTON_SPACE);
	BindButton("Jump",   wi::input::GAMEPAD_BUTTON_2);          // A / cross
	BindButton("ReleaseCursor", wi::input::KEYBOARD_BUTTON_ESCAPE);
	BindButton("CaptureCursor", wi::input::MOUSE_BUTTON_LEFT);

	// Interactive mode (FPS): toggle a free-cursor mode where you hold RMB to look.
	BindButton("ToggleInteractive", (BUTTON)'I');
	BindButton("LookDrag", wi::input::MOUSE_BUTTON_RIGHT);
}

// ── queries ────────────────────────────────────────────────────────────────────
bool InputSystem::Down(const std::string& action) const {
	const InputAction* a = Find(action);
	if (a == nullptr) return false;
	for (const InputBinding& b : a->positive) if (buttonActive(b)) return true;
	return false;
}
bool InputSystem::Pressed(const std::string& action) const {
	const InputAction* a = Find(action);
	if (a == nullptr) return false;
	for (const InputBinding& b : a->positive) if (buttonPressed(b)) return true;
	return false;
}
bool InputSystem::Released(const std::string& action) const {
	const InputAction* a = Find(action);
	if (a == nullptr) return false;
	for (const InputBinding& b : a->positive) if (buttonReleased(b)) return true;
	return false;
}
float InputSystem::Axis(const std::string& action) const {
	const InputAction* a = Find(action);
	if (a == nullptr) return 0.0f;
	float v = 0.0f;
	for (const InputBinding& b : a->positive) v += bindingAxis(b);
	for (const InputBinding& b : a->negative) v -= bindingAxis(b);
	return std::clamp(v, -1.0f, 1.0f);
}

XMFLOAT2 InputSystem::MoveVector() const {
	return XMFLOAT2(Axis("MoveX"), Axis("MoveY"));
}
XMFLOAT2 InputSystem::LookVector() const {
	return XMFLOAT2(Axis("LookX"), Axis("LookY"));
}

} // namespace st
