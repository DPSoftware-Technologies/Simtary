#pragma once
// Native Component system for the Simtary engine.
//	A Unity-like component model for C++: attach C++ classes to scene entities ("GameObjects")
//	and drive them with Start()/Update()/Destroy() lifecycle callbacks.
//
//	Attachment is data-driven through the engine's MetadataComponent so it stays fully
//	compatible with the Wicked Editor. Two metadata conventions are used:
//
//		NCI_<LocalID>                            = "ComponentName"   (string)
//			"Native Component Import": attaches the component named ComponentName to the entity.
//			LocalID lets you stack several instances of the same component on one entity.
//
//		NCA_<LocalID>_<ArgName>                  = value             (bool / int / float / string)
//			"Native Component Argument": a parameter handed to that component instance.
//			(LocalID already identifies the component, so the component name is not repeated.)
//
//	Example (matches the editor screenshot):
//		NCI_0          = "MyClass1"
//		NCA_0_Args1    = "Hello"   (string)
//		NCA_0_Args2    = 123       (int)
//		NCA_0_Args3    = 0.542     (float)
//		NCA_0_Args4    = true      (bool)
//
//	Define a component:
//		struct MyClass1 : wi::scene::NativeComponent
//		{
//			float speed = 1.0f;
//			void Start()  override { Bind(speed, "Args3"); }
//			void Update(float dt) override
//			{
//				TransformComponent* tr = GetComponent<TransformComponent>(); // engine component on same entity
//				if (tr) tr->Translate(XMFLOAT3(0, speed * dt, 0));
//			}
//		};
//		ST_REGISTER_NATIVE_COMPONENT(MyClass1) // put this in a .cpp file

#include "wiECS.h"
#include "wiVector.h"
#include "wiUnorderedMap.h"
#include "wiJobSystem.h"

#include <string>
#include <memory>
#include <functional>
#include <type_traits>

namespace wi::scene
{
	struct Scene; // forward declaration; full definition lives in wiScene.h

	// Lightweight compile-time type identity. RTTI is disabled engine-wide (/GR-, -fno-rtti)
	// so typeid/dynamic_cast are unavailable; this gives each type a unique stable address instead.
	using NativeTypeID = const void*;
	template<typename T>
	inline NativeTypeID GetNativeTypeID()
	{
		static const char id = 0;
		return &id;
	}

	// Base class for a native component. Derive from this and override the lifecycle methods.
	//	The engine fills in scene/entity/localID/componentName before Start() is called.
	struct NativeComponent
	{
		// Runtime context (set by the engine, valid from Start() onwards):
		Scene* scene = nullptr;                            // owning scene
		wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;  // owning entity ("GameObject")
		int localID = 0;                                   // the <LocalID> from NCI_<LocalID>
		std::string componentName;                         // registered name (NCI_ value)

		NativeComponent() = default;
		virtual ~NativeComponent() = default;

		// Lifecycle (override in your subclass). Call order on an entity, per instance:
		//	Awake -> OnEnable -> Start -> (FixedUpdate* , Update) every frame -> OnDisable -> Destroy
		//
		//	Awake       : once, the first frame the instance exists, BEFORE OnEnable/Start, and
		//	              regardless of the enabled flag (use for self-setup independent of others).
		//	OnEnable    : each time the instance transitions disabled -> enabled (incl. the first
		//	              time it is seen while enabled). Fires right after Awake on first enable.
		//	Start       : once, before the first Update, only while enabled. A disabled instance
		//	              defers Start until it is first enabled.
		//	FixedUpdate : zero or more times per frame on a fixed timestep (see FIXED_DT), only
		//	              while enabled. Use for physics-style stepping independent of frame rate.
		//	Update      : every frame while enabled and dt > 0.
		//	OnDisable   : each time the instance transitions enabled -> disabled (and once before
		//	              Destroy if it was still enabled).
		//	Destroy     : once when removed / entity removed / scene cleared.
		virtual void Awake() {}
		virtual void OnEnable() {}
		virtual void Start() {}
		virtual void FixedUpdate(float fixedDt) {}
		virtual void Update(float dt) {}
		virtual void OnDisable() {}
		virtual void Destroy() {}

