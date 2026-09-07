#include "devui/iminput.h"

#include "imgui.h"
#include "wiInput.h"
#include "input/InputActions.h"
#include "input/InputSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st::devui {

namespace {

// A fixed-size ring of recent samples. 600 at 60 Hz is ten seconds of history,
// which is long enough to catch an intermittent dropout without the trace turning
// into a solid block.
struct Ring {
	static constexpr int CAP = 600;
	float  v[CAP] = {};
	int    count  = 0;
	int    head   = 0;

	void push(float x) {
		v[head] = x;
		head = (head + 1) % CAP;
		if (count < CAP)
			count++;
	}
	// 0 is the oldest sample still held, count-1 the newest.
	float at(int i) const { return v[(head - count + i + 2 * CAP) % CAP]; }
	float newest() const { return count > 0 ? at(count - 1) : 0.0f; }
	void clear() { count = 0; head = 0; }
};

// Everything the panel remembers between frames. One set per player slot; the
// slot count is the same 4 XInput allows, which is also as many as the DevUI can
// usefully show at once.
struct SlotHistory {
	Ring lx_raw, ly_raw, rx_raw, ry_raw, lt_raw, rt_raw;
	Ring lx, ly, rx, ry, lt, rt;

	// "The value did not change for N frames" is the other half of the picture:
	// SDL only writes an axis when an event arrives, so a stuck value and a
	// genuinely still stick look the same in the trace but not here.
	int   stale_frames_L = 0;
	int   stale_frames_R = 0;
	float last_L_x = 0, last_L_y = 0, last_R_x = 0, last_R_y = 0;
};

SlotHistory histories[8];
Ring        frame_ms;
bool        paused = false;
int         selected_slot = 0;

void PushSlot(int slot, const wi::input::ControllerState& s) {
	SlotHistory& h = histories[slot];
	h.lx_raw.push(s.thumbstick_L_raw.x); h.ly_raw.push(s.thumbstick_L_raw.y);
	h.rx_raw.push(s.thumbstick_R_raw.x); h.ry_raw.push(s.thumbstick_R_raw.y);
	h.lt_raw.push(s.trigger_L_raw);      h.rt_raw.push(s.trigger_R_raw);

	h.lx.push(s.thumbstick_L.x); h.ly.push(s.thumbstick_L.y);
	h.rx.push(s.thumbstick_R.x); h.ry.push(s.thumbstick_R.y);
	h.lt.push(s.trigger_L);      h.rt.push(s.trigger_R);

	const bool moved_L = s.thumbstick_L_raw.x != h.last_L_x || s.thumbstick_L_raw.y != h.last_L_y;
	const bool moved_R = s.thumbstick_R_raw.x != h.last_R_x || s.thumbstick_R_raw.y != h.last_R_y;
	h.stale_frames_L = moved_L ? 0 : h.stale_frames_L + 1;
	h.stale_frames_R = moved_R ? 0 : h.stale_frames_R + 1;
	h.last_L_x = s.thumbstick_L_raw.x; h.last_L_y = s.thumbstick_L_raw.y;
	h.last_R_x = s.thumbstick_R_raw.x; h.last_R_y = s.thumbstick_R_raw.y;
}

// Root-mean-square deviation from the mean over the last `window` samples. This is
// the number that says "the stick is noisy" - a resting stick on a healthy pad
// reads well under 0.01 raw.
float Jitter(const Ring& r, int window = 120) {
	const int n = std::min(window, r.count);
	if (n < 2)
		return 0.0f;
	float mean = 0;
	for (int i = 0; i < n; ++i)
		mean += r.at(r.count - n + i);
	mean /= (float)n;
	float acc = 0;
	for (int i = 0; i < n; ++i) {
		const float d = r.at(r.count - n + i) - mean;
		acc += d * d;
	}
	return std::sqrt(acc / (float)n);
}

float PeakAbs(const Ring& r, int window = 120) {
	const int n = std::min(window, r.count);
	float peak = 0;
	for (int i = 0; i < n; ++i)
		peak = std::max(peak, std::fabs(r.at(r.count - n + i)));
	return peak;
}

// One scope rectangle carrying two series: raw (dim) under processed (bright).
// Drawn by hand rather than with PlotLines because the whole point is the two
// lines on ONE set of axes - PlotLines can only draw a single series per call.
void DrawTrace(const char* label, const Ring& raw, const Ring& processed,
               float lo, float hi, float height = 58.0f) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);

	ImGui::InvisibleButton(label, size);

	dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255));
	dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));

	const float span = std::max(1e-5f, hi - lo);
	auto ytoscreen = [&](float v) {
		const float t = (v - lo) / span;
		return p1.y - t * size.y;
	};

	// Zero line, and the deadzone band so "the input is inside the deadzone" is
	// readable without doing arithmetic against the slider.
	const wi::input::AnalogSettings& cfg = wi::input::GetAnalogSettings();
	const bool bipolar = lo < 0.0f;
	const float dz = bipolar ? cfg.stick_deadzone : cfg.trigger_deadzone;
	dl->AddRectFilled(ImVec2(p0.x, ytoscreen(std::min(hi, dz))),
	                  ImVec2(p1.x, ytoscreen(std::max(lo, bipolar ? -dz : 0.0f))),
	                  IM_COL32(60, 50, 30, 120));
	if (bipolar)
		dl->AddLine(ImVec2(p0.x, ytoscreen(0)), ImVec2(p1.x, ytoscreen(0)), IM_COL32(90, 90, 100, 255));

	auto plot = [&](const Ring& r, ImU32 col, float thickness) {
		if (r.count < 2)
			return;
		const float dx = size.x / (float)(Ring::CAP - 1);
		const int first = Ring::CAP - r.count;
		ImVec2 prev(p0.x + first * dx, ytoscreen(std::clamp(r.at(0), lo, hi)));
		for (int i = 1; i < r.count; ++i) {
			const ImVec2 cur(p0.x + (first + i) * dx, ytoscreen(std::clamp(r.at(i), lo, hi)));
			dl->AddLine(prev, cur, col, thickness);
			prev = cur;
		}
	};

	plot(raw, IM_COL32(120, 160, 255, 160), 1.0f);       // what the device said
	plot(processed, IM_COL32(120, 255, 140, 255), 1.6f); // what the game gets

	char txt[96];
	snprintf(txt, sizeof(txt), "%s  raw % .3f   game % .3f", label, raw.newest(), processed.newest());
	dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(210, 210, 220, 255), txt);
}

