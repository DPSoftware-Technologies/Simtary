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
//
//	Threading:
//		Compute(), FixedUpdate() and Update() run MULTI-THREADED by default: the scene's
//		instances are spread over the job system's worker threads. The stages are barriered,
//		so every Compute() in the scene has finished before the first FixedUpdate() starts,
//		and each FixedUpdate() step finishes before the next one begins.
//		Awake/OnEnable/Start/OnDisable/Destroy/DrawDebug always run on the main thread.
//		From a parallel stage you may read the scene and write your own instance, but NOT
//		create/remove entities or components, write metadata, or call the renderer, physics
//		or the GPU - queue that with RunOnMainThread(), or opt the component out entirely:
//			struct MyClass1 : wi::scene::NativeComponent
//			{
//				ST_NATIVE_COMPONENT_MAIN_THREAD()   // Compute/FixedUpdate/Update go serial
//			};

#include "wiECS.h"
#include "wiVector.h"
#include "wiUnorderedMap.h"
#include "wiJobSystem.h"
#include "wiSpinLock.h"

#include <string>
#include <memory>
#include <functional>
#include <type_traits>
#include <atomic>
#include <thread>

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

	// Where one component's per-frame callbacks (Compute / FixedUpdate / Update) are run.
	enum class NativeThreading
	{
		Parallel,   // default: on the job system's worker threads, many instances at once
		MainThread, // opt out: serially on the thread that drives Scene::Update
	};

	// The three per-frame stages, in the order they run. Each is a barrier: the whole scene
	//  finishes one stage before any instance starts the next.
	enum class NativeStage
	{
		Compute,     // heavy math / lookups, results read by Update
		FixedUpdate, // fixed 60 Hz step, runs 0..MAX_FIXED_STEPS times per frame
		Update,      // the frame's work
	};

	// Base class for a native component. Derive from this and override the lifecycle methods.
	//	The engine fills in scene/entity/localID/componentName before Start() is called.
	struct NativeComponent
	{
		// Runtime context (set by the engine, valid from Start() onwards):
		Scene* scene = nullptr;                            // owning scene
		wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;  // owning entity ("GameObject")
		int localID = 0;                                   // the <LocalID> from NCI_<LocalID>
		std::string componentName;                         // registered name (NCI_ value)
		uint32_t executionIndex = 0;                       // stable order key for this frame (engine-set)

		NativeComponent() = default;
		virtual ~NativeComponent() = default;

		// Lifecycle (override in your subclass). Call order on an entity, per instance:
		//	Awake -> OnEnable -> Start -> (Compute, FixedUpdate*, Update) every frame -> OnDisable -> Destroy
		//
		//	The [main] / [worker] tag is the thread the callback runs on (see GetThreading below).
		//
		//	Awake       : [main]   once, the first frame the instance exists, BEFORE OnEnable/Start,
		//	              and regardless of the enabled flag (self-setup independent of others).
		//	OnEnable    : [main]   each time the instance transitions disabled -> enabled (incl. the
		//	              first time it is seen while enabled). Fires right after Awake on first enable.
		//	Start       : [main]   once, before the first Compute/Update, only while enabled. A
		//	              disabled instance defers Start until it is first enabled. This is where
		//	              creating entities/components belongs - it is always on the main thread.
		//	Compute     : [worker] once per frame while enabled, BEFORE any instance's FixedUpdate or
		//	              Update. The parallel stage: do the heavy math here, park the result in a
		//	              member, and let Update apply it. Runs on dt == 0 frames too (loading), so
		//	              check dt if that matters.
		//	FixedUpdate : [worker] zero or more times per frame on a fixed timestep (see FIXED_DT),
		//	              only while enabled. Physics-style stepping, independent of frame rate.
		//	Update      : [worker] every frame while enabled and dt > 0.
		//	OnDisable   : [main]   each time the instance transitions enabled -> disabled (and once
		//	              before Destroy if it was still enabled).
		//	Destroy     : [main]   once when removed / entity removed / scene cleared.
		virtual void Awake() {}
		virtual void OnEnable() {}
		virtual void Start() {}
		virtual void Compute(float dt) {}
		virtual void FixedUpdate(float fixedDt) {}
		virtual void Update(float dt) {}
		virtual void OnDisable() {}
		virtual void Destroy() {}

		// ------------------------------------------------------------------
		// Threading.
		//	Compute/FixedUpdate/Update run on job system worker threads by default, so a scene
		//	full of components uses every core instead of one. The contract inside those three:
		//
		//		ALLOWED   : read and write this instance's own members; read the scene
		//		            (GetComponent<T>(), transforms, Scene::Intersects, GetFloat/GetString...);
		//		            write a component no other instance writes this frame.
		//		FORBIDDEN : creating or removing entities/components (scene->xxx.Create/Remove,
		//		            Entity_Create/Entity_Remove/Entity_Duplicate, Attach/DetachNativeComponent),
		//		            writing metadata, wi::renderer::DrawLine/DrawSphere/... (unlocked global
		//		            list), wi::physics::* mutations, GPU work (GetDevice()->...), ImGui, and
		//		            writing anything another instance may also write.
		//
		//	Two ways out, choose per component:
		//		1) RunOnMainThread([this]{ ... }) - queue just the unsafe part; it runs on the main
		//		   thread at the end of the current stage, same frame, in a deterministic order.
		//		2) ST_NATIVE_COMPONENT_MAIN_THREAD() in the component body - this component's three
		//		   per-frame callbacks go back to the serial main-thread pass. No other change
		//		   needed: identical semantics to the single-threaded engine.
		//
		//	SetEnabled() and SetEntityRef() write metadata, so they defer themselves automatically
		//	when called from a worker thread - they are safe to call from anywhere.
		virtual NativeThreading GetThreading() const { return NativeThreading::Parallel; }

		// Queue work to run on the main thread at the end of the current stage (Compute, one
		//	FixedUpdate step, or Update). Callable from any thread. Outside an update it simply
		//	runs the callback right away. Callbacks are replayed in instance order, then in
		//	submission order per instance, so the frame is reproducible whatever the scheduler did.
		void RunOnMainThread(std::function<void()> fn);

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

	// Every component name currently registered through ST_REGISTER_NATIVE_COMPONENT(_AS),
	//	sorted alphabetically. This is what an editor's "Add Component" list is built from:
	//	the registry is populated by static initializers, so by the time any frame runs it
	//	holds every native component the engine AND the game linked in.
	void GetRegisteredNativeComponentNames(wi::vector<std::string>& out);

	// ------------------------------------------------------------------
	// Editor-side attach / detach.
	//	Both work purely on the entity's MetadataComponent — the same NCI_/NCA_/NCE_ keys the
	//	system already reconciles in NativeComponentManager::RunUpdate — so a change takes
	//	effect on the next frame, needs no engine restart, and is saved with the scene.
	//
	//	AttachNativeComponent : writes NCI_<LocalID> = name using the lowest free LocalID on
	//	                        the entity (creating a MetadataComponent if it has none).
	//	                        Returns the LocalID, or -1 if `name` is not registered.
	//	DetachNativeComponent : erases NCI_<LocalID>, NCE_<LocalID> and every
	//	                        NCA_<LocalID>_* argument. The instance's OnDisable()/Destroy()
	//	                        run on the next RunUpdate, not inside this call.
	int  AttachNativeComponent(Scene& scene, wi::ecs::Entity entity, const std::string& name);
	void DetachNativeComponent(Scene& scene, wi::ecs::Entity entity, int localID);

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

		// ---- threading (runtime only, never serialized) ----
		// Global off switch for the parallel path: set false and every instance runs on the main
		//	thread in instance order, exactly like the pre-multicore engine. First thing to try
		//	when a bug smells like a race.
		static inline bool multithreading = true;
		// Under this many parallel-eligible instances the dispatch costs more than it saves.
		static constexpr uint32_t PARALLEL_MIN_COUNT = 8;

		struct DeferredCall
		{
			uint32_t order = 0;      // executionIndex of the instance that queued it
			uint32_t sequence = 0;   // submission counter, breaks ties within one instance
			std::function<void()> fn;
		};
		wi::SpinLock deferredLock;
		wi::vector<DeferredCall> deferred;
		std::atomic<uint32_t> deferredSequence{ 0 };
		std::thread::id mainThreadID;                 // whoever called RunUpdate this frame
		bool phaseRunning = false;                    // true while the lifecycle passes are running
		wi::vector<wi::ecs::Entity> pendingRemovals;  // Entity_Remove asked for mid-pass
		bool pendingClear = false;                    // Scene::Clear asked for mid-pass

		// This frame's execution lists, rebuilt every RunUpdate. Kept as members so the
		//	allocation is paid once instead of per frame.
		wi::vector<Instance*> parallelList;
		wi::vector<Instance*> serialList;

		bool IsMainThread() const { return std::this_thread::get_id() == mainThreadID; }
		void EnqueueMainThread(uint32_t order, std::function<void()> fn);
		void FlushMainThreadQueue();
		// Run one stage over the whole scene: parallel list dispatched, main-thread list serial,
		//	then the deferred queue flushed. Returns when every instance has finished the stage.
		void RunStage(NativeStage stage, float stageDt);

		// Reconcile attachments against metadata, fire Start() on new ones and Update(dt) on all.
		//	Called by Scene::RunNativeComponentUpdateSystem once per frame.
		void RunUpdate(Scene& scene, float dt);
		// Destroy + drop every instance on one entity (called from Scene::Entity_Remove).
		//	Called while an update is in flight (a component removed an entity from its own
		//	Update) it queues instead: destroying now would free an Instance the running pass
		//	still points at. The queue is applied at the end of RunUpdate, same frame.
		void RemoveEntity(wi::ecs::Entity entity);
		// Destroy + drop everything (called from Scene::Clear). Also queued if an update is in
		//	flight - the instances live until the end of that RunUpdate.
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

// Opt a component out of multithreading: its Compute/FixedUpdate/Update run serially on the
//	main thread, so they may touch the scene, the renderer, physics and the GPU freely.
//	Place in the component's body:  struct MyClass1 : ... { ST_NATIVE_COMPONENT_MAIN_THREAD() ... };
#define ST_NATIVE_COMPONENT_MAIN_THREAD() \
	::wi::scene::NativeThreading GetThreading() const override { return ::wi::scene::NativeThreading::MainThread; }

// Register a native component under a custom name (when the NCI_ string differs from the C++ type name).
//	Place in a .cpp file:  ST_REGISTER_NATIVE_COMPONENT_AS(MyClass1, "Spinner")
#define ST_REGISTER_NATIVE_COMPONENT_AS(TYPE, NAME) \
	namespace { struct TYPE##_NativeRegAs { TYPE##_NativeRegAs() { \
		::wi::scene::RegisterNativeComponent(NAME, []() { \
			return std::unique_ptr<::wi::scene::NativeComponent>(new TYPE()); }, \
			::wi::scene::GetNativeTypeID<TYPE>()); \
	} }; static TYPE##_NativeRegAs _global_##TYPE##_NativeRegAs_instance; }
