#pragma once
// Gamepad Analog scope: the DevUI window for the controller analog path.
//
// This panel exists because "the stick sometimes does nothing, and sometimes is
// not steady" is not a bug you can read off a number. It needs a picture of the
// signal, and it needs BOTH signals - what the device reported and what the
// engine handed the game after deadzoning - on the same axis, or a bad deadzone
// and a bad device look identical.
//
// What each part answers:
//
//	Devices table   how many player slots wi::input believes exist and which
//	                backend owns each. On Windows this build compiles XInput,
//	                RawInput and SDL all at once, so one physical pad taking two
//	                slots is a real failure mode and this is where it is visible.
//	Stick pad       the 2D gate: deadzone ring, saturation ring, raw dot, processed
//	                dot, and a decaying trail of recent raw samples. A stick that
//	                drifts sits off-centre at rest; a stick that jitters smears.
//	Traces          the same values over time. The raw line moving while the
//	                processed line sits at zero IS the deadzone eating the input.
//	Frame time      analog value applied per frame is only as steady as the frame
//	                it is applied in, so a spiky dt here explains "not stable" even
//	                when both traces are clean.
//	Deadzone        wi::input::GetAnalogSettings(), live. Drag it to zero and the
//	                processed line should lie exactly on the raw one.
//
// Sampling happens on every call, before the early-out, so a collapsed window
// still records history.

namespace st::devui {

// Draw the Gamepad Analog window. `show` is the DevUI menu's toggle.
void GamepadAnalogWindow(bool* show);

// Draw the Input Actions window: the three-pane view of st::input::Registry -
// action maps, the actions in the selected map with their bindings, and the selected
// action's properties plus its LIVE value. The equivalent of Unity's .inputactions
// editor, read-mostly: the maps are registered in code from st::App::OnKeyRegister,
// so structure is not editable here, but per-binding scale and invert are, which is
// what tuning a look sensitivity actually needs.
void InputActionsWindow(bool* show);

} // namespace st::devui
