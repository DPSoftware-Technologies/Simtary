#pragma once
// The "Add Component" catalogue for the Properties inspector.
//
//	Two families sit behind one menu, exactly as the user thinks about them:
//
//	  Engine components  - the wi::scene::Scene component managers (Transform, Light,
//	                       Object, Material, ... 38 of them). Added/removed by calling
//	                       Create()/Remove() on the matching manager. This is live scene
//	                       state; it is written to disk only when the scene is saved.
//
//	  Native components  - the project's own C++ classes registered with
//	                       ST_REGISTER_NATIVE_COMPONENT. Added/removed by writing the
//	                       NCI_/NCA_/NCE_ metadata keys (AttachNativeComponent /
//	                       DetachNativeComponent in Engine/stNativeComponent.h), which the
//	                       NativeComponentManager reconciles on the next frame - so attach
//	                       and detach are genuinely realtime, and they persist with the scene.
//
//	The engine table is hand-written rather than derived from Scene::componentLibrary
//	because ComponentManager_Interface deliberately exposes no type-erased Create(): a
//	component has to be constructed as its concrete type. The table is the one place that
//	knows the mapping, and it is checked against the library key at runtime.

#include "wiECS.h"

#include <string>

namespace wi::scene { struct Scene; }
namespace st { class EditorHistory; }

// One engine component type as the inspector sees it. The three hooks are plain function
//	pointers (capture-less lambdas), so the table is a compile-time constant.
struct EngineComponentType
{
	const char* name;        // display name, e.g. "Transform"
	const char* libraryKey;  // ComponentLibrary key, e.g. "wi::scene::Scene::transforms"
	bool  needsSetup;        // bare Create() gives an object that still needs data to be useful
	bool (*Has)(wi::scene::Scene&, wi::ecs::Entity);
	void (*Add)(wi::scene::Scene&, wi::ecs::Entity);
	void (*Remove)(wi::scene::Scene&, wi::ecs::Entity);
};

// The full table and its size. Ordered as the components are declared on Scene.
const EngineComponentType* EngineComponentTable(size_t& count);
// Look a type up by its ComponentLibrary key ("wi::scene::Scene::lights"); nullptr if unknown.
const EngineComponentType* FindEngineComponentByKey(const std::string& libraryKey);

// The "+ Add Component" button plus its popup (engine list + native list, both filtered by
//	one search box). Draws into the CURRENT window. Returns true on the frame something was
//	attached, so the caller can rebuild anything it caches per entity.
//	`history` is optional: pass one and the attach becomes a single undo step.
bool AddComponentButton(wi::scene::Scene& scene, wi::ecs::Entity entity,
	st::EditorHistory* history = nullptr);

// Small "x" button that detaches an engine component. Draw it on the same line as the
//	component's header. Returns true on the frame it removed the component - the caller must
//	stop touching that component's pointer for the rest of the frame.
bool RemoveEngineComponentButton(wi::scene::Scene& scene, wi::ecs::Entity entity,
	const EngineComponentType& type, st::EditorHistory* history = nullptr);

// ------------------------------------------------------------------ new object ---

// Put a freshly created or freshly imported entity at `position` (origin-relative float
//	space, the same space the editor's spawn point is in) and, if asked, attach it under
//	`parent`.
//
//	Writes the 64-bit ABSOLUTE position as well as the local one. That is the field
//	TransformComponent::Serialize round-trips, so an entity placed only in local space comes
//	back at the world origin after a save/load - or after an undo/redo.
void PlaceEntityAt(wi::scene::Scene& scene, wi::ecs::Entity e, const XMFLOAT3& position,
	wi::ecs::Entity parent = wi::ecs::INVALID_ENTITY);

// The "Create" menu: emits menu items for every kind of object the editor can spawn
//	(empty, cube/sphere/plane, the three light types, camera, probe, decal, force field,
//	emitter, hair, sound, weather) into the CURRENT menu or popup.
//
//	spawnPosition : where the new object lands, in origin-relative float space. The editor
//	                passes a point in front of its free camera.
//	parent        : optional - the new entity is attached under it (Scene::Component_Attach).
//	history       : optional - the create becomes one undo step.
//
//	Returns the new entity, or INVALID_ENTITY when nothing was picked this frame.
wi::ecs::Entity CreateObjectMenuItems(wi::scene::Scene& scene, const XMFLOAT3& spawnPosition,
	wi::ecs::Entity parent, st::EditorHistory* history = nullptr);
