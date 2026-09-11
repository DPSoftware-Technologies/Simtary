#include "input/InputComponent.h"
#include "input/InputSystem.h"

#include "imgui.h"

#include <cmath>
#include <string>

namespace st {

void InputComponent::Start() {
	Bind(actionMap, "actionMap");
	Bind(playerIndex, "playerIndex");
	runtime_.Bind(&InputSystem::Get().Actions(), actionMap, playerIndex);
	boundMap_ = actionMap;
	boundPlayer_ = playerIndex;
}

void InputComponent::SwitchMap(const std::string& name) {
	actionMap = name;
	// Re-binding clears the state vector, so nothing carries over from the old map.
	runtime_.Bind(&InputSystem::Get().Actions(), actionMap, playerIndex);
	boundMap_ = actionMap;
}

void InputComponent::Compute(float dt) {
	// Re-bind when the map or the player slot was changed - by SwitchMap, by the
	//	inspector, or by a map that only got registered after this component started
	//	(a scene loaded before OnKeyRegister ran leaves the runtime invalid until then).
	if (actionMap != boundMap_ || playerIndex != boundPlayer_ || !runtime_.Valid()) {
		runtime_.Bind(&InputSystem::Get().Actions(), actionMap, playerIndex);
		boundMap_ = actionMap;
		boundPlayer_ = playerIndex;
	}

	// The frame snapshot (pointer delta, wheel, touches) was latched on the main
	//	thread by InputSystem::Update, which runs before the scene updates. Everything
	//	read from here is either that snapshot or a const wi::input query.
	runtime_.Evaluate(dt);
}

void InputComponent::DescribeParams(wi::vector<NativeParam>& out) {
	out.push_back(NativeParam::String("actionMap", &actionMap,
		"Name of a map registered in st::App::OnKeyRegister (\"Player\", \"UI\", ...)"));
	out.push_back(NativeParam::Int("playerIndex", &playerIndex, 0, 7,
		"Gamepad slot this entity listens to. Keyboard and mouse ignore it."));
}

void InputComponent::DrawDebug() {
	bool dirty = false;
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", actionMap.c_str());
	if (ImGui::InputText("Action map", buf, sizeof(buf))) {
		actionMap = buf;
		dirty = true;
	}
	dirty |= ImGui::InputInt("Player index", &playerIndex);
	if (dirty) SaveBoundParams();

	const st::input::ActionMap* map = runtime_.Map();
	if (map == nullptr) {
		ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Map \"%s\" is not registered.", actionMap.c_str());
		ImGui::TextDisabled("Register it from st::App::OnKeyRegister(). Available maps are");
		ImGui::TextDisabled("listed in Simtary > Input Actions.");
		return;
	}

	// This table shows what the GAME sees, gate and all - which is why every value can sit
	//	at 0 with a stick clearly deflected. The editor raises an input capture whenever the
	//	Game Viewport is not the focused panel, and focusing THIS panel to read the table is
	//	what raises it. Say so, or the component looks broken.
	{
		const st::input::DeviceGate& gate = st::input::Gate();
		if (gate.windowUnfocused) {
			ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Window not focused - all input suppressed.");
		} else {
			std::string held;
			for (int d = 0; d < (int)st::input::DeviceClass::Count; ++d) {
				if (!gate.suppressed[d])
					continue;
				if (!held.empty()) held += ", ";
				held += st::input::ToString((st::input::DeviceClass)d);
			}
			if (!held.empty()) {
				ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "UI owns: %s", held.c_str());
				ImGui::TextDisabled("Values below stay 0 until the Game Viewport has focus.");
				ImGui::TextDisabled("Simtary > Input Actions reads the same maps ungated.");
			}
		}
	}

	ImGui::Separator();
	if (!ImGui::BeginTable("##actions", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("Action");
	ImGui::TableSetupColumn("Type");
	ImGui::TableSetupColumn("Value");
	ImGui::TableSetupColumn("State");
	ImGui::TableHeadersRow();
	for (int i = 0; i < (int)map->actions.size(); ++i) {
		const st::input::Action& a = map->actions[i];
		const st::input::ActionState& s = runtime_.State(i);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextUnformatted(a.name.c_str());
		ImGui::TableNextColumn(); ImGui::TextDisabled("%s", st::input::ToString(a.controlType));
		ImGui::TableNextColumn();
		if (a.controlType == st::input::ControlType::Vector2)
			ImGui::Text("% .2f, % .2f", s.value.x, s.value.y);
		else
			ImGui::Text("% .2f", s.value.x);
		ImGui::TableNextColumn();
		if (s.down)
			ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "down %.2fs [%s]",
				s.heldTime, st::input::ToString(s.lastDevice));
		else
			ImGui::TextDisabled("-");
	}
	ImGui::EndTable();
}

} // namespace st

// The registration macro pastes the type name into an identifier, so it cannot take a
// qualified one. The alias IS the type, so GetComponent<st::InputComponent>() still
// resolves to the same identity.
using StInputComponent = st::InputComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StInputComponent, "stInput", "Input")