// The 2D stick gate. This is the view that makes a drifting or square-deadzoned
// stick obvious at a glance, which a pair of time traces does not.
void DrawStickPad(const char* label, const Ring& rx, const Ring& ry,
                  const Ring& px, const Ring& py, float side = 150.0f) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 size(side, side);
	const ImVec2 c(p0.x + side * 0.5f, p0.y + side * 0.5f);
	const float r = side * 0.5f - 4.0f;

	ImGui::InvisibleButton(label, size);

	dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(20, 20, 24, 255));
	dl->AddRect(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(70, 70, 80, 255));
	dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(60, 60, 70, 255));
	dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(60, 60, 70, 255));
	dl->AddCircle(c, r, IM_COL32(70, 70, 80, 255), 48);

	const wi::input::AnalogSettings& cfg = wi::input::GetAnalogSettings();
	dl->AddCircle(c, r * cfg.stick_deadzone, IM_COL32(220, 170, 60, 200), 40);   // deadzone
	dl->AddCircle(c, r * cfg.stick_saturation, IM_COL32(90, 110, 160, 180), 48); // saturation

	// Trail of recent RAW samples, oldest faintest. A still stick that smears here
	// is noisy; one whose smear sits off-centre has drift.
	const int trail = std::min(90, rx.count);
	for (int i = 0; i < trail; ++i) {
		const int idx = rx.count - trail + i;
		const float a = (float)(i + 1) / (float)trail;
		const ImVec2 pt(c.x + std::clamp(rx.at(idx), -1.0f, 1.0f) * r,
		                c.y - std::clamp(ry.at(idx), -1.0f, 1.0f) * r);
		dl->AddCircleFilled(pt, 1.5f, IM_COL32(120, 160, 255, (int)(a * 110.0f)));
	}

	const ImVec2 raw_pt(c.x + std::clamp(rx.newest(), -1.0f, 1.0f) * r,
	                    c.y - std::clamp(ry.newest(), -1.0f, 1.0f) * r);
	const ImVec2 out_pt(c.x + std::clamp(px.newest(), -1.0f, 1.0f) * r,
	                    c.y - std::clamp(py.newest(), -1.0f, 1.0f) * r);
	dl->AddLine(c, out_pt, IM_COL32(120, 255, 140, 120), 1.0f);
	dl->AddCircleFilled(raw_pt, 3.5f, IM_COL32(120, 160, 255, 255));
	dl->AddCircleFilled(out_pt, 4.5f, IM_COL32(120, 255, 140, 255));

	dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(210, 210, 220, 255), label);
}

} // namespace

