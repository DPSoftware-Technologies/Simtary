#pragma once
// Scene Hierarchy (Explorer) + Properties (Inspector) debug UI.
//
//	Hierarchy lists every entity in the scene as a parent/child tree (built from the
//	HierarchyComponent links) and lets you click one to select it. Properties shows the
//	selected entity's components -- BOTH engine components (Transform, Light, Camera,
//	Object, Material, Metadata, ...) and Native Components -- and edits them live: a
//	change is written straight into the component and picked up by the next scene Update.
//
//	Engine-component edits are NOT persisted to disk on their own (they mutate the live
//	scene); Native-Component "enabled" toggles do write metadata (see imnativecomponents.h).
//
//	Selection is just a wi::ecs::Entity the caller owns, so the two windows can live
//	independently and share state without this module holding any global.

#include "wiECS.h"

#include <string>

namespace wi::scene { struct Scene; struct NativeComponent; }
namespace st { class EditorHistory; }

// ImGui drag-drop payload type that Hierarchy rows publish and EntityField accepts.
//	Payload is a single wi::ecs::Entity. Exposed so other panels can author drop targets too.
inline constexpr const char* SIMTARY_ENTITY_PAYLOAD = "SIMTARY_ENTITY";

// Unity-like "object field": a button you can drag an entity from the Hierarchy onto.
//	Safe to draw inside the Properties panel: a Hierarchy drag does not change the selection, so
//	the field under the cursor survives until the drop.
//	Shows the current target's name (or "(none)"). On drop, points the native component's
//	`paramName` entity-ref at the dropped entity and persists it to metadata (GUID-based, so it
//	survives renames / duplicate names / scene reload, and stays editable in the Wicked Editor).
//	Returns true on the frame the reference changed. Right-click clears the field.
bool EntityField(wi::scene::Scene& scene, const char* label,
	wi::scene::NativeComponent& comp, const char* paramName);

// Emit the hierarchy tree into the CURRENT window (no Begin/End). Clicking a row writes
//	the clicked entity into `selected`.
//
//	A row is BOTH a selection target and a drag source, so the two are told apart by the mouse
//	release, not the press: selection only moves when the press and the release land on the same
//	row without passing the drag threshold. Dragging a row therefore leaves `selected` (and with
//	it the Properties panel you are dropping onto) exactly where it was. The panel shows which
//	mode it is in above the tree, and tints the row being carried.
//
//	`history` is optional. Pass one (Editor mode does) and the panel also grows a Create
//	button and a right-click menu — create child / duplicate / delete — each recorded as a
//	single undo step. Without it the panel is read-only apart from selection, which is what
//	the plain DevUI Hierarchy window wants.
void HierarchyGUI(wi::scene::Scene& scene, wi::ecs::Entity& selected,
	st::EditorHistory* history = nullptr);

// Standalone "Hierarchy" window wrapper. p_open is optional (pass a bool* for a close box).
void HierarchyWindow(wi::scene::Scene& scene, wi::ecs::Entity& selected, bool* p_open = nullptr);

// Emit the inspector for `selected` into the CURRENT window (no Begin/End). Pass `selected`
//	by reference so the panel can clear it (set to INVALID_ENTITY) when the entity is gone.
//	`history` is optional; pass one to make every edit here undoable.
void PropertiesGUI(wi::scene::Scene& scene, wi::ecs::Entity& selected,
	st::EditorHistory* history = nullptr);

// Standalone "Properties" window wrapper. p_open is optional.
void PropertiesWindow(wi::scene::Scene& scene, wi::ecs::Entity& selected, bool* p_open = nullptr);
