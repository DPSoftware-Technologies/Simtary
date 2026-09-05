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

// source classification (for gating)
namespace {
	bool IsMouseButton(BUTTON b) {
		return b >= wi::input::MOUSE_BUTTON_LEFT && b <= wi::input::MOUSE_SCROLL_AS_BUTTON_DOWN;
	}
}

bool InputSystem::gated(const InputBinding& b) const {
	if (uiInputCaptured_)
		return true;                        // Editor mode: nothing reaches the game, gamepad included
	if (b.IsAnalog())
		return false;                       // gamepad analog: never gated
	if (wi::input::IsGamepadButton(b.button))
		return false;                       // gamepad button: never gated
	if (IsMouseButton(b.button))
		return mouseSuspended_;             // mouse: gated while ImGui owns the mouse
	return keyboardSuspended_;              // keyboard: gated while ImGui owns kb / unfocused
}

// analog reads
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

// digital reads (gated)
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

// per-frame update
void InputSystem::Update(float /*dt*/) {
	// Source gating from ImGui + window focus. GetIO() is valid here because this is
	// called after ImguiUpdate() (NewFrame) in st::App::Update.
	//	While the cursor is captured (FPS mode) the player is driving the game, not typing
	//	into ImGui, so keyboard/mouse go to the game regardless of WantCapture* - otherwise
	//	an open debug window (ImGui keyboard nav) would silently eat WASD. Focus is still
	//	required so we don't read keys while another OS window is active.
	const ImGuiIO& io = ImGui::GetIO();
	const bool unfocused = (SDL_GetKeyboardFocus() == nullptr);
	//	uiMouseLook_ is the developer-tooling override: the editor's free camera is flying,
	//	so nothing reaches the game at all until the look ends.
	//	Three-way ownership, in priority order:
	//	  1. developer tooling (Editor mode outside the Game Viewport, or a freecam look)
	//	  2. the Game Viewport, which bypasses the WantCapture* gating entirely
	//	  3. nobody in particular - the original WantCapture* behaviour
	const bool toolingOwns = uiMouseLook_ || uiInputCaptured_;
	const bool kbToGame    = gameViewKeyboard_ && !toolingOwns && !io.WantTextInput;
	const bool mouseToGame = gameViewMouse_ && !toolingOwns;

	keyboardSuspended_ = unfocused || toolingOwns ||
		(!kbToGame && !mouseCaptured_ && io.WantCaptureKeyboard);
	mouseSuspended_    = toolingOwns ||
		(!mouseToGame && !mouseCaptured_ && io.WantCaptureMouse);

	// While developer tooling owns input, also raise ImGui's own capture flags. This is the
	//	only gate that reaches EVERY reader:
	//
	//	  - on Windows wi::input takes the keyboard from Raw Input and the mouse from
	//	    GetCursorPos/VK_LBUTTON, neither of which passes through the SDL event queue, so
	//	    filtering events in st::Run cannot suppress them;
	//	  - game code is already written against WantCapture* (that is the documented contract
	//	    for "the UI has the keyboard"), so raising it makes an existing fly camera or
	//	    native component back off with no change on the game's side.
	//
	//	SetNextFrame* applies from the following frame, and this runs every frame the capture
	//	is held, so the flags stay raised for as long as it lasts and are released with it.
	if (toolingOwns)
	{
		ImGui::SetNextFrameWantCaptureKeyboard(true);
		ImGui::SetNextFrameWantCaptureMouse(true);
	}
	else
	{
		// ...and force them LOW when the Game Viewport has input, for the same reason: game
		// code that checks WantCapture* has to see "the UI does not want this" or it backs
		// off inside the very panel that is meant to be playing.
		if (kbToGame)    ImGui::SetNextFrameWantCaptureKeyboard(false);
		if (mouseToGame) ImGui::SetNextFrameWantCaptureMouse(false);
	}

	// Relative-mouse delta: only meaningful while captured. SDL accumulates motion
	// between calls, so reading once per frame here is the single consumer.
	if (relativeActive_ || uiMouseLook_) {
		int dx = 0, dy = 0;
		SDL_GetRelativeMouseState(&dx, &dy);
		mouseDelta_ = XMFLOAT2((float)dx, (float)dy);
	} else {
		mouseDelta_ = XMFLOAT2(0, 0);
	}
}

// mouse capture (single owner of SDL relative mode)
void InputSystem::SetMouseCaptured(bool captured) {
	if (uiMouseLook_) {
		// Developer tooling owns the cursor. Remember what the game wanted so releasing the
		// override hands it straight back, instead of the scene having to ask again.
		gameCaptureWanted_ = captured;
		return;
	}
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

void InputSystem::SetUIMouseLook(bool on) {
	if (on == uiMouseLook_)
		return;

	if (on) {
		gameCaptureWanted_ = mouseCaptured_;
		uiMouseLook_ = true;
		if (!relativeActive_) {
			SDL_SetRelativeMouseMode(SDL_TRUE);
			SDL_GetRelativeMouseState(nullptr, nullptr); // flush the first-frame jump
			relativeActive_ = true;
		}
		return;
	}

	// Releasing: restore whatever the game had asked for in the meantime. SDL puts the
	// cursor back where it was when relative mode started.
	uiMouseLook_ = false;
	mouseCaptured_ = gameCaptureWanted_;
	if (relativeActive_ && !mouseCaptured_) {
		SDL_SetRelativeMouseMode(SDL_FALSE);
		relativeActive_ = false;
		mouseDelta_ = XMFLOAT2(0, 0);
	}
}

void InputSystem::SetUIInputCapture(bool on) {
	uiInputCaptured_ = on;
}

void InputSystem::SetGameViewportInput(bool keyboard, bool mouse) {
	gameViewKeyboard_ = keyboard;
	gameViewMouse_ = mouse;
}

void InputSystem::SetUIMouseConfined(bool on) {
	if (on == uiMouseConfined_)
		return;
	uiMouseConfined_ = on;

	if (on) {
		// SDL_GetKeyboardFocus() is the window the user is actually on; the app only ever
		// has one. If nothing is focused there is nothing to confine to.
		grabbedWindow_ = SDL_GetKeyboardFocus();
		if (grabbedWindow_ != nullptr)
			SDL_SetWindowMouseGrab(grabbedWindow_, SDL_TRUE);
		return;
	}

	// Release the SAME window we grabbed: focus may have moved on in the meantime, and SDL
	// drops the grab by itself on focus loss, so re-releasing a different window is wrong.
	if (grabbedWindow_ != nullptr) {
		SDL_SetWindowMouseGrab(grabbedWindow_, SDL_FALSE);
		grabbedWindow_ = nullptr;
	}
}

// keybinding configuration
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

// queries
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