void GamepadAnalogWindow(bool* show) {
	// Sample BEFORE the early-out: a collapsed or scrolled-away window must not
	// leave a hole in the history the user is about to go looking at.
	const int slot_count = std::min(wi::input::GetControllerCount(), (int)IM_ARRAYSIZE(histories));
	if (!paused) {
		for (int i = 0; i < slot_count; ++i) {
			wi::input::ControllerState state;
			wi::input::GetControllerState(&state, i);
			PushSlot(i, state);
		}
		frame_ms.push(ImGui::GetIO().DeltaTime * 1000.0f);
	}

	ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Gamepad Analog", show)) {
		ImGui::End();
		return;
	}

	// Devices
	ImGui::SeparatorText("Devices");
	if (wi::input::GetControllerCount() == 0) {
		ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No controller registered.");
		ImGui::TextDisabled("A pad plugged in now is picked up on the next frame.");
	} else if (ImGui::BeginTable("##pads", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Player");
		ImGui::TableSetupColumn("Backend");
		ImGui::TableSetupColumn("State");
		ImGui::TableHeadersRow();
		for (int i = 0; i < wi::input::GetControllerCount(); ++i) {
			wi::input::ControllerState st;
			const bool connected = wi::input::GetControllerState(&st, i);
			const char* backend = wi::input::GetControllerBackend(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("%d", i);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(backend ? backend : "?");
			ImGui::TableNextColumn();
			if (connected)
				ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "connected");
			else
				ImGui::TextDisabled("disconnected");
		}
		ImGui::EndTable();
	}

	// One physical pad on two backends is the failure this table is here to catch.
	int live = 0;
	for (int i = 0; i < wi::input::GetControllerCount(); ++i) {
		if (wi::input::GetControllerState(nullptr, i))
			live++;
	}
	if (live > 1) {
		ImGui::TextDisabled("%d live slots. If you only have one pad plugged in, two", live);
		ImGui::TextDisabled("backends have claimed it and player 1 may be reading the wrong one.");
	}

	ImGui::Spacing();
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Player slot", &selected_slot);
	selected_slot = std::clamp(selected_slot, 0, (int)IM_ARRAYSIZE(histories) - 1);
	ImGui::SameLine();
	ImGui::Checkbox("Pause capture", &paused);
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		histories[selected_slot] = SlotHistory();
		frame_ms.clear();
	}

	const SlotHistory& h = histories[selected_slot];

	// Stick gates
	ImGui::SeparatorText("Stick gate  (blue = device, green = what the game reads)");
	DrawStickPad("Left stick", h.lx_raw, h.ly_raw, h.lx, h.ly);
	ImGui::SameLine();
	DrawStickPad("Right stick", h.rx_raw, h.ry_raw, h.rx, h.ry);

	// Traces
	ImGui::SeparatorText("Traces  (10 s, newest on the right)");
	DrawTrace("L X", h.lx_raw, h.lx, -1.0f, 1.0f);
	DrawTrace("L Y", h.ly_raw, h.ly, -1.0f, 1.0f);
	DrawTrace("R X", h.rx_raw, h.rx, -1.0f, 1.0f);
	DrawTrace("R Y", h.ry_raw, h.ry, -1.0f, 1.0f);
	DrawTrace("LT",  h.lt_raw, h.lt,  0.0f, 1.0f);
	DrawTrace("RT",  h.rt_raw, h.rt,  0.0f, 1.0f);

	// Diagnostics
	ImGui::SeparatorText("Diagnostics  (last 2 s of raw)");
	if (ImGui::BeginTable("##diag", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Axis");
		ImGui::TableSetupColumn("Jitter (RMS)");
		ImGui::TableSetupColumn("Peak |raw|");
		ImGui::TableSetupColumn("Verdict");
		ImGui::TableHeadersRow();

		const float dz = wi::input::GetAnalogSettings().stick_deadzone;
		struct Row { const char* name; const Ring* r; };
		const Row rows[] = {
			{ "L X", &h.lx_raw }, { "L Y", &h.ly_raw },
			{ "R X", &h.rx_raw }, { "R Y", &h.ry_raw },
		};
		for (const Row& row : rows) {
			const float jitter = Jitter(*row.r);
			const float peak   = PeakAbs(*row.r);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(row.name);
			ImGui::TableNextColumn(); ImGui::Text("%.4f", jitter);
			ImGui::TableNextColumn(); ImGui::Text("%.3f", peak);
			ImGui::TableNextColumn();
			if (jitter > 0.02f)
				ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "noisy");
			else if (peak > dz * 0.6f && jitter < 0.005f)
				ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "resting off-centre (drift)");
			else
				ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "clean");
		}
		ImGui::EndTable();
	}

	ImGui::Text("Frames since the left stick last changed: %d", h.stale_frames_L);
	ImGui::SameLine();
	ImGui::TextDisabled("(right: %d)", h.stale_frames_R);
	ImGui::TextDisabled("An event-driven backend (SDL) holds its last value between events,");
	ImGui::TextDisabled("so a high count on a stick you ARE moving means events are not arriving.");

	// Frame time. Analog input is applied per frame, so a spiky frame is a spiky
	// input no matter how clean the axis is.
	ImGui::SeparatorText("Frame time");
	{
		float worst = 0, mean = 0;
		const int n = std::min(120, frame_ms.count);
		for (int i = 0; i < n; ++i) {
			const float v = frame_ms.at(frame_ms.count - n + i);
			worst = std::max(worst, v);
			mean += v;
		}
		if (n > 0)
			mean /= (float)n;
		ImGui::Text("mean %.2f ms   worst %.2f ms", mean, worst);
		if (worst > mean * 2.0f + 4.0f)
			ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
				"Frame time is spiking. Analog input integrated against this reads as unstable\n"
				"even when the axis itself is clean.");
		static float scratch[Ring::CAP];
		for (int i = 0; i < frame_ms.count; ++i)
			scratch[i] = frame_ms.at(i);
		ImGui::PlotLines("##frametime", scratch, frame_ms.count, 0, nullptr,
		                 0.0f, std::max(33.0f, worst), ImVec2(ImGui::GetContentRegionAvail().x, 50));
	}

	// Live deadzone. Set the deadzone to 0 and the green line should sit exactly on
	// the blue one; anything else is a backend still doing its own shaping.
	ImGui::SeparatorText("Deadzone (live, shared by every backend)");
	{
		wi::input::AnalogSettings& cfg = wi::input::GetAnalogSettings();
		ImGui::SliderFloat("Stick deadzone", &cfg.stick_deadzone, 0.0f, 0.5f, "%.3f");
		ImGui::SliderFloat("Stick saturation", &cfg.stick_saturation, 0.5f, 1.0f, "%.3f");
		ImGui::SliderFloat("Trigger deadzone", &cfg.trigger_deadzone, 0.0f, 0.5f, "%.3f");
		ImGui::SliderFloat("Trigger saturation", &cfg.trigger_saturation, 0.5f, 1.0f, "%.3f");
		if (ImGui::Button("Reset to defaults"))
			cfg = wi::input::AnalogSettings();
		ImGui::SameLine();
		if (ImGui::Button("Deadzone off (raw passthrough)")) {
			cfg.stick_deadzone = 0.0f;
			cfg.trigger_deadzone = 0.0f;
			cfg.stick_saturation = 1.0f;
			cfg.trigger_saturation = 1.0f;
		}
	}

	ImGui::End();
}


