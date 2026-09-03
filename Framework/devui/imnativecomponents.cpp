#include "imnativecomponents.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "imcomponentinspectors.h"
#include "imgui.h"

#include <string>
#include <vector>

// Draw the widgets a component describes through DescribeParams(), and write every
// edit back to the NCA_ metadata that produced it.
//
// This is the inspector path for a component that CANNOT use DrawDebug(): ImGui is
// linked at the app level, so anything living in Engine/ (the audio emitter and
// collector, for instance) has no way to call it. The component supplies plain data
// and this decides how to render it.
//
// An edit here PERSISTS with no help from the component: the live member is updated so
// the change is audible/visible on the next Update, and Set*() writes the same NCA_ key
// that Bind() reads, so it survives a save and reload. A hand-drawn DrawDebug() reaches
// the same place, but only by calling SaveBoundParams() itself.
void NativeComponentParamsGUI(wi::scene::NativeComponent& component)
{
	wi::vector<wi::scene::NativeComponent::NativeParam> params;
	component.DescribeParams(params);
	if (params.empty())
		return;

	using Param = wi::scene::NativeComponent::NativeParam;
	const char* openGroup = nullptr;
	bool groupVisible = true;

	for (const Param& p : params)
	{
		if (p.name == nullptr)
			continue;
		// Action and Live carry function pointers instead of a member address; every
		// other type is a pointer into the component and is useless without one.
		const bool functional = (p.type == Param::Type::Action || p.type == Param::Type::Live);
		if (!functional && p.value == nullptr)
			continue;
		if (functional && p.action == nullptr && p.liveGet == nullptr)
			continue;

		// Group headers. Compared by POINTER, not by strcmp: the groups come from
		// string literals in the component, so consecutive params in one section share
		// the same address and this stays a pointer compare per row.
		if (p.group != openGroup)
		{
			openGroup = p.group;
			groupVisible = (p.group == nullptr) ||
				ImGui::CollapsingHeader(p.group, ImGuiTreeNodeFlags_DefaultOpen);
		}
		if (!groupVisible)
			continue;

		if (p.sameLine)
			ImGui::SameLine();
		ImGui::PushID(p.name);
		bool changed = false;
		switch (p.type)
		{
		case Param::Type::Action:
		{
			// A button is an act, not a value: nothing is written to metadata, because
			// "the user pressed Play" is not scene state.
			if (ImGui::Button(p.name))
				p.action(component);
			break;
		}
		case Param::Type::Live:
		{
			const float value = p.liveGet(component);
			if (p.liveSet != nullptr)
			{
				// Scrubber. The upper bound can move under it (a clip is swapped), so it
				// is queried每 frame rather than baked in.
				float v = value;
				const float hi = p.liveMax ? p.liveMax(component) : p.maxValue;
				if (ImGui::SliderFloat(p.name, &v, p.minValue, hi > p.minValue ? hi : p.minValue + 1.0f))
					p.liveSet(component, v);
			}
			else if (p.bar)
			{
				ImGui::ProgressBar(value, ImVec2(-FLT_MIN, 0));
				ImGui::SameLine();
				ImGui::TextUnformatted(p.name);
			}
			else
			{
				ImGui::Text(p.format ? p.format : "%.3f", value);
				ImGui::SameLine();
				ImGui::TextUnformatted(p.name);
			}
			break;
		}
		case Param::Type::Bool:
		{
			bool* v = (bool*)p.value;
			if (ImGui::Checkbox(p.name, v))
			{
				component.SetBool(p.name, *v);
				changed = true;
			}
			break;
		}
		case Param::Type::Enum:
		{
			int* v = (int*)p.value;
			if (p.labels != nullptr && ImGui::Combo(p.name, v, p.labels))
			{
				component.SetInt(p.name, *v);
				changed = true;
			}
			break;
		}
		case Param::Type::Int:
		{
			int* v = (int*)p.value;
			const bool ranged = p.maxValue > p.minValue;
			if (ranged ? ImGui::SliderInt(p.name, v, (int)p.minValue, (int)p.maxValue)
			           : ImGui::DragInt(p.name, v))
			{
				component.SetInt(p.name, *v);
				changed = true;
			}
			break;
		}
		case Param::Type::Float:
		{
			float* v = (float*)p.value;
			const bool ranged = p.maxValue > p.minValue;
			if (ranged ? ImGui::SliderFloat(p.name, v, p.minValue, p.maxValue)
			           : ImGui::DragFloat(p.name, v, 0.01f))
			{
				component.SetFloat(p.name, *v);
				changed = true;
			}
			break;
		}
		case Param::Type::String:
		{
			std::string* v = (std::string*)p.value;
			if (p.asset)
			{
				// Asset path: the same field engine components use, so a row dragged out
				// of the Resource Explorer drops onto a native component exactly as it
				// does onto a material's texture slot.
				//
				// AssetDropField returns true only for a DROP. A typed edit is committed
				// separately on IsItemDeactivatedAfterEdit - writing metadata on every
				// keystroke would persist "assets/au", "assets/aud", ... as the user types.
				const bool dropped = st::devui::AssetDropField(p.name, *v);
				if (dropped || ImGui::IsItemDeactivatedAfterEdit())
				{
					component.SetString(p.name, *v);
					changed = true;
				}
			}
			else
			{
				// ImGui edits a fixed buffer, so the string is copied out and back.
				char buffer[260];
				const size_t n = v->copy(buffer, sizeof(buffer) - 1);
				buffer[n] = 0;
				if (ImGui::InputText(p.name, buffer, sizeof(buffer),
					ImGuiInputTextFlags_EnterReturnsTrue))
				{
					*v = buffer;
					component.SetString(p.name, *v);
					changed = true;
				}
			}
			break;
		}
		}
		if (p.tooltip != nullptr && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", p.tooltip);
		ImGui::PopID();
		(void)changed;
	}
}

void NativeComponentsGUI(wi::scene::Scene& scene)
{
	// Global threading switch. Turning it off puts every component back on the main thread in
	//	instance order - the first thing to try when a bug looks like a race between two
	//	components rather than a bug inside one.
	bool mt = wi::scene::NativeComponentManager::multithreading;
	if (ImGui::Checkbox("multithreaded", &mt))
		wi::scene::NativeComponentManager::multithreading = mt;
	ImGui::SameLine();
	ImGui::TextDisabled("(%u worker threads)", wi::jobsystem::GetThreadCount());
	ImGui::Separator();

	auto& instances = scene.nativeComponents.instances;
	if (instances.empty())
	{
		ImGui::TextDisabled("No native components attached.");
		return;
	}

	for (auto& pair : instances)
	{
		const wi::ecs::Entity entity = pair.first;
		wi::vector<wi::scene::NativeComponentManager::Instance>& list = pair.second;
		if (list.empty())
			continue;

		// Entity header: prefer the NameComponent, fall back to the raw id.
		std::string label;
		if (const wi::scene::NameComponent* nc = scene.names.GetComponent(entity); nc && !nc->name.empty())
			label = nc->name + "  (entity " + std::to_string(entity) + ")";
		else
			label = "entity " + std::to_string(entity);

		ImGui::PushID((int)entity);
		if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Indent();
			for (wi::scene::NativeComponentManager::Instance& inst : list)
			{
				if (!inst.component)
					continue;

				ImGui::PushID(inst.localID);
				// One node per component instance: "Spinner [0]"
				const std::string title = inst.name + " [" + std::to_string(inst.localID) + "]";
				if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				{
					// Enable/disable toggle -> writes NCE_<id> metadata (persisted).
					//	Edge fires OnEnable/OnDisable on the next NativeComponent update.
					bool enabled = inst.component->IsEnabled();
					if (ImGui::Checkbox("enabled", &enabled))
						inst.component->SetEnabled(enabled);
					ImGui::SameLine();
					ImGui::TextDisabled(inst.started ? "(started)" : "(awaiting start)");
					ImGui::SameLine();
					// Where this instance's Compute/FixedUpdate/Update actually ran.
					if (inst.component->GetThreading() == wi::scene::NativeThreading::Parallel &&
						wi::scene::NativeComponentManager::multithreading)
						ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "[parallel]");
					else
						ImGui::TextDisabled("[main thread]");

					ImGui::BeginDisabled(!enabled);
					// Described parameters first — persisted by this file — then the
					// component's own hand-drawn widgets, which persist only as far as
					// their DrawDebug() calls SaveBoundParams(). A component may use
					// either or both.
					NativeComponentParamsGUI(*inst.component);
					inst.component->DrawDebug();
					ImGui::EndDisabled();
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
}

void NativeComponentsWindow(wi::scene::Scene& scene, bool* p_open)
{
	if (ImGui::Begin("Native Components", p_open))
		NativeComponentsGUI(scene);
	ImGui::End();
}
