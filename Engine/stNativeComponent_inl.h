#pragma once
// Out-of-line template definitions for NativeComponent.
//	Included at the very end of wiScene.h, where struct Scene and all engine component
//	types are complete (these templates need to touch Scene's component managers).

namespace wi::scene
{
	// Maps an engine component type T to its ComponentManager on the Scene and returns the
	//	component attached to 'e' (or nullptr). Add a line here to expose more engine types.
	template<typename T>
	inline T* GetEngineComponentPtr(Scene* s, wi::ecs::Entity e)
	{
		if (s == nullptr)
			return nullptr;
		if constexpr (std::is_same_v<T, NameComponent>)                    return s->names.GetComponent(e);
		else if constexpr (std::is_same_v<T, LayerComponent>)              return s->layers.GetComponent(e);
		else if constexpr (std::is_same_v<T, TransformComponent>)          return s->transforms.GetComponent(e);
		else if constexpr (std::is_same_v<T, HierarchyComponent>)          return s->hierarchy.GetComponent(e);
		else if constexpr (std::is_same_v<T, MaterialComponent>)           return s->materials.GetComponent(e);
		else if constexpr (std::is_same_v<T, MeshComponent>)               return s->meshes.GetComponent(e);
		else if constexpr (std::is_same_v<T, ObjectComponent>)             return s->objects.GetComponent(e);
		else if constexpr (std::is_same_v<T, RigidBodyPhysicsComponent>)   return s->rigidbodies.GetComponent(e);
		else if constexpr (std::is_same_v<T, SoftBodyPhysicsComponent>)    return s->softbodies.GetComponent(e);
		else if constexpr (std::is_same_v<T, ArmatureComponent>)           return s->armatures.GetComponent(e);
		else if constexpr (std::is_same_v<T, LightComponent>)              return s->lights.GetComponent(e);
		else if constexpr (std::is_same_v<T, CameraComponent>)             return s->cameras.GetComponent(e);
		else if constexpr (std::is_same_v<T, EnvironmentProbeComponent>)   return s->probes.GetComponent(e);
		else if constexpr (std::is_same_v<T, ForceFieldComponent>)         return s->forces.GetComponent(e);
		else if constexpr (std::is_same_v<T, DecalComponent>)              return s->decals.GetComponent(e);
		else if constexpr (std::is_same_v<T, AnimationComponent>)          return s->animations.GetComponent(e);
		else if constexpr (std::is_same_v<T, SoundComponent>)              return s->sounds.GetComponent(e);
		else if constexpr (std::is_same_v<T, VideoComponent>)              return s->videos.GetComponent(e);
		else if constexpr (std::is_same_v<T, InverseKinematicsComponent>)  return s->inverse_kinematics.GetComponent(e);
		else if constexpr (std::is_same_v<T, SpringComponent>)             return s->springs.GetComponent(e);
		else if constexpr (std::is_same_v<T, ColliderComponent>)           return s->colliders.GetComponent(e);
		else if constexpr (std::is_same_v<T, ScriptComponent>)             return s->scripts.GetComponent(e);
		else if constexpr (std::is_same_v<T, ExpressionComponent>)         return s->expressions.GetComponent(e);
		else if constexpr (std::is_same_v<T, HumanoidComponent>)           return s->humanoids.GetComponent(e);
		else if constexpr (std::is_same_v<T, MetadataComponent>)           return s->metadatas.GetComponent(e);
		else if constexpr (std::is_same_v<T, CharacterComponent>)          return s->characters.GetComponent(e);
		else
		{
			static_assert(!sizeof(T*),
				"GetComponent<T>: T is neither a mapped engine component nor a NativeComponent subclass. "
				"Add a mapping line in GetEngineComponentPtr (stNativeComponent_inl.h).");
			return nullptr;
		}
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
