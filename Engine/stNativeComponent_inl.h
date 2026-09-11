#pragma once
// Out-of-line template definitions for NativeComponent.
//	Included at the very end of wiScene.h, where struct Scene and all engine component
//	types are complete (these templates need to touch Scene's component managers).

#include "wiBacklog.h" // Require<T>() warns here when a native type is not registered

namespace wi::scene
{
	// Maps an engine component type T to its ComponentManager on the Scene. ONE chain, used by
	//	both the read (GetEngineComponentPtr) and the get-or-create (EnsureEngineComponentPtr) -
	//	a second copy of this list is a second place to forget a type. Add a line here to expose
	//	more engine types.
	template<typename T>
	inline wi::ecs::ComponentManager<T>* GetEngineComponentManager(Scene* s)
	{
		if (s == nullptr)
			return nullptr;
		if constexpr (std::is_same_v<T, NameComponent>)                    return &s->names;
		else if constexpr (std::is_same_v<T, LayerComponent>)              return &s->layers;
		else if constexpr (std::is_same_v<T, TransformComponent>)          return &s->transforms;
		else if constexpr (std::is_same_v<T, HierarchyComponent>)          return &s->hierarchy;
		else if constexpr (std::is_same_v<T, MaterialComponent>)           return &s->materials;
		else if constexpr (std::is_same_v<T, MeshComponent>)               return &s->meshes;
		else if constexpr (std::is_same_v<T, ObjectComponent>)             return &s->objects;
		else if constexpr (std::is_same_v<T, RigidBodyPhysicsComponent>)   return &s->rigidbodies;
		else if constexpr (std::is_same_v<T, SoftBodyPhysicsComponent>)    return &s->softbodies;
		else if constexpr (std::is_same_v<T, ArmatureComponent>)           return &s->armatures;
		else if constexpr (std::is_same_v<T, LightComponent>)              return &s->lights;
		else if constexpr (std::is_same_v<T, CameraComponent>)             return &s->cameras;
		else if constexpr (std::is_same_v<T, EnvironmentProbeComponent>)   return &s->probes;
		else if constexpr (std::is_same_v<T, ForceFieldComponent>)         return &s->forces;
		else if constexpr (std::is_same_v<T, DecalComponent>)              return &s->decals;
		else if constexpr (std::is_same_v<T, AnimationComponent>)          return &s->animations;
		else if constexpr (std::is_same_v<T, SoundComponent>)              return &s->sounds;
		else if constexpr (std::is_same_v<T, VideoComponent>)              return &s->videos;
		else if constexpr (std::is_same_v<T, InverseKinematicsComponent>)  return &s->inverse_kinematics;
		else if constexpr (std::is_same_v<T, SpringComponent>)             return &s->springs;
		else if constexpr (std::is_same_v<T, ColliderComponent>)           return &s->colliders;
		else if constexpr (std::is_same_v<T, ScriptComponent>)             return &s->scripts;
		else if constexpr (std::is_same_v<T, ExpressionComponent>)         return &s->expressions;
		else if constexpr (std::is_same_v<T, HumanoidComponent>)           return &s->humanoids;
		else if constexpr (std::is_same_v<T, MetadataComponent>)           return &s->metadatas;
		else if constexpr (std::is_same_v<T, CharacterComponent>)          return &s->characters;
		else
		{
			static_assert(!sizeof(T*),
				"GetComponent<T>: T is neither a mapped engine component nor a NativeComponent subclass. "
				"Add a mapping line in GetEngineComponentManager (stNativeComponent_inl.h).");
			return nullptr;
		}
	}

	// The component attached to 'e', or nullptr. Never creates.
	template<typename T>
	inline T* GetEngineComponentPtr(Scene* s, wi::ecs::Entity e)
	{
		wi::ecs::ComponentManager<T>* m = GetEngineComponentManager<T>(s);
		return (m == nullptr) ? nullptr : m->GetComponent(e);
	}

