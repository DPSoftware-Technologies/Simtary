#include "imnativecomponents.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "imgui.h"

#include <string>

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
					inst.component->DrawDebug();   // component-supplied widgets bound to live members
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
