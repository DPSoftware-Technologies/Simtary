#include "stAudioSpatial.h"
#include "wiBacklog.h"

#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>

#ifdef SIMTARY_HAS_STEAMAUDIO
#include <phonon.h>
#endif

namespace st::audio
{
	namespace
	{
		inline float Dot(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) { return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z); }
		inline float Length(const XMFLOAT3& v) { return std::sqrt(Dot(v, v)); }
		inline XMFLOAT3 Normalize(const XMFLOAT3& v)
		{
			const float len = Length(v);
			return len > 1e-6f ? XMFLOAT3(v.x / len, v.y / len, v.z / len) : XMFLOAT3(0, 0, 1);
		}
		inline XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b)
		{
			return XMFLOAT3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
		}
	}

#ifdef SIMTARY_HAS_STEAMAUDIO

	// Steam Audio path
	namespace
	{
		inline IPLVector3 ToIPL(const XMFLOAT3& v) { return IPLVector3{ v.x, v.y, v.z }; }

		// Steam Audio wants a full orthonormal basis, not just forward+up. Building
		// right from forward x up (rather than trusting the caller's up to be exactly
		// perpendicular) keeps the basis orthonormal even when a transform has drifted.
		IPLCoordinateSpace3 ToIPLSpace(const SpatialTransform& t)
		{
			const XMFLOAT3 ahead = Normalize(t.forward);
			XMFLOAT3 right = Cross(ahead, Normalize(t.up));
			if (Length(right) < 1e-5f)
			{
				// forward is parallel to up (a straight-down camera): pick any
				// perpendicular rather than emitting a degenerate basis.
				right = Cross(ahead, XMFLOAT3(0, 0, 1));
				if (Length(right) < 1e-5f) right = XMFLOAT3(1, 0, 0);
			}
			right = Normalize(right);
			const XMFLOAT3 up = Normalize(Cross(right, ahead));

			IPLCoordinateSpace3 space{};
			space.right = ToIPL(right);
			space.up = ToIPL(up);
			space.ahead = ToIPL(ahead);
			space.origin = ToIPL(t.position);
			return space;
		}

		IPLSceneType ToIPL(SceneBackend b)
		{
			switch (b)
			{
			case SceneBackend::Embree:     return IPL_SCENETYPE_EMBREE;
			case SceneBackend::RadeonRays: return IPL_SCENETYPE_RADEONRAYS;
			default:                       return IPL_SCENETYPE_DEFAULT;
			}
		}

		IPLHRTFInterpolation ToIPL(HRTFInterpolation i)
		{
			return i == HRTFInterpolation::Nearest ? IPL_HRTFINTERPOLATION_NEAREST : IPL_HRTFINTERPOLATION_BILINEAR;
		}

		IPLReflectionEffectType ToIPL(ReflectionMode m)
		{
			switch (m)
			{
			case ReflectionMode::Parametric:    return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
			case ReflectionMode::Hybrid:        return IPL_REFLECTIONEFFECTTYPE_HYBRID;
			case ReflectionMode::TrueAudioNext: return IPL_REFLECTIONEFFECTTYPE_TAN;
			default:                            return IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
			}
		}

		void SteamAudioLog(IPLLogLevel level, const char* message)
		{
			if (message == nullptr) return;
			switch (level)
			{
			case IPL_LOGLEVEL_ERROR:   wilog_error("SteamAudio: %s", message); break;
			case IPL_LOGLEVEL_WARNING: wilog_warning("SteamAudio: %s", message); break;
			default:                   wilog("SteamAudio: %s", message); break;
			}
		}
	}

	struct Spatializer::Impl
	{
		IPLContext context = nullptr;
		IPLHRTF hrtf = nullptr;
		IPLScene scene = nullptr;
		IPLSimulator simulator = nullptr;
		IPLAudioSettings audioSettings{};
		SimulationSettings sim;
		bool initialized = false;

		// Geometry. Materials are appended and never removed: an index handed out as a
		// mesh's material must stay valid for the mesh's life, and a handful of floats
		// is not worth a free list.
		std::vector<IPLMaterial> materials;
		std::mutex geometryMutex;
		std::unordered_map<uint32_t, IPLStaticMesh> meshes;
		uint32_t nextMeshHandle = 1;
		bool sceneDirty = false;

		// Sources are registered with the simulator, and iplSimulatorCommit must not
		// run while the audio thread is inside iplSourceGetOutputs. One mutex covers
		// both; it is taken for microseconds and only on add/remove and commit.
		std::mutex simulatorMutex;
		std::vector<IPLSource> sources;

		// The listener the SIMULATOR runs against. Multiple collectors can render, but
		// occlusion and reflections are simulated from one point of view - simulating
		// per collector would multiply the ray budget by the collector count. The
		// primary collector wins; secondary ones get the direct path spatialized from
		// their own position with the primary's occlusion verdict.
		std::mutex listenerMutex;
		SpatialTransform listener;
	};

	Spatializer::Spatializer() : impl_(std::make_unique<Impl>()) {}
	Spatializer::~Spatializer() { Shutdown(); }

	Spatializer& Spatializer::Get()
	{
		static Spatializer instance;
		return instance;
	}

	bool Spatializer::IsInitialized() const { return impl_->initialized; }
	bool Spatializer::IsSteamAudioAvailable() const { return impl_->initialized; }
	const char* Spatializer::GetBackendName() const { return "Steam Audio"; }
	const SimulationSettings& Spatializer::GetSimulationSettings() const { return impl_->sim; }

	bool Spatializer::Initialize(int sampleRate, int frameSize, const SimulationSettings& settings)
	{
		Impl& s = *impl_;
		if (s.initialized)
			return true;

		s.sim = settings;

		IPLContextSettings contextSettings{};
		contextSettings.version = STEAMAUDIO_VERSION;
		contextSettings.logCallback = SteamAudioLog;
		contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2; // ceiling, not a requirement: the SDK picks the best the CPU has
		if (iplContextCreate(&contextSettings, &s.context) != IPL_STATUS_SUCCESS)
		{
			wilog_error("stAudioSpatial: iplContextCreate failed - falling back to plain panning.");
			return false;
		}

		s.audioSettings.samplingRate = sampleRate;
		s.audioSettings.frameSize = frameSize;

		IPLHRTFSettings hrtfSettings{};
		hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
		hrtfSettings.volume = 1.0f;
		hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
		if (iplHRTFCreate(s.context, &s.audioSettings, &hrtfSettings, &s.hrtf) != IPL_STATUS_SUCCESS)
		{
			wilog_error("stAudioSpatial: iplHRTFCreate failed.");
			iplContextRelease(&s.context);
			return false;
		}

		IPLSceneSettings sceneSettings{};
		sceneSettings.type = ToIPL(settings.backend);
		if (iplSceneCreate(s.context, &sceneSettings, &s.scene) != IPL_STATUS_SUCCESS)
		{
			// A missing Embree/RadeonRays runtime is the usual cause; the built-in BVH
			// always works, so retry with it rather than losing spatial audio entirely.
			wilog_warning("stAudioSpatial: scene backend unavailable, retrying with the built-in ray tracer.");
			sceneSettings.type = IPL_SCENETYPE_DEFAULT;
			s.sim.backend = SceneBackend::Default;
			if (iplSceneCreate(s.context, &sceneSettings, &s.scene) != IPL_STATUS_SUCCESS)
			{
				wilog_error("stAudioSpatial: iplSceneCreate failed.");
				iplHRTFRelease(&s.hrtf);
				iplContextRelease(&s.context);
				return false;
			}
		}

		IPLSimulationSettings simSettings{};
		simSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
		if (settings.reflections) simSettings.flags = (IPLSimulationFlags)(simSettings.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
		if (settings.pathing)     simSettings.flags = (IPLSimulationFlags)(simSettings.flags | IPL_SIMULATIONFLAGS_PATHING);
		simSettings.sceneType = sceneSettings.type;
		simSettings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
		simSettings.maxNumOcclusionSamples = 64;
		simSettings.maxNumRays = settings.maxRays;
		simSettings.numDiffuseSamples = 1024;
		simSettings.maxDuration = settings.maxDuration;
		simSettings.maxOrder = settings.maxOrder;
		simSettings.maxNumSources = settings.maxSources;
		simSettings.numThreads = settings.threads;
		simSettings.rayBatchSize = settings.raysPerBounce;
		simSettings.numVisSamples = 4;
		simSettings.samplingRate = sampleRate;
		simSettings.frameSize = frameSize;
		if (iplSimulatorCreate(s.context, &simSettings, &s.simulator) != IPL_STATUS_SUCCESS)
		{
			wilog_error("stAudioSpatial: iplSimulatorCreate failed.");
			iplSceneRelease(&s.scene);
			iplHRTFRelease(&s.hrtf);
			iplContextRelease(&s.context);
			return false;
		}
		iplSimulatorSetScene(s.simulator, s.scene);
		iplSimulatorCommit(s.simulator);

		s.initialized = true;
		wilog("stAudioSpatial: Steam Audio %d.%d.%d up (%d Hz, %d-frame blocks, %s ray tracer, %d rays / %d bounces).",
			STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH,
			sampleRate, frameSize,
			s.sim.backend == SceneBackend::Embree ? "Embree" :
			s.sim.backend == SceneBackend::RadeonRays ? "Radeon Rays" : "built-in",
			settings.maxRays, settings.maxBounces);
		return true;
	}

	void Spatializer::Shutdown()
	{
		Impl& s = *impl_;
		if (!s.initialized)
			return;
		s.initialized = false;

		ClearGeometry();
		{
			std::lock_guard<std::mutex> lock(s.simulatorMutex);
			s.sources.clear();
			if (s.simulator) iplSimulatorRelease(&s.simulator);
		}
		if (s.scene)   iplSceneRelease(&s.scene);
		if (s.hrtf)    iplHRTFRelease(&s.hrtf);
		if (s.context) iplContextRelease(&s.context);
		s.materials.clear();
	}

	int Spatializer::AddMaterial(const MaterialDesc& material)
	{
		Impl& s = *impl_;
		if (!s.initialized) return -1;
		std::lock_guard<std::mutex> lock(s.geometryMutex);
		IPLMaterial m{};
		m.absorption[0] = material.absorption[0];
		m.absorption[1] = material.absorption[1];
		m.absorption[2] = material.absorption[2];
		m.scattering = material.scattering;
		m.transmission[0] = material.transmission[0];
		m.transmission[1] = material.transmission[1];
		m.transmission[2] = material.transmission[2];
		s.materials.push_back(m);
		return (int)s.materials.size() - 1;
	}

	uint32_t Spatializer::AddStaticMesh(const float* vertices, int vertexCount,
		const int32_t* indices, int triangleCount, const int32_t* materialIndices)
	{
		Impl& s = *impl_;
		if (!s.initialized || vertices == nullptr || indices == nullptr || vertexCount <= 0 || triangleCount <= 0)
			return 0;

		std::lock_guard<std::mutex> lock(s.geometryMutex);
		if (s.materials.empty())
		{
			// Every mesh needs a material index into a non-empty array; give the scene
			// a neutral one rather than refusing the geometry.
			IPLMaterial fallback{};
			fallback.absorption[0] = 0.10f; fallback.absorption[1] = 0.20f; fallback.absorption[2] = 0.30f;
			fallback.scattering = 0.05f;
			fallback.transmission[0] = 0.20f; fallback.transmission[1] = 0.05f; fallback.transmission[2] = 0.01f;
			s.materials.push_back(fallback);
		}

		// IPLVector3 and IPLTriangle are layout-compatible with float[3] / int32[3],
		// but the SDK copies the arrays inside iplStaticMeshCreate, so these staging
		// vectors only need to outlive the call.
		std::vector<IPLVector3> verts((size_t)vertexCount);
		for (int i = 0; i < vertexCount; ++i)
			verts[(size_t)i] = IPLVector3{ vertices[i * 3 + 0], vertices[i * 3 + 1], vertices[i * 3 + 2] };
		std::vector<IPLTriangle> tris((size_t)triangleCount);
		for (int i = 0; i < triangleCount; ++i)
		{
			tris[(size_t)i].indices[0] = indices[i * 3 + 0];
			tris[(size_t)i].indices[1] = indices[i * 3 + 1];
			tris[(size_t)i].indices[2] = indices[i * 3 + 2];
		}
		std::vector<IPLint32> mats((size_t)triangleCount, 0);
		if (materialIndices != nullptr)
		{
			for (int i = 0; i < triangleCount; ++i)
			{
				const int32_t m = materialIndices[i];
				mats[(size_t)i] = (m >= 0 && m < (int32_t)s.materials.size()) ? (IPLint32)m : 0;
			}
		}

		IPLStaticMeshSettings meshSettings{};
		meshSettings.numVertices = vertexCount;
		meshSettings.numTriangles = triangleCount;
		meshSettings.numMaterials = (IPLint32)s.materials.size();
		meshSettings.vertices = verts.data();
		meshSettings.triangles = tris.data();
		meshSettings.materialIndices = mats.data();
		meshSettings.materials = s.materials.data();

		IPLStaticMesh mesh = nullptr;
		if (iplStaticMeshCreate(s.scene, &meshSettings, &mesh) != IPL_STATUS_SUCCESS)
		{
			wilog_error("stAudioSpatial: iplStaticMeshCreate failed (%d verts, %d tris).", vertexCount, triangleCount);
			return 0;
		}
		iplStaticMeshAdd(mesh, s.scene);
		const uint32_t handle = s.nextMeshHandle++;
		s.meshes[handle] = mesh;
		s.sceneDirty = true;
		return handle;
	}

	void Spatializer::RemoveMesh(uint32_t meshHandle)
	{
		Impl& s = *impl_;
		if (!s.initialized) return;
		std::lock_guard<std::mutex> lock(s.geometryMutex);
		auto it = s.meshes.find(meshHandle);
		if (it == s.meshes.end()) return;
		iplStaticMeshRemove(it->second, s.scene);
		iplStaticMeshRelease(&it->second);
		s.meshes.erase(it);
		s.sceneDirty = true;
	}

	void Spatializer::ClearGeometry()
	{
		Impl& s = *impl_;
		std::lock_guard<std::mutex> lock(s.geometryMutex);
		for (auto& entry : s.meshes)
		{
			if (s.scene) iplStaticMeshRemove(entry.second, s.scene);
			iplStaticMeshRelease(&entry.second);
		}
		s.meshes.clear();
		s.sceneDirty = true;
	}

	void Spatializer::CommitScene()
	{
		Impl& s = *impl_;
		if (!s.initialized) return;
		std::lock_guard<std::mutex> lock(s.geometryMutex);
		if (!s.sceneDirty) return;
		iplSceneCommit(s.scene);
		{
			std::lock_guard<std::mutex> simLock(s.simulatorMutex);
			iplSimulatorCommit(s.simulator);
		}
		s.sceneDirty = false;
	}

	void Spatializer::RunSimulation()
	{
		Impl& s = *impl_;
		if (!s.initialized) return;

		SpatialTransform listener;
		{
			std::lock_guard<std::mutex> lock(s.listenerMutex);
			listener = s.listener;
		}

		IPLSimulationSharedInputs shared{};
		shared.listener = ToIPLSpace(listener);
		shared.numRays = s.sim.maxRays;
		shared.numBounces = s.sim.maxBounces;
		shared.duration = s.sim.maxDuration;
		shared.order = s.sim.maxOrder;
		shared.irradianceMinDistance = s.sim.irradianceMinDistance;

		std::lock_guard<std::mutex> lock(s.simulatorMutex);
		IPLSimulationFlags flags = IPL_SIMULATIONFLAGS_DIRECT;
		if (s.sim.reflections) flags = (IPLSimulationFlags)(flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
		if (s.sim.pathing)     flags = (IPLSimulationFlags)(flags | IPL_SIMULATIONFLAGS_PATHING);
		iplSimulatorSetSharedInputs(s.simulator, flags, &shared);

		// Direct is cheap enough to run every pass; reflections and pathing are the
		// ray-tracing spend and are why this whole function lives on a worker thread.
		iplSimulatorRunDirect(s.simulator);
		if (s.sim.reflections) iplSimulatorRunReflections(s.simulator);
		if (s.sim.pathing)     iplSimulatorRunPathing(s.simulator);
	}

	// SpatialSource

	struct SpatialSource::Impl
	{
		IPLSource source = nullptr;
		bool registered = false;

		std::mutex mutex;             // guards transform + result, both tiny
		SpatialTransform transform;
		SpatialResult result;

		IPLSimulationOutputs outputs{};   // last simulator verdict, read on the audio thread
		std::atomic<bool> outputsValid{ false };
	};

	SpatialSource::SpatialSource() : impl_(std::make_unique<Impl>()) {}
	SpatialSource::~SpatialSource() { Destroy(); }
	bool SpatialSource::IsValid() const { return impl_->source != nullptr; }

	bool SpatialSource::Create(const EmitterSpatialSettings& settings)
	{
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		if (!g.initialized)
			return false;
		settings_ = settings;

		IPLSourceSettings sourceSettings{};
		sourceSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
		if (settings.reflections && g.sim.reflections)
			sourceSettings.flags = (IPLSimulationFlags)(sourceSettings.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
		if (settings.pathing && g.sim.pathing)
			sourceSettings.flags = (IPLSimulationFlags)(sourceSettings.flags | IPL_SIMULATIONFLAGS_PATHING);

		std::lock_guard<std::mutex> lock(g.simulatorMutex);
		if (iplSourceCreate(g.simulator, &sourceSettings, &impl_->source) != IPL_STATUS_SUCCESS)
		{
			wilog_warning("stAudioSpatial: iplSourceCreate failed (simulator source budget is %d).", g.sim.maxSources);
			return false;
		}
		iplSourceAdd(impl_->source, g.simulator);
		iplSimulatorCommit(g.simulator);
		impl_->registered = true;
		g.sources.push_back(impl_->source);
		return true;
	}

	void SpatialSource::Destroy()
	{
		if (impl_->source == nullptr)
			return;
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		std::lock_guard<std::mutex> lock(g.simulatorMutex);
		if (impl_->registered && g.simulator)
		{
			iplSourceRemove(impl_->source, g.simulator);
			iplSimulatorCommit(g.simulator);
			g.sources.erase(std::remove(g.sources.begin(), g.sources.end(), impl_->source), g.sources.end());
		}
		iplSourceRelease(&impl_->source);
		impl_->source = nullptr;
		impl_->registered = false;
	}

	void SpatialSource::SetSettings(const EmitterSpatialSettings& settings) { settings_ = settings; }

	void SpatialSource::SetTransform(const SpatialTransform& transform)
	{
		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->transform = transform;
		}
		if (impl_->source == nullptr)
			return;

		Spatializer::Impl& g = *Spatializer::Get().impl_;
		const EmitterSpatialSettings& es = settings_;

		IPLSimulationInputs inputs{};
		inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
		if (es.reflections && g.sim.reflections)
			inputs.flags = (IPLSimulationFlags)(inputs.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
		if (es.pathing && g.sim.pathing)
			inputs.flags = (IPLSimulationFlags)(inputs.flags | IPL_SIMULATIONFLAGS_PATHING);

		IPLDirectSimulationFlags direct = (IPLDirectSimulationFlags)0;
		if (es.distanceModel != DistanceAttenuationModel::None)
			direct = (IPLDirectSimulationFlags)(direct | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
		if (es.airAbsorption != AirAbsorptionModel::None)
			direct = (IPLDirectSimulationFlags)(direct | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
		if (es.applyDirectivity)
			direct = (IPLDirectSimulationFlags)(direct | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY);
		if (es.occlusion != OcclusionMode::Off)
			direct = (IPLDirectSimulationFlags)(direct | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
		if (es.transmission != TransmissionMode::Off)
			direct = (IPLDirectSimulationFlags)(direct | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
		inputs.directFlags = direct;

		inputs.source = ToIPLSpace(transform);

		inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
		inputs.distanceAttenuationModel.minDistance = es.minDistance;

		inputs.airAbsorptionModel.type = (es.airAbsorption == AirAbsorptionModel::Exponential)
			? IPL_AIRABSORPTIONTYPE_EXPONENTIAL : IPL_AIRABSORPTIONTYPE_DEFAULT;
		inputs.airAbsorptionModel.coefficients[0] = es.airAbsorptionCoefficients[0];
		inputs.airAbsorptionModel.coefficients[1] = es.airAbsorptionCoefficients[1];
		inputs.airAbsorptionModel.coefficients[2] = es.airAbsorptionCoefficients[2];

		inputs.directivity.dipoleWeight = es.dipoleWeight;
		inputs.directivity.dipolePower = es.dipolePower;

		inputs.occlusionType = (es.occlusion == OcclusionMode::Volumetric)
			? IPL_OCCLUSIONTYPE_VOLUMETRIC : IPL_OCCLUSIONTYPE_RAYCAST;
		inputs.occlusionRadius = es.occlusionRadius;
		inputs.numOcclusionSamples = es.occlusionSamples;
		inputs.numTransmissionRays = es.transmissionRays;

		inputs.reverbScale[0] = 1.0f;
		inputs.reverbScale[1] = 1.0f;
		inputs.reverbScale[2] = 1.0f;
		inputs.hybridReverbTransitionTime = 1.0f;
		inputs.hybridReverbOverlapPercent = 0.25f;

		inputs.visRadius = es.pathingVisRadius;
		inputs.visThreshold = es.pathingVisThreshold;
		inputs.visRange = es.pathingRange;
		inputs.pathingOrder = g.sim.maxOrder;
		inputs.enableValidation = es.pathingValidation ? IPL_TRUE : IPL_FALSE;
		inputs.findAlternatePaths = IPL_TRUE;

		IPLSimulationOutputs outputs{};
		{
			std::lock_guard<std::mutex> lock(g.simulatorMutex);
			iplSourceSetInputs(impl_->source, inputs.flags, &inputs);
			// Pull the previous pass's verdict while we hold the lock: this is the only
			// place the game thread and the simulator meet, and the audio thread reads
			// the cached copy instead of touching the simulator at all.
			iplSourceGetOutputs(impl_->source, inputs.flags, &outputs);
		}

		XMFLOAT3 listenerPosition;
		{
			std::lock_guard<std::mutex> lock(g.listenerMutex);
			listenerPosition = g.listener.position;
		}

		SpatialResult r;
		r.distance = Length(Sub(transform.position, listenerPosition));
		r.distanceAttenuation = outputs.direct.distanceAttenuation;
		r.occlusion = outputs.direct.occlusion;
		r.directivity = outputs.direct.directivity;
		for (int i = 0; i < 3; ++i)
		{
			r.airAbsorption[i] = outputs.direct.airAbsorption[i];
			r.transmission[i] = outputs.direct.transmission[i];
		}
		r.audible = r.distance <= es.maxDistance;

		// outputs and result are published together under the source's own lock: the
		// audio thread reads both there, so it can never pair a fresh direct-path
		// parameter block with a stale distance.
		{
			std::lock_guard<std::mutex> resultLock(impl_->mutex);
			impl_->outputs = outputs;
			impl_->result = r;
		}
		impl_->outputsValid.store(true, std::memory_order_release);
	}

	SpatialResult SpatialSource::GetResult() const
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return impl_->result;
	}

	// SpatialRenderer

	struct SpatialRenderer::Impl
	{
		// Effect state is per (emitter, collector) pair: two microphones hearing the
		// same emitter run two independent interpolators. Keyed by source pointer.
		struct PerSource
		{
			IPLDirectEffect direct = nullptr;
			IPLBinauralEffect binaural = nullptr;
			IPLAmbisonicsEncodeEffect encode = nullptr;
			IPLReflectionEffect reflection = nullptr;
			IPLPathEffect path = nullptr;
		};
		std::unordered_map<SpatialSource*, PerSource> perSource;

		IPLAmbisonicsDecodeEffect decode = nullptr;
		IPLAudioBuffer monoIn{};       // 1 x frameSize
		IPLAudioBuffer directOut{};    // 1 x frameSize
		IPLAudioBuffer spatialOut{};   // 2 or (order+1)^2 x frameSize
		IPLAudioBuffer accumulator{};  // the collector's mix bus
		IPLAudioBuffer ambiBus{};      // reflection / ambisonic accumulation
		int frameSize = 0;
		int busChannels = 0;
		int ambiChannels = 0;
		bool valid = false;
	};

	SpatialRenderer::SpatialRenderer() : impl_(std::make_unique<Impl>()) {}
	SpatialRenderer::~SpatialRenderer() { Destroy(); }
	bool SpatialRenderer::IsValid() const { return impl_->valid; }

	int SpatialRenderer::GetOutputChannels() const
	{
		if (settings_.output == SpatialOutput::Ambisonics)
		{
			const int order = std::max(0, std::min(settings_.ambisonicsOrder, 3));
			return (order + 1) * (order + 1);
		}
		return 2;
	}

	bool SpatialRenderer::Create(const CollectorSpatialSettings& settings)
	{
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		if (!g.initialized)
			return false;
		settings_ = settings;
		Impl& s = *impl_;
		s.frameSize = g.audioSettings.frameSize;
		s.busChannels = GetOutputChannels();
		const int order = std::max(0, std::min(std::max(settings.ambisonicsOrder, g.sim.maxOrder), 3));
		s.ambiChannels = (order + 1) * (order + 1);

		iplAudioBufferAllocate(g.context, 1, s.frameSize, &s.monoIn);
		iplAudioBufferAllocate(g.context, 1, s.frameSize, &s.directOut);
		iplAudioBufferAllocate(g.context, std::max(2, s.busChannels), s.frameSize, &s.spatialOut);
		iplAudioBufferAllocate(g.context, std::max(2, s.busChannels), s.frameSize, &s.accumulator);
		iplAudioBufferAllocate(g.context, s.ambiChannels, s.frameSize, &s.ambiBus);

		IPLAmbisonicsDecodeEffectSettings decodeSettings{};
		decodeSettings.maxOrder = order;
		decodeSettings.hrtf = g.hrtf;
		decodeSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
		if (iplAmbisonicsDecodeEffectCreate(g.context, &g.audioSettings, &decodeSettings, &s.decode) != IPL_STATUS_SUCCESS)
		{
			wilog_warning("stAudioSpatial: iplAmbisonicsDecodeEffectCreate failed - reflections will not be decoded.");
			s.decode = nullptr;
		}

		s.valid = true;
		return true;
	}

	void SpatialRenderer::Destroy()
	{
		Impl& s = *impl_;
		if (!s.valid)
			return;
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		for (auto& entry : s.perSource)
		{
			if (entry.second.direct)     iplDirectEffectRelease(&entry.second.direct);
			if (entry.second.binaural)   iplBinauralEffectRelease(&entry.second.binaural);
			if (entry.second.encode)     iplAmbisonicsEncodeEffectRelease(&entry.second.encode);
			if (entry.second.reflection) iplReflectionEffectRelease(&entry.second.reflection);
			if (entry.second.path)       iplPathEffectRelease(&entry.second.path);
		}
		s.perSource.clear();
		if (s.decode) iplAmbisonicsDecodeEffectRelease(&s.decode);
		if (g.context)
		{
			iplAudioBufferFree(g.context, &s.monoIn);
			iplAudioBufferFree(g.context, &s.directOut);
			iplAudioBufferFree(g.context, &s.spatialOut);
			iplAudioBufferFree(g.context, &s.accumulator);
			iplAudioBufferFree(g.context, &s.ambiBus);
		}
		s.valid = false;
	}

	void SpatialRenderer::SetSettings(const CollectorSpatialSettings& settings) { settings_ = settings; }

	void SpatialRenderer::SetTransform(const SpatialTransform& transform)
	{
		transform_ = transform;
		// The simulator has one point of view; the primary collector publishes it. A
		// secondary collector calling this only moves its own rendering, which is why
		// the write is unconditional but the simulator only ever reads the last writer
		// before RunSimulation - and the engine makes sure that is the primary.
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		if (!g.initialized) return;
		std::lock_guard<std::mutex> lock(g.listenerMutex);
		g.listener = transform;
	}

	void SpatialRenderer::BeginBlock(int frames)
	{
		Impl& s = *impl_;
		if (!s.valid) return;
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		for (int c = 0; c < s.accumulator.numChannels; ++c)
			std::memset(s.accumulator.data[c], 0, (size_t)frames * sizeof(float));
		for (int c = 0; c < s.ambiBus.numChannels; ++c)
			std::memset(s.ambiBus.data[c], 0, (size_t)frames * sizeof(float));
		(void)g;
	}

	void SpatialRenderer::Accumulate(SpatialSource& source, const float* monoInput, int frames, float gain)
	{
		Impl& s = *impl_;
		if (!s.valid || monoInput == nullptr || frames <= 0)
			return;
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		const EmitterSpatialSettings& es = source.settings_;

		// Lazily build this pair's effect chain. The allocation happens on the audio
		// thread the first block an emitter is audible to this collector, which is one
		// malloc at the moment a sound starts rather than a stall in steady state. It
		// is the price of not pre-allocating emitters x collectors effect chains up
		// front; if a project ever hears it, pre-warm by calling Accumulate with a
		// silent block when the emitter is created.
		auto it = s.perSource.find(&source);
		if (it == s.perSource.end())
		{
			Impl::PerSource ps{};
			IPLDirectEffectSettings directSettings{};
			directSettings.numChannels = 1;
			iplDirectEffectCreate(g.context, &g.audioSettings, &directSettings, &ps.direct);

			if (es.output == SpatialOutput::Ambisonics || settings_.output == SpatialOutput::Ambisonics)
			{
				IPLAmbisonicsEncodeEffectSettings encodeSettings{};
				encodeSettings.maxOrder = std::max(0, std::min(settings_.ambisonicsOrder, 3));
				iplAmbisonicsEncodeEffectCreate(g.context, &g.audioSettings, &encodeSettings, &ps.encode);
			}
			else
			{
				IPLBinauralEffectSettings binauralSettings{};
				binauralSettings.hrtf = g.hrtf;
				iplBinauralEffectCreate(g.context, &g.audioSettings, &binauralSettings, &ps.binaural);
			}

			if (es.reflections && g.sim.reflections)
			{
				IPLReflectionEffectSettings reflectionSettings{};
				reflectionSettings.type = ToIPL(ReflectionMode::Convolution);
				reflectionSettings.irSize = (IPLint32)(g.sim.maxDuration * (float)g.audioSettings.samplingRate);
				reflectionSettings.numChannels = s.ambiChannels;
				iplReflectionEffectCreate(g.context, &g.audioSettings, &reflectionSettings, &ps.reflection);
			}
			if (es.pathing && g.sim.pathing)
			{
				IPLPathEffectSettings pathSettings{};
				pathSettings.maxOrder = g.sim.maxOrder;
				pathSettings.spatialize = IPL_FALSE; // the ambisonic bus is decoded once, at EndBlock
				pathSettings.hrtf = g.hrtf;
				pathSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
				iplPathEffectCreate(g.context, &g.audioSettings, &pathSettings, &ps.path);
			}
			it = s.perSource.emplace(&source, ps).first;
		}
		Impl::PerSource& ps = it->second;

		// Input: mono, pre-gained.
		for (int i = 0; i < frames; ++i)
			s.monoIn.data[0][i] = monoInput[i] * gain;
		s.monoIn.numSamples = frames;
		s.directOut.numSamples = frames;
		s.spatialOut.numSamples = frames;
		s.accumulator.numSamples = frames;
		s.ambiBus.numSamples = frames;

		// 1. Direct path: distance, air, directivity, occlusion, transmission.
		// One snapshot of everything the game thread publishes, taken once: the source
		// lock is held for a struct copy, never for the DSP that follows.
		const bool haveOutputs = source.impl_->outputsValid.load(std::memory_order_acquire);
		IPLSimulationOutputs sourceOutputs{};
		SpatialTransform sourceTransform;
		{
			std::lock_guard<std::mutex> lock(source.impl_->mutex);
			sourceOutputs = source.impl_->outputs;
			sourceTransform = source.impl_->transform;
		}

		if (ps.direct && haveOutputs)
		{
			IPLDirectEffectParams params = sourceOutputs.direct;
			IPLDirectEffectFlags applyFlags = (IPLDirectEffectFlags)0;
			if (es.applyDistanceAttenuation && es.distanceModel != DistanceAttenuationModel::None)
				applyFlags = (IPLDirectEffectFlags)(applyFlags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);
			if (es.applyAirAbsorption && es.airAbsorption != AirAbsorptionModel::None)
				applyFlags = (IPLDirectEffectFlags)(applyFlags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
			if (es.applyDirectivity)
				applyFlags = (IPLDirectEffectFlags)(applyFlags | IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
			if (es.applyOcclusion && es.occlusion != OcclusionMode::Off)
				applyFlags = (IPLDirectEffectFlags)(applyFlags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
			if (es.applyTransmission && es.transmission != TransmissionMode::Off)
				applyFlags = (IPLDirectEffectFlags)(applyFlags | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
			params.flags = applyFlags;
			params.transmissionType = (es.transmission == TransmissionMode::FrequencyDependent)
				? IPL_TRANSMISSIONTYPE_FREQDEPENDENT : IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;
			iplDirectEffectApply(ps.direct, &params, &s.monoIn, &s.directOut);
		}
		else
		{
			std::memcpy(s.directOut.data[0], s.monoIn.data[0], (size_t)frames * sizeof(float));
		}

		// 2. Spatialize the direct signal from THIS collector's point of view. The
		//    direction is computed here rather than taken from the simulator, so a
		//    second microphone hears the emitter from its own angle even though the
		//    occlusion verdict above came from the primary listener.
		const IPLCoordinateSpace3 listenerSpace = ToIPLSpace(transform_);
		IPLVector3 localDirection = iplCalculateRelativeDirection(
			g.context, ToIPL(sourceTransform.position), listenerSpace.origin, listenerSpace.ahead, listenerSpace.up);

		if (ps.binaural)
		{
			IPLBinauralEffectParams binauralParams{};
			binauralParams.direction = localDirection;
			binauralParams.interpolation = ToIPL(es.interpolation);
			binauralParams.spatialBlend = std::max(0.0f, std::min(es.spatialBlend, 1.0f));
			binauralParams.hrtf = g.hrtf;
			binauralParams.peakDelays = nullptr;
			iplBinauralEffectApply(ps.binaural, &binauralParams, &s.directOut, &s.spatialOut);
			iplAudioBufferMix(g.context, &s.spatialOut, &s.accumulator);
		}
		else if (ps.encode)
		{
			IPLAmbisonicsEncodeEffectParams encodeParams{};
			encodeParams.direction = localDirection;
			encodeParams.order = std::max(0, std::min(settings_.ambisonicsOrder, 3));
			iplAmbisonicsEncodeEffectApply(ps.encode, &encodeParams, &s.directOut, &s.ambiBus);
		}

		// 3. Reflections and pathing land on the ambisonic bus, decoded once in
		//    EndBlock rather than per source.
		if (ps.reflection && haveOutputs)
		{
			IPLReflectionEffectParams reflectionParams = sourceOutputs.reflections;
			reflectionParams.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
			reflectionParams.numChannels = s.ambiChannels;
			reflectionParams.irSize = (IPLint32)(g.sim.maxDuration * (float)g.audioSettings.samplingRate);
			iplReflectionEffectApply(ps.reflection, &reflectionParams, &s.directOut, &s.ambiBus, nullptr);
		}
		if (ps.path && haveOutputs)
		{
			IPLPathEffectParams pathParams = sourceOutputs.pathing;
			pathParams.order = g.sim.maxOrder;
			pathParams.binaural = IPL_FALSE;
			pathParams.hrtf = g.hrtf;
			pathParams.listener = listenerSpace;
			iplPathEffectApply(ps.path, &pathParams, &s.directOut, &s.ambiBus);
		}
	}

	void SpatialRenderer::EndBlock(float* const* out, int frames)
	{
		Impl& s = *impl_;
		if (!s.valid || out == nullptr || frames <= 0)
			return;
		Spatializer::Impl& g = *Spatializer::Get().impl_;

		if (settings_.output == SpatialOutput::Ambisonics)
		{
			// The collector wants B-format: hand back the raw ambisonic bus, mixed with
			// whatever the encode path already put there.
			const int channels = std::min(GetOutputChannels(), s.ambiBus.numChannels);
			for (int c = 0; c < channels; ++c)
				if (out[c]) std::memcpy(out[c], s.ambiBus.data[c], (size_t)frames * sizeof(float));
			return;
		}

		// Decode the ambisonic bus (reflections, pathing, ambisonically-encoded direct
		// paths) down to stereo and fold it into the binaural accumulator.
		if (s.decode)
		{
			IPLAmbisonicsDecodeEffectParams decodeParams{};
			decodeParams.order = std::max(0, std::min(std::max(settings_.ambisonicsOrder, g.sim.maxOrder), 3));
			decodeParams.hrtf = g.hrtf;
			decodeParams.orientation = ToIPLSpace(transform_);
			decodeParams.binaural = settings_.binauralReverb ? IPL_TRUE : IPL_FALSE;
			s.spatialOut.numSamples = frames;
			iplAmbisonicsDecodeEffectApply(s.decode, &decodeParams, &s.ambiBus, &s.spatialOut);
			iplAudioBufferMix(g.context, &s.spatialOut, &s.accumulator);
		}

		for (int c = 0; c < 2; ++c)
			if (out[c]) std::memcpy(out[c], s.accumulator.data[std::min(c, s.accumulator.numChannels - 1)],
				(size_t)frames * sizeof(float));
	}

#else // !SIMTARY_HAS_STEAMAUDIO

	// Fallback path - no Steam Audio SDK in this build.
	//
	// Inverse-distance attenuation plus constant-power panning. No HRTF, no occlusion,
	// no reflections, no pathing. Everything above still compiles and every component
	// option is still accepted and stored; the ones that need ray tracing are simply
	// inert. This exists so the engine builds and makes noise on a machine that never
	// fetched the SDK, not as a shipping configuration.

	struct Spatializer::Impl
	{
		bool initialized = false;
		SimulationSettings sim;
		std::mutex listenerMutex;
		SpatialTransform listener;
	};

	Spatializer::Spatializer() : impl_(std::make_unique<Impl>()) {}
	Spatializer::~Spatializer() { Shutdown(); }

	Spatializer& Spatializer::Get()
	{
		static Spatializer instance;
		return instance;
	}

	bool Spatializer::Initialize(int sampleRate, int frameSize, const SimulationSettings& settings)
	{
		impl_->sim = settings;
		impl_->initialized = true;
		wilog_warning("stAudioSpatial: built without Steam Audio (SIMTARY_HAS_STEAMAUDIO undefined) - "
			"3D audio falls back to distance attenuation and stereo panning. %d Hz, %d-frame blocks.",
			sampleRate, frameSize);
		return true;
	}

	void Spatializer::Shutdown() { impl_->initialized = false; }
	bool Spatializer::IsInitialized() const { return impl_->initialized; }
	bool Spatializer::IsSteamAudioAvailable() const { return false; }
	const char* Spatializer::GetBackendName() const { return "fallback panner"; }
	const SimulationSettings& Spatializer::GetSimulationSettings() const { return impl_->sim; }

	int  Spatializer::AddMaterial(const MaterialDesc&) { return -1; }
	uint32_t Spatializer::AddStaticMesh(const float*, int, const int32_t*, int, const int32_t*) { return 0; }
	void Spatializer::RemoveMesh(uint32_t) {}
	void Spatializer::ClearGeometry() {}
	void Spatializer::CommitScene() {}
	void Spatializer::RunSimulation() {}

	struct SpatialSource::Impl
	{
		std::mutex mutex;
		SpatialTransform transform;
		SpatialResult result;
	};

	SpatialSource::SpatialSource() : impl_(std::make_unique<Impl>()) {}
	SpatialSource::~SpatialSource() {}
	bool SpatialSource::Create(const EmitterSpatialSettings& settings) { settings_ = settings; return true; }
	void SpatialSource::Destroy() {}
	bool SpatialSource::IsValid() const { return true; }
	void SpatialSource::SetSettings(const EmitterSpatialSettings& settings) { settings_ = settings; }

	void SpatialSource::SetTransform(const SpatialTransform& transform)
	{
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		SpatialTransform listener;
		{
			std::lock_guard<std::mutex> lock(g.listenerMutex);
			listener = g.listener;
		}
		SpatialResult r;
		r.distance = Length(Sub(transform.position, listener.position));
		r.distanceAttenuation = settings_.distanceModel == DistanceAttenuationModel::None
			? 1.0f : settings_.minDistance / std::max(r.distance, settings_.minDistance);
		r.audible = r.distance <= settings_.maxDistance;
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->transform = transform;
		impl_->result = r;
	}

	SpatialResult SpatialSource::GetResult() const
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return impl_->result;
	}

	struct SpatialRenderer::Impl
	{
		std::vector<float> accumL, accumR;
		bool valid = false;
	};

	SpatialRenderer::SpatialRenderer() : impl_(std::make_unique<Impl>()) {}
	SpatialRenderer::~SpatialRenderer() {}
	bool SpatialRenderer::Create(const CollectorSpatialSettings& settings) { settings_ = settings; impl_->valid = true; return true; }
	void SpatialRenderer::Destroy() { impl_->valid = false; }
	bool SpatialRenderer::IsValid() const { return impl_->valid; }
	void SpatialRenderer::SetSettings(const CollectorSpatialSettings& settings) { settings_ = settings; }
	int  SpatialRenderer::GetOutputChannels() const { return 2; }

	void SpatialRenderer::SetTransform(const SpatialTransform& transform)
	{
		transform_ = transform;
		Spatializer::Impl& g = *Spatializer::Get().impl_;
		std::lock_guard<std::mutex> lock(g.listenerMutex);
		g.listener = transform;
	}

	void SpatialRenderer::BeginBlock(int frames)
	{
		impl_->accumL.assign((size_t)frames, 0.0f);
		impl_->accumR.assign((size_t)frames, 0.0f);
	}

	void SpatialRenderer::Accumulate(SpatialSource& source, const float* monoInput, int frames, float gain)
	{
		if (monoInput == nullptr || frames <= 0) return;
		SpatialTransform st;
		{
			std::lock_guard<std::mutex> lock(source.impl_->mutex);
			st = source.impl_->transform;
		}
		const EmitterSpatialSettings& es = source.GetSettings();
		const XMFLOAT3 delta = Sub(st.position, transform_.position);
		const float distance = Length(delta);
		if (distance > es.maxDistance) return;

		const float attenuation = (es.distanceModel == DistanceAttenuationModel::None)
			? 1.0f : es.minDistance / std::max(distance, es.minDistance);

		// Constant-power pan from the azimuth in the listener's own basis.
		const XMFLOAT3 ahead = Normalize(transform_.forward);
		XMFLOAT3 right = Normalize(Cross(ahead, Normalize(transform_.up)));
		const XMFLOAT3 dir = Normalize(delta);
		const float pan = std::max(-1.0f, std::min(Dot(dir, right), 1.0f)) * es.spatialBlend;
		const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
		const float gl = std::cos(angle) * attenuation * gain;
		const float gr = std::sin(angle) * attenuation * gain;

		for (int i = 0; i < frames; ++i)
		{
			impl_->accumL[(size_t)i] += monoInput[i] * gl;
			impl_->accumR[(size_t)i] += monoInput[i] * gr;
		}
	}

	void SpatialRenderer::EndBlock(float* const* out, int frames)
	{
		if (out == nullptr) return;
		if (out[0]) std::memcpy(out[0], impl_->accumL.data(), (size_t)frames * sizeof(float));
		if (out[1]) std::memcpy(out[1], impl_->accumR.data(), (size_t)frames * sizeof(float));
	}

#endif // SIMTARY_HAS_STEAMAUDIO
}
