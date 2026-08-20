#pragma once
// Scene debug UI for Native Components.
//	Iterates every live NativeComponent instance in the scene and calls its DrawDebug()
//	override, grouped per entity. Editing a widget mutates the instance's live member, so the
//	change is visible on the next NativeComponent::Update(). Values are NOT written back to the
//	MetadataComponent, so they are not persisted on scene save (see stNativeComponent.h).

namespace wi::scene { struct Scene; }

// Emit the native-component tree as ImGui widgets into the CURRENT window (no Begin/End).
//	Drop into any existing panel: ImGui::Begin(...); NativeComponentsGUI(scene); ImGui::End();
void NativeComponentsGUI(wi::scene::Scene& scene);

// Convenience: open a standalone "Native Components" window and draw the tree inside it.
//	p_open is optional (pass a bool* to get a close button), matching ImGui::Begin convention.
void NativeComponentsWindow(wi::scene::Scene& scene, bool* p_open = nullptr);