	// The component attached to 'e', creating it if absent. Main thread only: Create() can
	//	reallocate the manager's backing arrays, which the parallel stages are reading.
	template<typename T>
	inline T* EnsureEngineComponentPtr(Scene* s, wi::ecs::Entity e)
	{
		wi::ecs::ComponentManager<T>* m = GetEngineComponentManager<T>(s);
		if (m == nullptr || e == wi::ecs::INVALID_ENTITY)
			return nullptr;
		T* existing = m->GetComponent(e);
		return (existing != nullptr) ? existing : &m->Create(e);
	}

	// RequiredComponent::For<T>() - pick the native or the engine half from T itself. The
	//	engine branch's lambda captures nothing, so it decays to the plain function pointer
	//	RequiredComponent stores.
	template<typename T>
	inline RequiredComponent RequiredComponent::For()
	{
		RequiredComponent r;
		if constexpr (std::is_base_of_v<NativeComponent, T>)
		{
			r.nativeType = GetNativeTypeID<T>();
		}
		else
		{
			r.ensureEngine = [](Scene& s, wi::ecs::Entity e) { EnsureEngineComponentPtr<T>(&s, e); };
		}
		return r;
	}

	// What ST_REQUIRE_COMPONENTS expands to. Order is preserved, which is the order the
	//	manager creates them in.
	template<typename... Ts>
	inline void AppendRequired(wi::vector<RequiredComponent>& out)
	{
		(out.push_back(RequiredComponent::For<Ts>()), ...);
	}

	template<typename T>
	inline T* NativeComponent::GetComponent()
	{
		if constexpr (std::is_base_of_v<NativeComponent, T>)
		{
			if (scene == nullptr)
				return nullptr;
			return static_cast<T*>(scene->nativeComponents.Get(entity, GetNativeTypeID<T>()));
		}
		else
		{
			return GetEngineComponentPtr<T>(scene, entity);
		}
	}

	template<typename T>
	inline T* NativeComponent::Require()
	{
		if constexpr (std::is_base_of_v<NativeComponent, T>)
		{
			if (scene == nullptr)
				return nullptr;
			// Already there: hand it back, nothing to attach.
			if (T* existing = static_cast<T*>(scene->nativeComponents.Get(entity, GetNativeTypeID<T>())))
				return existing;

			// Not there. Attaching is a metadata write, so the instance does not exist until the
			//	next reconcile - there is nothing honest to return this frame. (DescribeRequired
			//	does not have this problem: the manager resolves it BEFORE building the lifecycle,
			//	so it can instantiate in the same pass.)
			const std::string* name = FindNativeComponentNameForType(GetNativeTypeID<T>());
			if (name == nullptr)
			{
				wi::backlog::post("Require<T>: that native component type is not registered.",
					wi::backlog::LogLevel::Warning);
				return nullptr;
			}
			AttachNativeComponent(*scene, entity, *name);
			return nullptr;
		}
		else
		{
			return EnsureEngineComponentPtr<T>(scene, entity);
		}
	}

	template<typename T>
	inline T* NativeComponent::GetComponentByID(int id)
	{
		static_assert(std::is_base_of_v<NativeComponent, T>,
			"GetComponentByID is only valid for native components (T must derive from NativeComponent).");
		if (scene == nullptr)
			return nullptr;
		return static_cast<T*>(scene->nativeComponents.GetByID(entity, GetNativeTypeID<T>(), id));
	}

	template<typename T>
	inline void NativeComponent::GetComponents(wi::vector<T*>& out)
	{
		static_assert(std::is_base_of_v<NativeComponent, T>,
			"GetComponents is only valid for native components (T must derive from NativeComponent).");
		if (scene == nullptr)
			return;
		wi::vector<NativeComponent*> tmp;
		scene->nativeComponents.GetAll(entity, GetNativeTypeID<T>(), tmp);
		out.reserve(out.size() + tmp.size());
		for (NativeComponent* p : tmp)
			out.push_back(static_cast<T*>(p));
	}
}
