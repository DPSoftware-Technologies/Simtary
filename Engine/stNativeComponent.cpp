#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "wiProfiler.h"

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <utility>

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

	void GetRegisteredNativeComponentNames(wi::vector<std::string>& out)
	{
		auto& registry = GetRegistry();
		out.clear();
		out.reserve(registry.size());
		for (auto& kv : registry)
			out.push_back(kv.first);
		std::sort(out.begin(), out.end());
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
		// Writing metadata is a structural scene write. Called from a worker thread (the
		// default for Compute/FixedUpdate/Update) it has to wait for the main thread, so the
		// call defers itself rather than making every caller think about it.
		if (scene->nativeComponents.phaseRunning && !scene->nativeComponents.IsMainThread())
		{
			RunOnMainThread([this, name, target] { SetEntityRef(name, target); });
			return;
		}
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

	// ------------------------------------------------------------------
	// Main-thread hand-off
	// ------------------------------------------------------------------
	void NativeComponent::RunOnMainThread(std::function<void()> fn)
	{
		if (!fn)
			return;
		// Outside an update there is no stage to defer to, and no parallel work to race with.
		if (scene == nullptr || !scene->nativeComponents.phaseRunning)
		{
			fn();
			return;
		}
		// Inside one, queue it even when the caller already is the main thread: a component
		// that opted out of multithreading then behaves exactly like a parallel one, which is
		// what makes ST_NATIVE_COMPONENT_MAIN_THREAD() a drop-in switch either way.
		scene->nativeComponents.EnqueueMainThread(executionIndex, std::move(fn));
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
		// See SetEntityRef: metadata writes belong on the main thread.
		if (scene->nativeComponents.phaseRunning && !scene->nativeComponents.IsMainThread())
		{
			RunOnMainThread([this, value] { SetEnabled(value); });
			return;
		}
		MetadataComponent* m = scene->metadatas.GetComponent(entity);
		if (m == nullptr)
			return;
		m->bool_values.set(EnableKey(this), value);
	}

	// ------------------------------------------------------------------
	// Editor-side attach / detach (metadata only; the manager reconciles next frame)
	// ------------------------------------------------------------------
	int AttachNativeComponent(Scene& scene, Entity entity, const std::string& name)
	{
		if (entity == INVALID_ENTITY || name.empty())
			return -1;
		if (FindNativeComponentRegistration(name) == nullptr)
		{
			wi::backlog::post("AttachNativeComponent: '" + name + "' is not registered.",
				wi::backlog::LogLevel::Warning);
			return -1;
		}

		MetadataComponent* m = scene.metadatas.GetComponent(entity);
		if (m == nullptr)
			m = &scene.metadatas.Create(entity); // attaching implies the entity carries metadata

		// Lowest free LocalID, so stacking N copies of a component gives 0,1,2... and a
		// detach in the middle is reused by the next attach.
		int localID = 0;
		while (m->string_values.has("NCI_" + std::to_string(localID)))
			++localID;

		m->string_values.set("NCI_" + std::to_string(localID), name);
		m->bool_values.set("NCE_" + std::to_string(localID), true); // explicit: visible in the editor
		return localID;
	}

	void DetachNativeComponent(Scene& scene, Entity entity, int localID)
	{
		MetadataComponent* m = scene.metadatas.GetComponent(entity);
		if (m == nullptr)
			return;

		const std::string idstr = std::to_string(localID);
		m->string_values.erase("NCI_" + idstr);
		m->bool_values.erase("NCE_" + idstr);

		// Every NCA_<localID>_* argument goes with it. Collect first: erase() reorders the
		// backing vectors, so mutating while iterating them would skip entries.
		const std::string argPrefix = "NCA_" + idstr + "_";
		auto sweep = [&argPrefix](auto& values) {
			wi::vector<std::string> doomed;
			for (const std::string& key : values.names)
			{
				if (key.rfind(argPrefix, 0) == 0)
					doomed.push_back(key);
			}
			for (const std::string& key : doomed)
				values.erase(key);
		};
		sweep(m->bool_values);
		sweep(m->int_values);
		sweep(m->float_values);
		sweep(m->string_values);
	}

	// ------------------------------------------------------------------
	// NativeComponentManager
	// ------------------------------------------------------------------
	void NativeComponentManager::RunUpdate(Scene& scene, float dt)
	{
		auto range = wi::profiler::BeginRangeCPU("Native Components");

		// Whoever drives Scene::Update is "the main thread" for this frame. Everything that
		// has to happen there (metadata writes, entity removal, RunOnMainThread callbacks) is
		// compared against this.
		mainThreadID = std::this_thread::get_id();

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

		// --- Pass C: drive the lifecycle (Awake -> enable edges -> Start) ---
		//	Still single-threaded: these fire once per instance, they are where components
		//	create entities and components, and their order is what a scene author reasons
		//	about. Only the per-frame stages below go wide.
		//
		//	From here on the instance containers must not move: the stages hand out raw
		//	Instance pointers, and user code may ask the scene to delete an entity mid-pass.
		//	'phaseRunning' turns those removals into a queue applied at the end (see
		//	RemoveEntity / Clear).
		phaseRunning = true;

		parallelList.clear();
		serialList.clear();

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
					continue; // disabled: skip Start / Compute / FixedUpdate / Update

				// Start: once, before the first Compute/Update, only while enabled.
				if (!inst.started)
				{
					c->Start();
					inst.started = true;
				}

				// Enabled and started -> it takes part in this frame's stages. Which list it
				// lands in is asked once per frame, so a component may change its mind (a
				// GetThreading() that depends on a parameter, say) between frames.
				if (multithreading && c->GetThreading() == NativeThreading::Parallel)
					parallelList.push_back(&inst);
				else
					serialList.push_back(&inst);
			}
		}
		FlushMainThreadQueue(); // anything Awake/OnEnable/Start queued

		// Execution order key: parallel instances first, then the main-thread ones. This is
		// what the deferred queue sorts by, so a frame replays identically no matter which
		// worker finished first.
		uint32_t order = 0;
		for (NativeComponentManager::Instance* inst : parallelList)
			inst->component->executionIndex = order++;
		for (NativeComponentManager::Instance* inst : serialList)
			inst->component->executionIndex = order++;

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

		// --- Pass D: the per-frame stages, one barrier each ---
		//	Compute for the whole scene, then every fixed step, then Update. Barriers are what
		//	make Compute worth having: by the time any Update runs, every Compute has finished,
		//	so reading another entity's computed result needs no ordering rule.
		RunStage(NativeStage::Compute, dt);

		for (int s = 0; s < fixedSteps; ++s)
			RunStage(NativeStage::FixedUpdate, FIXED_DT);

		if (dt > 0)
			RunStage(NativeStage::Update, dt);

		// --- Pass E: leave the pass, then apply what it asked for ---
		phaseRunning = false;
		parallelList.clear();   // the Instance pointers must not outlive the pass
		serialList.clear();
		FlushMainThreadQueue(); // a stage may have queued work after its own flush

		if (pendingClear)
		{
			pendingClear = false;
			pendingRemovals.clear();
			Clear();
		}
		else if (!pendingRemovals.empty())
		{
			wi::vector<Entity> removals;
			std::swap(removals, pendingRemovals);
			for (Entity e : removals)
				RemoveEntity(e);
		}

		wi::profiler::EndRange(range);
	}

	// One stage across the whole scene: the parallel instances dispatched over the job system,
	//	then the opted-out ones serially, then whatever either of them handed to the main thread.
	void NativeComponentManager::RunStage(NativeStage stage, float stageDt)
	{
		auto invoke = [stage, stageDt](NativeComponent* c)
		{
			switch (stage)
			{
			case NativeStage::Compute:     c->Compute(stageDt); break;
			case NativeStage::FixedUpdate: c->FixedUpdate(stageDt); break;
			case NativeStage::Update:      c->Update(stageDt); break;
			}
		};

		if (!parallelList.empty())
		{
			// One job per instance (group size 1): component cost is user code and wildly
			// uneven, so handing them out one at a time is what keeps a fat Update from
			// pinning a worker while the others idle. Below PARALLEL_MIN_COUNT, or on a
			// single-core machine, the dispatch costs more than it saves.
			if (parallelList.size() >= PARALLEL_MIN_COUNT && wi::jobsystem::GetThreadCount() > 1)
			{
				NativeComponentManager::Instance* const* list = parallelList.data();
				wi::jobsystem::context ctx;
				wi::jobsystem::Dispatch(ctx, (uint32_t)parallelList.size(), 1,
					[list, &invoke](wi::jobsystem::JobArgs args)
					{
						invoke(list[args.jobIndex]->component.get());
					});
				wi::jobsystem::Wait(ctx);
			}
			else
			{
				for (NativeComponentManager::Instance* inst : parallelList)
					invoke(inst->component.get());
			}
		}

		for (NativeComponentManager::Instance* inst : serialList)
			invoke(inst->component.get());

		FlushMainThreadQueue();
	}

	void NativeComponentManager::EnqueueMainThread(uint32_t order, std::function<void()> fn)
	{
		if (!fn)
			return;
		DeferredCall call;
		call.order = order;
		call.sequence = deferredSequence.fetch_add(1);
		call.fn = std::move(fn);

		deferredLock.lock();
		deferred.push_back(std::move(call));
		deferredLock.unlock();
	}

	void NativeComponentManager::FlushMainThreadQueue()
	{
		if (deferred.empty())
			return;

		// Instance order, then submission order within one instance: the threads finish in
		// whatever order the scheduler picked, the replay must not depend on it.
		std::stable_sort(deferred.begin(), deferred.end(),
			[](const DeferredCall& a, const DeferredCall& b)
			{
				if (a.order != b.order)
					return a.order < b.order;
				return a.sequence < b.sequence;
			});

		// A callback may queue more work (or remove an entity), so the batch is swapped out
		// before it runs - pushing into the vector being walked would invalidate it.
		wi::vector<DeferredCall> batch;
		std::swap(batch, deferred);
		for (DeferredCall& call : batch)
		{
			if (call.fn)
				call.fn();
		}
	}

	void NativeComponentManager::RemoveEntity(Entity entity)
	{
		if (phaseRunning)
		{
			// Mid-update: a component asked the scene to delete an entity (possibly its own).
			// Destroying the instances now would free memory the running stage still points
			// at, so the removal waits for the end of RunUpdate - same frame, no dangling.
			pendingRemovals.push_back(entity);
			return;
		}

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
		if (phaseRunning)
		{
			// Same reasoning as RemoveEntity: the instances outlive the pass that asked.
			pendingClear = true;
			return;
		}

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
