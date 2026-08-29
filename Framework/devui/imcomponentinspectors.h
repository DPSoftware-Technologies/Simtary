#pragma once
// Full property editors for every engine component, for the Properties inspector.
//
//	imcomponents.h owns the CATALOGUE (what can be attached, and how). This file owns the
//	EDITORS: one function per wi::scene::Scene component manager, exposing every option the
//	component actually has rather than the handful the panel used to show. All 38 managers
//	are covered, so the inspector's "no inline editor" fallback is now a bug report rather
//	than a normal state.
//
//	Two of the 38 are NOT drawn here and stay in imhierarchy.cpp:
//
//	  Name       — its widget shares a row with the detach button, so it is laid out by hand
//	               above everything else.
//	  Transform  — it carries the euler cache and the "something else is driving this
//	               transform" drift check, which are Properties-panel behaviour, not a field
//	               list.
//
//	HasEngineComponentInspector() still answers true for both: it means "the Properties
//	panel has a full editor for this key", which is what the caller's fallback asks.
//
//	── Undo ──────────────────────────────────────────────────────────────────────
//
//	Every widget here is bracketed by TrackEdit (see the .cpp): BeginEntity on the frame
//	ImGui makes the item active, Commit on the frame it goes inactive HAVING changed
//	something, Abort otherwise. So one drag is one undo step no matter how many frames it
//	spans, and a drag that ends where it started records nothing. The capture is an archive
//	of the whole entity — the same step kind a component add/remove uses — because a
//	component's fields are not all plain values (a texture name reloads a resource, a
//	physics field rebuilds a body) and only the archive puts all of that back.
//
//	── What is editable ──────────────────────────────────────────────────────────
//
//	Serialized fields are editable. Non-serialized ones (a computed AABB, a GPU buffer, a
//	resolved descriptor index, a physics body pointer) are shown read-only where they are
//	worth seeing and omitted where they are not — writing them lasts until the system that
//	owns them runs again, which reads as the field being broken.

#include "wiECS.h"

#include <string>

namespace wi::scene { struct Scene; }
namespace st { class EditorHistory; }

namespace st::devui {

// ── shared inspector helpers ──────────────────────────────────────────────────
// Defined here rather than in imhierarchy.cpp because both files draw components.

// The entity's NameComponent, or "Entity <id>" when it has none.
std::string EntityLabel(wi::scene::Scene& scene, wi::ecs::Entity e);

// Inline std::string text edit through a fixed scratch buffer. Returns true on change.
bool EditString(const char* label, std::string& s);

// One component's collapsing header plus the right-aligned "x" that detaches it.
//	Returns true when the caller should draw the component's body — i.e. the header is open
//	AND the component still exists. The "x" calls ComponentManager::Remove immediately, so
//	any pointer the caller is holding is dangling the moment this returns false.
bool ComponentHeader(wi::scene::Scene& scene, wi::ecs::Entity e, const char* libraryKey,
	const char* label, bool defaultOpen, st::EditorHistory* history);

// ── the inspectors ────────────────────────────────────────────────────────────

// Draw every engine component present on `entity` EXCEPT names and transforms, in the same
//	order the Add Component catalogue lists them. Each draws its own header and detach
//	button; absent components draw nothing. `history` is optional.
void EngineComponentInspectors(wi::scene::Scene& scene, wi::ecs::Entity entity,
	st::EditorHistory* history = nullptr);

// True when the Properties panel has a full editor for that ComponentLibrary key
//	("wi::scene::Scene::lights"). Includes names and transforms, which the panel draws
//	itself. The inspector's leftover list is built from whatever this rejects.
bool HasEngineComponentInspector(const std::string& libraryKey);

} // namespace st::devui