// ---------------------------------------------------------------------------
// Input Actions
// ---------------------------------------------------------------------------
namespace {

// One runtime the panel drives itself, so an action's live value is visible even
// when no entity in the scene has an stInput component pointed at that map yet.
// It is bound to whatever map the panel is showing and evaluated once per frame.
st::input::ActionRuntime preview_runtime;
int  preview_player = 0;
int  selected_map = 0;
int  selected_action = 0;

const char* PartName(st::input::Part p) {
	switch (p) {
	case st::input::Part::Value: return "value";
	case st::input::Part::X:     return "x";
	case st::input::Part::Y:     return "y";
	case st::input::Part::Up:    return "up";
	case st::input::Part::Down:  return "down";
	case st::input::Part::Left:  return "left";
	case st::input::Part::Right: return "right";
	}
	return "?";
}

// A horizontal fill bar for one component of an action's value. Signed values are
// drawn from the middle out, so pushing a stick left and right is symmetric.
void ValueBar(const char* label, float v, bool signedRange) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 size(ImGui::GetContentRegionAvail().x, 16.0f);
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
	ImGui::InvisibleButton(label, size);

	dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
	dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));

	const float clamped = std::clamp(v, -1.0f, 1.0f);
	if (signedRange) {
		const float mid = (p0.x + p1.x) * 0.5f;
		dl->AddLine(ImVec2(mid, p0.y), ImVec2(mid, p1.y), IM_COL32(90, 90, 100, 255));
		const float end = mid + clamped * (size.x * 0.5f);
		dl->AddRectFilled(ImVec2(std::min(mid, end), p0.y + 2), ImVec2(std::max(mid, end), p1.y - 2),
		                  IM_COL32(120, 255, 140, 220));
	} else {
		dl->AddRectFilled(ImVec2(p0.x, p0.y + 2),
		                  ImVec2(p0.x + std::max(0.0f, clamped) * size.x, p1.y - 2),
		                  IM_COL32(120, 255, 140, 220));
	}
	char txt[48];
	snprintf(txt, sizeof(txt), "%s % .3f", label, v);
	dl->AddText(ImVec2(p0.x + 5, p0.y + 1), IM_COL32(210, 210, 220, 255), txt);
}

} // namespace