		// Enabled flag, backed by metadata NCE_<localID> (bool, default true when the key is
		//	absent). Reading is the source of truth each frame; toggling fires OnEnable/OnDisable
		//	on the next Update and gates Start/FixedUpdate/Update. SetEnabled writes the metadata
		//	so the change persists (editable in the Wicked Editor and saved with the scene).
		bool IsEnabled() const;
		void SetEnabled(bool value);

		// Debug/inspector UI (override in your subclass): draw ImGui widgets bound to this
		//	instance's live members. Called by the scene debug UI (see imnativecomponents.h)
		//	while an ImGui window is already active — do NOT Begin()/End() here. Editing a member
		//	takes effect on the next Update(); it is NOT written back to metadata (not persisted).
		//	The base does nothing so components are debug-able only if they opt in.
		virtual void DrawDebug() {}

		// Parameter access. Reads NCA_<localID>_<name> from the entity's MetadataComponent.
		bool        GetBool(const std::string& name, bool def = false) const;
		int         GetInt(const std::string& name, int def = 0) const;
		float       GetFloat(const std::string& name, float def = 0.0f) const;
		std::string GetString(const std::string& name, const std::string& def = "") const;
		bool        HasParam(const std::string& name) const;

		// Entity-reference parameter ("drag an object into a field", Unity-style).
		//	Stored as a STRING arg NCA_<localID>_<name> whose value is the target entity's
		//	stable GUID (see EnsureEntityGUID). String storage keeps it editable in the Wicked
		//	Editor; the GUID (not the name) makes it survive renames and duplicate names.
		//
		//	GetEntityRef : resolve the stored GUID to a live Entity (INVALID_ENTITY if unset /
		//	               the target is gone). Cheap-ish (scans metadata) — cache the result.
		//	SetEntityRef : point the field at 'target'; ensures the target has a GUID and writes
		//	               the GUID into this instance's metadata arg (persisted, editor-visible).
		//	               Pass INVALID_ENTITY to clear the field.
		wi::ecs::Entity GetEntityRef(const std::string& name) const;
		void            SetEntityRef(const std::string& name, wi::ecs::Entity target);

		// One-line binding of a member field to a parameter (type deduced from the field):
		//	Bind(speed, "Args3");  // same as: speed = GetFloat("Args3", speed);
		template<typename T>
		void Bind(T& field, const std::string& name)
		{
			if constexpr (std::is_same_v<T, bool>)                          field = GetBool(name, field);
			else if constexpr (std::is_same_v<T, std::string>)              field = GetString(name, field);
			else if constexpr (std::is_floating_point_v<T>)                 field = (T)GetFloat(name, (float)field);
			else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)  field = (T)GetInt(name, (int)field);
			else static_assert(!sizeof(T*), "Bind: unsupported field type (use bool/int/float/std::string).");
		}

		// GetComponent<T>() — Unity-style lookup on the SAME entity:
		//	- if T is an engine component (TransformComponent, MeshComponent, ...) returns it (or nullptr)
		//	- if T derives from NativeComponent returns the first native instance of that type (or nullptr)
		template<typename T> T* GetComponent();
		// Native-only: get the instance attached with a specific LocalID (for stacked components).
		template<typename T> T* GetComponentByID(int id);
		// Native-only: append every native instance of type T on this entity to 'out'.
		template<typename T> void GetComponents(wi::vector<T*>& out);

