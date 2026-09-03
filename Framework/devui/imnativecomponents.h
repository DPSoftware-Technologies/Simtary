#pragma once
// Scene debug UI for Native Components.
//	Iterates every live NativeComponent instance in the scene and calls its DrawDebug()
//	override, grouped per entity. Editing a widget mutates the instance's live member, so the
//	change is visible on the next NativeComponent::Update().
//
//	Whether that edit is SAVED is the component's own call: DrawDebug() writes the member and
//	nothing else, so a component persists its edits by calling SaveBoundParams() once its
//	widgets report a change (every component in Framework/ does). Parameters described through
//	DescribeParams() are written back by NativeComponentParamsGUI below either way.

namespace wi::scene { struct Scene; struct NativeComponent; }

// Draw the widgets a component describes through NativeComponent::DescribeParams(),
// writing every edit back to the NCA_ metadata it came from (so it persists with the
// scene, unlike DrawDebug()).
//
// Shared deliberately: native components are rendered in TWO places - this file's
// standalone "Native Components" window, and the "Native Components" section of the
// Properties inspector (imhierarchy.cpp). A component that appears in one and not the
// other is the bug this being one function prevents.
//
// Emits into the CURRENT window; no Begin/End.
void NativeComponentParamsGUI(wi::scene::NativeComponent& component);

// Emit the native-component tree as ImGui widgets into the CURRENT window (no Begin/End).
//	Drop into any existing panel: ImGui::Begin(...); NativeComponentsGUI(scene); ImGui::End();
void NativeComponentsGUI(wi::scene::Scene& scene);

// Convenience: open a standalone "Native Components" window and draw the tree inside it.
//	p_open is optional (pass a bool* to get a close button), matching ImGui::Begin convention.
void NativeComponentsWindow(wi::scene::Scene& scene, bool* p_open = nullptr);