void InputActionsWindow(bool* show) {
	st::input::Registry& reg = st::InputSystem::Get().Actions();

	// Keep the preview runtime pointed at the visible map and evaluated every frame,
	// BEFORE the early-out, so the live column is never a frame behind the tree.
	if (!reg.Maps().empty()) {
		selected_map = std::clamp(selected_map, 0, (int)reg.Maps().size() - 1);
		preview_runtime.Bind(&reg, reg.Maps()[selected_map].name, preview_player);
		preview_runtime.Evaluate(ImGui::GetIO().DeltaTime);
	}

	ImGui::SetNextWindowSize(ImVec2(820, 480), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Input Actions", show)) {
		ImGui::End();
		return;
	}

	if (reg.Maps().empty()) {
		ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No action maps registered.");
		ImGui::TextDisabled("Register them from st::App::OnKeyRegister(st::input::Registry&).");
		ImGui::End();
		return;
	}

	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Preview player", &preview_player);
	preview_player = std::clamp(preview_player, 0, 7);
	ImGui::SameLine();
	ImGui::TextDisabled("(the live column reads this gamepad slot)");
	ImGui::Separator();

	const float paneH = ImGui::GetContentRegionAvail().y;

	// --- Action Maps -------------------------------------------------------
	ImGui::BeginChild("##maps", ImVec2(160, paneH), true);
	ImGui::SeparatorText("Action Maps");
	for (int i = 0; i < (int)reg.Maps().size(); ++i) {
		if (ImGui::Selectable(reg.Maps()[i].name.c_str(), i == selected_map)) {
			selected_map = i;
			selected_action = 0;
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	st::input::ActionMap& map = reg.Maps()[selected_map];
	selected_action = map.actions.empty() ? 0 : std::clamp(selected_action, 0, (int)map.actions.size() - 1);

	// --- Actions + bindings ------------------------------------------------
	ImGui::BeginChild("##actions", ImVec2(330, paneH), true);
	ImGui::SeparatorText("Actions");
	for (int i = 0; i < (int)map.actions.size(); ++i) {
		st::input::Action& a = map.actions[i];
		ImGui::PushID(i);

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow
		                         | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (i == selected_action)
			flags |= ImGuiTreeNodeFlags_Selected;
		const bool open = ImGui::TreeNodeEx("##node", flags, "%s", a.name.c_str());
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			selected_action = i;

		// A live dot next to the action name: green while it is down. This is the
		// fastest way to answer "is my binding even reaching the action".
		const st::input::ActionState& st_ = preview_runtime.State(i);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::TextColored(st_.down ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(0.35f, 0.35f, 0.4f, 1.0f), "*");

		if (open) {
			for (const st::input::Binding& b : a.bindings) {
				ImGui::Bullet();
				ImGui::TextDisabled("%s", b.Describe().c_str());
			}
			if (a.bindings.empty())
				ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "  no bindings");
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// --- Properties --------------------------------------------------------
	ImGui::BeginChild("##props", ImVec2(0, paneH), true);
	ImGui::SeparatorText("Action Properties");
	if (map.actions.empty()) {
		ImGui::TextDisabled("This map has no actions.");
		ImGui::EndChild();
		ImGui::End();
		return;
	}

	st::input::Action& a = map.actions[selected_action];
	ImGui::Text("%s", a.name.c_str());
	ImGui::Separator();
	ImGui::Text("Action Type   %s", st::input::ToString(a.actionType));
	ImGui::Text("Control Type  %s", st::input::ToString(a.controlType));
	ImGui::SetNextItemWidth(140);
	ImGui::SliderFloat("Press point", &a.pressPoint, 0.0f, 1.0f, "%.2f");

	ImGui::SeparatorText("Live value");
	const st::input::ActionState& state = preview_runtime.State(selected_action);
	if (a.controlType == st::input::ControlType::Vector2) {
		ValueBar("x", state.value.x, true);
		ValueBar("y", state.value.y, true);
	} else {
		ValueBar("value", state.value.x, a.controlType == st::input::ControlType::Axis);
	}
	ImGui::Text("down %d   pressed %d   released %d   held %.2fs",
		(int)state.down, (int)state.pressed, (int)state.released, state.heldTime);
	ImGui::Text("last device: %s", st::input::ToString(state.lastDevice));

	// Per-binding processors. The STRUCTURE is registered in code and not editable
	// here on purpose - it would be an edit with nowhere to persist to - but scale and
	// invert are exactly what tuning a look sensitivity needs, and they are live.
	ImGui::SeparatorText("Bindings");
	if (!ImGui::BeginTable("##bindings", 4,
			ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
		ImGui::EndChild();
		ImGui::End();
		return;
	}
	ImGui::TableSetupColumn("Control");
	ImGui::TableSetupColumn("Scheme");
	ImGui::TableSetupColumn("Part");
	ImGui::TableSetupColumn("Processors");
	ImGui::TableHeadersRow();
	for (int i = 0; i < (int)a.bindings.size(); ++i) {
		st::input::Binding& b = a.bindings[i];
		ImGui::PushID(i);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextUnformatted(b.Describe().c_str());
		ImGui::TableNextColumn();
		if (b.scheme.empty()) ImGui::TextDisabled("(any)");
		else                  ImGui::TextUnformatted(b.scheme.c_str());
		ImGui::TableNextColumn(); ImGui::TextDisabled("%s", PartName(b.part));
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(90);
		ImGui::DragFloat("##scale", &b.scale, 0.01f, -10.0f, 10.0f, "x%.2f");
		ImGui::SameLine();
		ImGui::Checkbox("inv", &b.invert);
		ImGui::PopID();
	}
	ImGui::EndTable();

	ImGui::TextDisabled("Structure is registered in code (st::App::OnKeyRegister) and is not");
	ImGui::TextDisabled("editable here. Scale and invert are live and are NOT persisted.");

	ImGui::EndChild();
	ImGui::End();
}

} // namespace st::devui