		// non-copyable (owns no copyable state by contract; instances are heap-owned by the manager)
		NativeComponent(const NativeComponent&) = delete;
		NativeComponent& operator=(const NativeComponent&) = delete;
	};

	// Factory + type identity stored per registered component name.
	using NativeComponentFactory = std::function<std::unique_ptr<NativeComponent>()>;
	struct NativeComponentRegistration
	{
		NativeComponentFactory factory;
		NativeTypeID typeID = nullptr;
	};

	// Register a component type so it can be attached from metadata.
	//	Prefer the ST_REGISTER_NATIVE_COMPONENT macro below.
	void RegisterNativeComponent(const std::string& name, NativeComponentFactory factory, NativeTypeID typeID);
	// Look up a registration by the metadata name (returns nullptr if not registered).
	const NativeComponentRegistration* FindNativeComponentRegistration(const std::string& name);

	// ------------------------------------------------------------------
	// Stable entity references (GUID-based).
	//	Engine entity IDs are session-local and names are mutable / non-unique, so neither is a
	//	safe persistent reference. These store a stable per-entity GUID in the entity's own
	//	MetadataComponent under the string key "EntityGUID" (editor-visible, saved with the scene).
	//
	//	EnsureEntityGUID : return the entity's GUID, creating (and storing) one if absent. Creates
	//	                   a MetadataComponent on the entity if it has none. GUIDs are unique within
	//	                   the scene (allocated as max-existing + 1). Returns "" only if entity is
	//	                   INVALID_ENTITY.
	//	FindEntityByGUID : reverse lookup, INVALID_ENTITY if no entity carries that GUID.
	//
	//	NOTE: Scene::Entity_Duplicate copies the GUID onto the clone -> a collision. Re-stamp the
	//	clone after duplicating if you need both addressable (see RegenerateEntityGUID).
	std::string     EnsureEntityGUID(Scene& scene, wi::ecs::Entity e);
	wi::ecs::Entity FindEntityByGUID(Scene& scene, const std::string& guid);
	// Force-assign a fresh unique GUID (use on a duplicated entity to break the inherited collision).
	std::string     RegenerateEntityGUID(Scene& scene, wi::ecs::Entity e);

	// Per-scene runtime state for native components.
	//	This is NOT serialized: attachments are rebuilt from the (serialized) MetadataComponent every Update.
	struct NativeComponentManager
	{
		struct Instance
		{
			std::unique_ptr<NativeComponent> component;
			NativeTypeID typeID = nullptr;
			int localID = 0;
			std::string name;
			bool awoken = false;   // Awake() fired
			bool started = false;  // Start() fired
			bool enabled = false;  // last-applied enabled state (drives OnEnable/OnDisable edges)
		};
		wi::unordered_map<wi::ecs::Entity, wi::vector<Instance>> instances; // entity -> attached instances

		// Fixed-timestep state for FixedUpdate (shared across all instances, like Unity).
		static constexpr float FIXED_DT = 1.0f / 60.0f;   // 60 Hz fixed step
		static constexpr int   MAX_FIXED_STEPS = 8;       // clamp to avoid the spiral of death
		float fixedAccumulator = 0.0f;

		// Reconcile attachments against metadata, fire Start() on new ones and Update(dt) on all.
		//	Called by Scene::RunNativeComponentUpdateSystem once per frame.
		void RunUpdate(Scene& scene, float dt);
		// Destroy + drop every instance on one entity (called from Scene::Entity_Remove).
		void RemoveEntity(wi::ecs::Entity entity);
		// Destroy + drop everything (called from Scene::Clear).
		void Clear();

		// Lookups used by NativeComponent::GetComponent / GetComponentByID / GetComponents:
		NativeComponent* Get(wi::ecs::Entity entity, NativeTypeID typeID) const;
		NativeComponent* GetByID(wi::ecs::Entity entity, NativeTypeID typeID, int localID) const;
		void GetAll(wi::ecs::Entity entity, NativeTypeID typeID, wi::vector<NativeComponent*>& out) const;
	};
}

// Register a native component under its own type name (the string used in NCI_<id>).
//	Place in a .cpp file:  ST_REGISTER_NATIVE_COMPONENT(MyClass1)
#define ST_REGISTER_NATIVE_COMPONENT(TYPE) \
	namespace { struct TYPE##_NativeReg { TYPE##_NativeReg() { \
		::wi::scene::RegisterNativeComponent(#TYPE, []() { \
			return std::unique_ptr<::wi::scene::NativeComponent>(new TYPE()); }, \
			::wi::scene::GetNativeTypeID<TYPE>()); \
	} }; static TYPE##_NativeReg _global_##TYPE##_NativeReg_instance; }

// Register a native component under a custom name (when the NCI_ string differs from the C++ type name).
//	Place in a .cpp file:  ST_REGISTER_NATIVE_COMPONENT_AS(MyClass1, "Spinner")
#define ST_REGISTER_NATIVE_COMPONENT_AS(TYPE, NAME) \
	namespace { struct TYPE##_NativeRegAs { TYPE##_NativeRegAs() { \
		::wi::scene::RegisterNativeComponent(NAME, []() { \
			return std::unique_ptr<::wi::scene::NativeComponent>(new TYPE()); }, \
			::wi::scene::GetNativeTypeID<TYPE>()); \
	} }; static TYPE##_NativeRegAs _global_##TYPE##_NativeRegAs_instance; }
