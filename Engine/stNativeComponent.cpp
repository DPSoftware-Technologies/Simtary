#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "wiProfiler.h"

#include <cstdlib>
#include <cstdio>

using namespace wi::ecs;

namespace wi::scene
{
	// ------------------------------------------------------------------
	// Global registry (component name -> factory)
	// ------------------------------------------------------------------
	static wi::unordered_map<std::string, NativeComponentRegistration>& GetRegistry()
	{
		// Function-local static so it is initialized before any ST_REGISTER_NATIVE_COMPONENT
		// static initializer that runs in another translation unit.
		static wi::unordered_map<std::string, NativeComponentRegistration> registry;
		return registry;
	}

	void RegisterNativeComponent(const std::string& name, NativeComponentFactory factory, NativeTypeID typeID)
	{
		NativeComponentRegistration& reg = GetRegistry()[name];
		reg.factory = std::move(factory);
		reg.typeID = typeID;
	}

	const NativeComponentRegistration* FindNativeComponentRegistration(const std::string& name)
	{
		auto& registry = GetRegistry();
		auto it = registry.find(name);
		if (it == registry.end())
			return nullptr;
		return &it->second;
	}

	// ------------------------------------------------------------------
	// Parameter access (reads NCA_<localID>_<name> from metadata)
	// ------------------------------------------------------------------
	static const MetadataComponent* GetMetadata(const NativeComponent* self)
	{
		if (self->scene == nullptr)
			return nullptr;
		return self->scene->metadatas.GetComponent(self->entity);
	}
	static std::string ArgKey(const NativeComponent* self, const std::string& name)
	{
		return "NCA_" + std::to_string(self->localID) + "_" + name;
	}

