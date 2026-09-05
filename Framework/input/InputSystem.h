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
//		Down/Pressed/Released(action) - digital, treats any bound button (and any
//		                                bound analog past a threshold) as the trigger.
//		Axis(action)                  - float in [-1,1]: sum of positive bindings
//		                                minus negative bindings (digital = 1, analog
//		                                = value*scale).
//		MoveVector()/LookVector()     - convenience 2D reads built from the default map.
//
//	Source gating: keyboard sources are ignored while ImGui owns the keyboard or the
//	window is unfocused; mouse-button sources are ignored while ImGui owns the mouse.
//	Gamepad sources are never gated. Relative-mouse delta (MouseDelta) is gated only
//	by capture state, so FPS look keeps working with an ImGui window open.

#include "wiInput.h"   // wi::input::BUTTON / GAMEPAD_ANALOG + DirectXMath (XMFLOAT2)

struct SDL_Window; // only ever held as a pointer here; SDL.h stays out of this header

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

	// keybinding configuration (re-bindable at runtime)
	void LoadDefaults();                                   // Milistry's default keymap
	void ClearAction(const std::string& action);
	void BindButton(const std::string& action, wi::input::BUTTON b, bool negative = false);
	void BindAnalog(const std::string& action, InputBinding::Analog a, float scale = 1.0f, bool negative = false);
	const InputAction* Find(const std::string& action) const;

	// queries
	bool  Down(const std::string& action) const;     // any bound source active
	bool  Pressed(const std::string& action) const;  // a bound button went down this frame
	bool  Released(const std::string& action) const; // a bound button went up this frame
	float Axis(const std::string& action) const;      // [-1,1], positive - negative

	XMFLOAT2 MoveVector() const; // (MoveX, MoveY) from the default move actions
	XMFLOAT2 LookVector() const; // (LookX, LookY) analog stick look (mouse handled via MouseDelta)

	// FPS mouse look (single owner of SDL relative-mouse mode)
	void     SetMouseCaptured(bool captured);
	bool     IsMouseCaptured() const { return mouseCaptured_; }
	XMFLOAT2 MouseDelta() const { return mouseDelta_; } // pixels since last frame (0 when not captured)

	// Developer-tooling override, for a UI that drives a camera with the mouse (the
	//	editor's free camera - Framework/devui/imeditor.h). While it is on:
	//	  - SDL relative-mouse mode is on, so the cursor is hidden and the pointer cannot
	//	    run off the window or hit a screen edge mid-drag;
	//	  - MouseDelta() keeps reporting motion, which is what the tool reads;
	//	  - every keyboard and mouse SOURCE stays gated, so the game does not also walk
	//	    forward while the editor is flying;
	//	  - SetMouseCaptured() from game code is remembered but not applied, so a scene's
	//	    own fly camera cannot yank the cursor back mid-look. It is applied on release.
	void SetUIMouseLook(bool on);
	bool IsUIMouseLook() const { return uiMouseLook_; }

	// Confine the cursor to the window without hiding it (SDL window mouse-grab). This is
	//	the other half of the editor's drag handling: a gizmo drag is an ordinary visible-
	//	cursor drag, and without a grab the pointer walks off onto another monitor or app
	//	while the drag is still live. Unlike SetUIMouseLook it does not gate game input.
	void SetUIMouseConfined(bool on);
	bool IsUIMouseConfined() const { return uiMouseConfined_; }

	// developer tooling owns input
	// Hard gate for Editor mode: while this is on, the GAME receives no keyboard or mouse
	//	input at all, whether it reads through this class or straight from wi::input.
	//
	//	The ImGui WantCapture* flags are not enough on their own. A focused panel does not
	//	set WantCaptureKeyboard - ImGui only raises that for text fields and nav - so typing
	//	WASD into the editor viewport still reached the game's fly camera. This flag is the
	//	missing piece: st::Run consults it before handing an SDL event to
	//	wi::input::sdlinput::ProcessEvent, so the event never enters the engine's input state.
	//
	//	Gamepad is gated for actions read through this class, but gamepad SDL events are
	//	still forwarded: an axis has no "released" event, so dropping them would freeze a
	//	stick at whatever value it last held.
	void SetUIInputCapture(bool on);
	bool IsUIInputCaptured() const { return uiInputCaptured_; }

	// The other direction, and the one that is easy to miss: hand input BACK to the game
	//	even though ImGui says it owns it.
	//
	//	Editor mode's Game Viewport is itself an ImGui window. Hovering it raises
	//	WantCaptureMouse, and clicking in it gives ImGui an ActiveId which raises
	//	WantCaptureKeyboard - so the panel whose entire job is to play the game was the thing
	//	suppressing the game's input. While this is set, the WantCapture* gating is bypassed
	//	and the flags are forced low, so components reading through this class AND game code
	//	reading wi::input behind a WantCapture* check both work normally.
	//
	//	keyboard : the Game Viewport is the focused panel (and no text field is live)
	//	mouse    : the pointer is actually over the Game Viewport image, so clicking the
	//	           Hierarchy does not also shoot in the game
	//	UI input capture always wins over this.
	void SetGameViewportInput(bool keyboard, bool mouse);

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
	bool     uiMouseLook_       = false; // developer tooling is driving a camera (see SetUIMouseLook)
	bool     gameCaptureWanted_ = false; // what game code asked for while uiMouseLook_ was on
	bool     uiMouseConfined_   = false; // cursor grabbed to the window (see SetUIMouseConfined)
	bool     uiInputCaptured_   = false; // developer tooling owns input (see SetUIInputCapture)
	bool     gameViewKeyboard_  = false; // game viewport owns the keyboard (see SetGameViewportInput)
	bool     gameViewMouse_     = false; // game viewport owns the mouse
	SDL_Window* grabbedWindow_  = nullptr; // the window the grab was applied to, to release the same one
};

} // namespace st
