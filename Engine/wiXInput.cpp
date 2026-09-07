#include "wiXInput.h"

#include <algorithm>

#if __has_include(<xinput.h>)

#if defined(PLATFORM_WINDOWS_DESKTOP)
#include <xinput.h>
#pragma comment(lib,"xinput.lib")
#endif // PLATFORM_WINDOWS_DESKTOP

#ifdef PLATFORM_XBOX
#include <XInputOnGameInput.h>
using namespace XInputOnGameInput;
#endif // PLATFORM_XBOX

namespace wi::input::xinput
{
	XINPUT_STATE controllers[4] = {};
	bool connected[arraysize(controllers)] = {};

	// Frames to wait before re-probing a slot that answered ERROR_DEVICE_NOT_CONNECTED.
	//	XInputGetState on an EMPTY user index is not a cheap read: it walks the device
	//	list, and on a machine with one pad the three empty slots cost far more than the
	//	one real one - milliseconds, every frame, as a spike rather than a constant. A
	//	frame that hitches makes analog input feel unstable even when the values are
	//	perfect, because whatever the stick drives is integrated against a delta time
	//	that jumps around. Hot-plug still works, it is just noticed within ~1s instead
	//	of within one frame.
	static constexpr int DISCONNECTED_RETRY_FRAMES = 60;
	int retry_countdown[arraysize(controllers)] = {};

	void Update()
	{
		for (DWORD i = 0; i < arraysize(controllers); i++)
		{
			if (!connected[i] && retry_countdown[i] > 0)
			{
				retry_countdown[i]--;
				controllers[i] = {};
				continue;
			}

			controllers[i] = {};
			DWORD dwResult = XInputGetState(i, &controllers[i]);

			if (dwResult == ERROR_SUCCESS)
			{
				connected[i] = true;
				retry_countdown[i] = 0;
			}
			else
			{
				connected[i] = false;
				retry_countdown[i] = DISCONNECTED_RETRY_FRAMES;
			}
		}
	}

	int GetMaxControllerCount()
	{
		return arraysize(controllers);
	}
	// Normalize one XInput axis. sThumbLX/LY are SHORT, so the negative end reaches
	//	-32768 while the positive end stops at 32767: dividing both by 32767 lets the
	//	left/down end overshoot -1, which then clamps and makes the axis very slightly
	//	asymmetric. Divide by the correct magnitude per sign instead.
	inline float normalize_axis(SHORT v)
	{
		const float f = (v < 0) ? ((float)v / 32768.0f) : ((float)v / 32767.0f);
		return std::max(-1.0f, std::min(1.0f, f));
	}
	bool GetControllerState(wi::input::ControllerState* state, int index)
	{
		if (index < arraysize(controllers))
		{
			if (connected[index])
			{
				if (state != nullptr)
				{
					*state = wi::input::ControllerState();
					const XINPUT_STATE& xinput_state = controllers[index];

					// Retrieve buttons:
					for (int button = wi::input::GAMEPAD_RANGE_START + 1; button < wi::input::GAMEPAD_RANGE_END; ++button)
					{
						bool down = false;
						switch (button)
						{
						case wi::input::GAMEPAD_BUTTON_UP: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP; break;
						case wi::input::GAMEPAD_BUTTON_LEFT: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT; break;
						case wi::input::GAMEPAD_BUTTON_DOWN: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN; break;
						case wi::input::GAMEPAD_BUTTON_RIGHT: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT; break;
						case wi::input::GAMEPAD_BUTTON_1: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_X; break;
						case wi::input::GAMEPAD_BUTTON_2: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_A; break;
						case wi::input::GAMEPAD_BUTTON_3: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_B; break;
						case wi::input::GAMEPAD_BUTTON_4: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y; break;
						case wi::input::GAMEPAD_BUTTON_5: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER; break;
						case wi::input::GAMEPAD_BUTTON_6: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
						case wi::input::GAMEPAD_BUTTON_7: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB; break;
						case wi::input::GAMEPAD_BUTTON_8: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB; break;
						case wi::input::GAMEPAD_BUTTON_9: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK; break;
						case wi::input::GAMEPAD_BUTTON_10: down = xinput_state.Gamepad.wButtons & XINPUT_GAMEPAD_START; break;
						}

						if (down)
						{
							state->buttons |= 1 << (button - wi::input::GAMEPAD_RANGE_START - 1);
						}
					}

					// Retrieve analog inputs. Raw first, in the engine's sign convention
					//	(L.y up-positive, R.y down-positive - XInput reports both sticks
					//	up-positive, so only the right one is flipped), then one shared
					//	radial deadzone on top. The old per-axis cut at 0.24 with no
					//	rescale is what made the stick jump straight from 0 to 0.24 and
					//	flicker whenever it rested near that line.
					state->thumbstick_L_raw = XMFLOAT2(
						normalize_axis(xinput_state.Gamepad.sThumbLX),
						normalize_axis(xinput_state.Gamepad.sThumbLY));
					state->thumbstick_R_raw = XMFLOAT2(
						normalize_axis(xinput_state.Gamepad.sThumbRX),
						-normalize_axis(xinput_state.Gamepad.sThumbRY));
					state->trigger_L_raw = (float)xinput_state.Gamepad.bLeftTrigger / 255.0f;
					state->trigger_R_raw = (float)xinput_state.Gamepad.bRightTrigger / 255.0f;

					state->thumbstick_L = wi::input::ApplyStickDeadzone(state->thumbstick_L_raw);
					state->thumbstick_R = wi::input::ApplyStickDeadzone(state->thumbstick_R_raw);
					state->trigger_L = wi::input::ApplyTriggerDeadzone(state->trigger_L_raw);
					state->trigger_R = wi::input::ApplyTriggerDeadzone(state->trigger_R_raw);

				}

				return true;
			}
		}
		return false;
	}
	void SetControllerFeedback(const wi::input::ControllerFeedback& data, int index)
	{
		if (index < arraysize(controllers))
		{
			XINPUT_VIBRATION vibration = {};
			vibration.wLeftMotorSpeed = (WORD)(std::max(0.0f, std::min(1.0f, data.vibration_left)) * 65535);
			vibration.wRightMotorSpeed = (WORD)(std::max(0.0f, std::min(1.0f, data.vibration_right)) * 65535);
			XInputSetState((DWORD)index, &vibration);
		}
	}
}

#else
namespace wi::input::xinput
{
	void Update() {}
	int GetMaxControllerCount() { return 0; }
	bool GetControllerState(wi::input::ControllerState* state, int index) { return false; }
	void SetControllerFeedback(const wi::input::ControllerFeedback& data, int index) {}
}
#endif // __has_include(<xinput.h>)