	bool NativeComponent::GetBool(const std::string& name, bool def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->bool_values.has(key) ? m->bool_values.get(key) : def;
	}
	int NativeComponent::GetInt(const std::string& name, int def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->int_values.has(key) ? m->int_values.get(key) : def;
	}
	float NativeComponent::GetFloat(const std::string& name, float def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->float_values.has(key) ? m->float_values.get(key) : def;
	}
	std::string NativeComponent::GetString(const std::string& name, const std::string& def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->string_values.has(key) ? m->string_values.get(key) : def;
	}
	bool NativeComponent::HasParam(const std::string& name) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return false;
		const std::string key = ArgKey(this, name);
		return m->bool_values.has(key) || m->int_values.has(key) ||
			m->float_values.has(key) || m->string_values.has(key);
	}

	// ------------------------------------------------------------------
	// Stable entity references (GUID-based)
	// ------------------------------------------------------------------
	static const std::string kEntityGUIDKey = "EntityGUID";

	std::string EnsureEntityGUID(Scene& scene, Entity e)
	{
		if (e == INVALID_ENTITY)
			return std::string();

		MetadataComponent* m = scene.metadatas.GetComponent(e);
		if (m == nullptr)
			m = &scene.metadatas.Create(e); // referencing implies "addressable" -> give it metadata

		if (m->string_values.has(kEntityGUIDKey))
		{
			const std::string existing = m->string_values.get(kEntityGUIDKey);
			if (!existing.empty())
				return existing;
		}

		// Allocate a scene-unique id as (max existing GUID) + 1, stored as hex.
		uint64_t maxv = 0;
		for (size_t i = 0; i < scene.metadatas.GetCount(); ++i)
		{
			const MetadataComponent& md = scene.metadatas[i];
			if (md.string_values.has(kEntityGUIDKey))
			{
				const uint64_t v = std::strtoull(md.string_values.get(kEntityGUIDKey).c_str(), nullptr, 16);
				if (v > maxv)
					maxv = v;
			}
		}
		char buf[24];
		std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)(maxv + 1));
		const std::string guid = buf;
		m->string_values.set(kEntityGUIDKey, guid);
		return guid;
	}

	Entity FindEntityByGUID(Scene& scene, const std::string& guid)
	{
		if (guid.empty())
			return INVALID_ENTITY;
		for (size_t i = 0; i < scene.metadatas.GetCount(); ++i)
		{
			const MetadataComponent& md = scene.metadatas[i];
			if (md.string_values.has(kEntityGUIDKey) && md.string_values.get(kEntityGUIDKey) == guid)
				return scene.metadatas.GetEntity(i);
		}
		return INVALID_ENTITY;
	}

	std::string RegenerateEntityGUID(Scene& scene, Entity e)
	{
		if (e == INVALID_ENTITY)
			return std::string();
		MetadataComponent* m = scene.metadatas.GetComponent(e);
		if (m == nullptr)
			m = &scene.metadatas.Create(e);
		m->string_values.erase(kEntityGUIDKey); // drop inherited/duplicate id, then mint a fresh one
		return EnsureEntityGUID(scene, e);
	}

	Entity NativeComponent::GetEntityRef(const std::string& name) const
	{
		if (scene == nullptr)
			return INVALID_ENTITY;
		const std::string guid = GetString(name, "");
		if (guid.empty())
			return INVALID_ENTITY;
		return FindEntityByGUID(*scene, guid);
	}

	void NativeComponent::SetEntityRef(const std::string& name, Entity target)
	{
		if (scene == nullptr)
			return;
		MetadataComponent* m = scene->metadatas.GetComponent(entity);
		if (m == nullptr)
			m = &scene->metadatas.Create(entity);
		const std::string key = ArgKey(this, name);
		if (target == INVALID_ENTITY)
		{
			m->string_values.set(key, ""); // clear the field (keep the key so the editor still shows it)
			return;
		}
		const std::string guid = EnsureEntityGUID(*scene, target);
		m->string_values.set(key, guid);
	}

	// Enabled flag: metadata NCE_<localID> (bool), default true when absent.
	static std::string EnableKey(const NativeComponent* self)
	{
		return "NCE_" + std::to_string(self->localID);
	}
	bool NativeComponent::IsEnabled() const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return true; // no metadata -> enabled by default
		const std::string key = EnableKey(this);
		return m->bool_values.has(key) ? m->bool_values.get(key) : true;
	}
	void NativeComponent::SetEnabled(bool value)
	{
		if (scene == nullptr)
			return;
		MetadataComponent* m = scene->metadatas.GetComponent(entity);
		if (m == nullptr)
			return;
		m->bool_values.set(EnableKey(this), value);
	}

	// ------------------------------------------------------------------
	// NativeComponentManager
	// ------------------------------------------------------------------
	void NativeComponentManager::RunUpdate(Scene& scene, float dt)
	{
		auto range = wi::profiler::BeginRangeCPU("Native Components");

		// --- Pass A: prune instances that no longer match the metadata ---
		wi::vector<Entity> empty_entities;
		for (auto& pair : instances)
		{
			const Entity entity = pair.first;
			wi::vector<NativeComponentManager::Instance>& list = pair.second;
			const MetadataComponent* meta = scene.metadatas.GetComponent(entity);

			for (size_t i = list.size(); i-- > 0; )
			{
				NativeComponentManager::Instance& inst = list[i];
				bool keep = false;
				if (meta != nullptr)
				{
					const std::string key = "NCI_" + std::to_string(inst.localID);
					if (meta->string_values.has(key) && meta->string_values.get(key) == inst.name)
						keep = true;
				}
				if (!keep)
				{
					if (inst.component)
					{
						if (inst.enabled)
							inst.component->OnDisable();
						inst.component->Destroy();
					}
					list.erase(list.begin() + i);
				}
			}

			if (list.empty())
				empty_entities.push_back(entity);
		}
		for (Entity e : empty_entities)
			instances.erase(e);

		// --- Pass B: create instances for NCI_<id> keys that aren't attached yet ---
		for (size_t mi = 0; mi < scene.metadatas.GetCount(); ++mi)
		{
			const Entity entity = scene.metadatas.GetEntity(mi);
			const MetadataComponent& meta = scene.metadatas[mi];

			for (size_t k = 0; k < meta.string_values.names.size(); ++k)
			{
				const std::string& key = meta.string_values.names[k];
				if (key.rfind("NCI_", 0) != 0) // not an import key
					continue;

				const std::string idstr = key.substr(4);
				if (idstr.empty())
					continue;
				const int localID = std::atoi(idstr.c_str());
				const std::string& compName = meta.string_values.values[k];
				if (compName.empty())
					continue;

				// Skip if already attached with this localID:
				bool exists = false;
				auto it = instances.find(entity);
				if (it != instances.end())
				{
					for (const NativeComponentManager::Instance& inst : it->second)
					{
						if (inst.localID == localID)
						{
							exists = true;
							break;
						}
					}
				}
				if (exists)
					continue;

				const NativeComponentRegistration* reg = FindNativeComponentRegistration(compName);
				if (reg == nullptr || !reg->factory)
				{
					wi::backlog::post(
						"NativeComponent: '" + compName + "' (NCI_" + idstr + ") is not registered. "
						"Did you add ST_REGISTER_NATIVE_COMPONENT(" + compName + ")?",
						wi::backlog::LogLevel::Warning);
					continue;
				}

				NativeComponentManager::Instance inst;
				inst.component = reg->factory();
				if (!inst.component)
					continue;
				inst.typeID = reg->typeID;
				inst.localID = localID;
				inst.name = compName;
				inst.started = false;
				inst.component->scene = &scene;
				inst.component->entity = entity;
				inst.component->localID = localID;
				inst.component->componentName = compName;
				instances[entity].push_back(std::move(inst));
			}
		}

		// --- Pass C: drive the lifecycle (Awake -> enable edges -> Start -> FixedUpdate -> Update) ---
		//	Single-threaded on purpose: user code may freely touch the scene.

		// Advance the shared fixed-step accumulator once per frame, clamped to avoid the
		// "spiral of death" when a frame hitch dumps a large dt.
		int fixedSteps = 0;
		if (dt > 0)
		{
			fixedAccumulator += dt;
			while (fixedAccumulator >= FIXED_DT && fixedSteps < MAX_FIXED_STEPS)
			{
				fixedAccumulator -= FIXED_DT;
				++fixedSteps;
			}
			if (fixedAccumulator > FIXED_DT * MAX_FIXED_STEPS)
				fixedAccumulator = 0.0f; // drop backlog beyond the clamp
		}

		for (auto& pair : instances)
		{
			wi::vector<NativeComponentManager::Instance>& list = pair.second;
			for (NativeComponentManager::Instance& inst : list)
			{
				if (!inst.component)
					continue;
				NativeComponent* c = inst.component.get();

				// Awake: once, before anything else, regardless of enabled state.
				if (!inst.awoken)
				{
					c->Awake();
					inst.awoken = true;
				}

				// Enable/disable edges from metadata (NCE_<id>, default true).
				const bool desired = c->IsEnabled();
				if (desired && !inst.enabled)
				{
					inst.enabled = true;
					c->OnEnable();
				}
				else if (!desired && inst.enabled)
				{
					inst.enabled = false;
					c->OnDisable();
				}

				if (!inst.enabled)
					continue; // disabled: skip Start / FixedUpdate / Update

				// Start: once, before the first Update, only while enabled.
				if (!inst.started)
				{
					c->Start();
					inst.started = true;
				}

				for (int s = 0; s < fixedSteps; ++s)
					c->FixedUpdate(FIXED_DT);

				if (dt > 0)
					c->Update(dt);
			}
		}

		wi::profiler::EndRange(range);
	}

	void NativeComponentManager::RemoveEntity(Entity entity)
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return;
		for (NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.component)
			{
				if (inst.enabled)
					inst.component->OnDisable();
				inst.component->Destroy();
			}
		}
		instances.erase(it);
	}

	void NativeComponentManager::Clear()
	{
		for (auto& pair : instances)
		{
			for (NativeComponentManager::Instance& inst : pair.second)
			{
				if (inst.component)
				{
					if (inst.enabled)
						inst.component->OnDisable();
					inst.component->Destroy();
				}
			}
		}
		instances.clear();
	}

	NativeComponent* NativeComponentManager::Get(Entity entity, NativeTypeID typeID) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return nullptr;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.typeID == typeID)
				return inst.component.get();
		}
		return nullptr;
	}

	NativeComponent* NativeComponentManager::GetByID(Entity entity, NativeTypeID typeID, int localID) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return nullptr;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.localID == localID && inst.typeID == typeID)
				return inst.component.get();
		}
		return nullptr;
	}

	void NativeComponentManager::GetAll(Entity entity, NativeTypeID typeID, wi::vector<NativeComponent*>& out) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.typeID == typeID)
				out.push_back(inst.component.get());
		}
	}

	// ------------------------------------------------------------------
	// Scene hook
	// ------------------------------------------------------------------
	void Scene::RunNativeComponentUpdateSystem(wi::jobsystem::context& ctx)
	{
		nativeComponents.RunUpdate(*this, dt);
	}
}
