#include "stAudioGeometry.h"
#include "wiScene.h"
#include "wiRenderer.h"
#include "wiBacklog.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace wi::ecs;
using namespace wi::scene;

namespace st
{
	namespace
	{
		// Steam Audio's own published material table, in Preset order. Custom has no row -
		// it reads the component's fields instead - so this array is one shorter than the
		// enum and every lookup is bounds-checked against its size.
		const audio::Spatializer::MaterialDesc kMaterialPresets[] = {
			/* Generic  */ { { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.100f, 0.050f, 0.030f } },
			/* Brick    */ { { 0.03f, 0.04f, 0.07f }, 0.05f, { 0.015f, 0.015f, 0.015f } },
			/* Concrete */ { { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.015f, 0.002f, 0.001f } },
			/* Ceramic  */ { { 0.01f, 0.02f, 0.02f }, 0.05f, { 0.060f, 0.044f, 0.011f } },
			/* Gravel   */ { { 0.60f, 0.70f, 0.80f }, 0.05f, { 0.031f, 0.012f, 0.008f } },
			/* Carpet   */ { { 0.24f, 0.69f, 0.73f }, 0.05f, { 0.020f, 0.005f, 0.003f } },
			/* Glass    */ { { 0.06f, 0.03f, 0.02f }, 0.05f, { 0.060f, 0.044f, 0.011f } },
			/* Plaster  */ { { 0.12f, 0.06f, 0.04f }, 0.05f, { 0.056f, 0.056f, 0.004f } },
			/* Wood     */ { { 0.11f, 0.07f, 0.06f }, 0.05f, { 0.070f, 0.014f, 0.005f } },
			/* Metal    */ { { 0.20f, 0.07f, 0.06f }, 0.05f, { 0.200f, 0.025f, 0.010f } },
			/* Rock     */ { { 0.13f, 0.20f, 0.24f }, 0.05f, { 0.015f, 0.002f, 0.001f } },
		};

		bool MatrixNearlyEqual(const XMFLOAT4X4& a, const XMFLOAT4X4& b, float epsilon = 0.0005f)
		{
			const float* pa = &a._11;
			const float* pb = &b._11;
			for (int i = 0; i < 16; ++i)
			{
				if (std::fabs(pa[i] - pb[i]) > epsilon)
					return false;
			}
			return true;
		}

		// One box, eight corners, twelve triangles, all baked into world space. The winding
		// is the engine's own outward-facing one; every shape here is a CLOSED solid, which
		// is what keeps that from mattering much - a listener inside a room still faces a
		// real surface (the inner face of the wall slab), whichever way the tracer decides
		// a normal points.
		void AppendBox(std::vector<float>& vertices, std::vector<int32_t>& indices,
			const XMFLOAT3& center, const XMFLOAT3& halfExtents, const XMMATRIX& matrix)
		{
			const int32_t base = (int32_t)(vertices.size() / 3);

			for (int corner = 0; corner < 8; ++corner)
			{
				const float sx = (corner & 1) ? 1.0f : -1.0f;
				const float sy = (corner & 2) ? 1.0f : -1.0f;
				const float sz = (corner & 4) ? 1.0f : -1.0f;
				const XMVECTOR local = XMVectorSet(
					center.x + sx * halfExtents.x,
					center.y + sy * halfExtents.y,
					center.z + sz * halfExtents.z, 1.0f);
				XMFLOAT3 world;
				XMStoreFloat3(&world, XMVector3Transform(local, matrix));
				vertices.push_back(world.x);
				vertices.push_back(world.y);
				vertices.push_back(world.z);
			}

			// Corner index bit 0 = +X, bit 1 = +Y, bit 2 = +Z.
			static const int32_t kFaces[12][3] = {
				{ 0, 2, 3 }, { 0, 3, 1 },   // -Z
				{ 4, 5, 7 }, { 4, 7, 6 },   // +Z
				{ 0, 4, 6 }, { 0, 6, 2 },   // -X
				{ 1, 3, 7 }, { 1, 7, 5 },   // +X
				{ 0, 1, 5 }, { 0, 5, 4 },   // -Y
				{ 2, 6, 7 }, { 2, 7, 3 },   // +Y
			};
			for (const int32_t* face : kFaces)
			{
				indices.push_back(base + face[0]);
				indices.push_back(base + face[1]);
				indices.push_back(base + face[2]);
			}
		}

		// The entity's own rendered mesh, if it has one: either straight on the entity or
		// through its ObjectComponent. Returns the triangle count appended.
		int AppendEntityMesh(Scene& scene, Entity entity, const XMMATRIX& matrix,
			std::vector<float>& vertices, std::vector<int32_t>& indices)
		{
			const MeshComponent* mesh = scene.meshes.GetComponent(entity);
			if (mesh == nullptr)
			{
				const ObjectComponent* object = scene.objects.GetComponent(entity);
				if (object != nullptr)
					mesh = scene.meshes.GetComponent(object->meshID);
			}
			if (mesh == nullptr || mesh->vertex_positions.empty() || mesh->indices.empty())
				return 0;

			const int32_t base = (int32_t)(vertices.size() / 3);
			vertices.reserve(vertices.size() + mesh->vertex_positions.size() * 3);
			for (const XMFLOAT3& position : mesh->vertex_positions)
			{
				XMFLOAT3 world;
				XMStoreFloat3(&world, XMVector3Transform(XMLoadFloat3(&position), matrix));
				vertices.push_back(world.x);
				vertices.push_back(world.y);
				vertices.push_back(world.z);
			}

			const size_t triangles = mesh->indices.size() / 3;
			indices.reserve(indices.size() + triangles * 3);
			for (size_t i = 0; i < triangles * 3; ++i)
				indices.push_back(base + (int32_t)mesh->indices[i]);
			return (int)triangles;
		}
	}

	audio::Spatializer::MaterialDesc AudioGeometryComponent::ResolveMaterial() const
	{
		if (material >= 0 && material < (int)(sizeof(kMaterialPresets) / sizeof(kMaterialPresets[0])))
			return kMaterialPresets[material];

		audio::Spatializer::MaterialDesc desc;
		desc.absorption[0] = absorptionLow;
		desc.absorption[1] = absorptionMid;
		desc.absorption[2] = absorptionHigh;
		desc.scattering = scattering;
		desc.transmission[0] = transmissionLow;
		desc.transmission[1] = transmissionMid;
		desc.transmission[2] = transmissionHigh;
		return desc;
	}

	int AudioGeometryComponent::BuildSlabs(Slab* out, int maxSlabs) const
	{
		// A zero dimension would be a degenerate triangle soup the ray tracer cannot use,
		// so every extent has a floor of a millimetre.
		const float hx = std::max(sizeX, 0.001f) * 0.5f;
		const float hy = std::max(sizeY, 0.001f) * 0.5f;
		const float hz = std::max(sizeZ, 0.001f) * 0.5f;
		const float t = std::max(thickness, 0.001f);
		const float ht = t * 0.5f;

		int count = 0;
		switch ((Shape)shape)
		{
		case Shape::Wall:
			// A panel standing in the entity's local XY plane, `thickness` deep along Z -
			// so the entity's forward axis points through the wall, the way a door's does.
			if (count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, 0, 0), XMFLOAT3(hx, hy, ht) };
			break;

		case Shape::Room:
			// The interior stays exactly size* - the slabs go OUTSIDE it - and they do not
			// overlap: the two X walls own the corners (they run the full height and depth
			// plus the thickness), the Z walls span only the interior width, and the floor
			// and ceiling only the interior footprint. Overlapping solids would make a
			// transmitted sound pay for the same wall twice.
			if (wallNegX && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(-(hx + ht), 0, 0), XMFLOAT3(ht, hy + t, hz + t) };
			if (wallPosX && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(hx + ht, 0, 0), XMFLOAT3(ht, hy + t, hz + t) };
			if (wallNegZ && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, 0, -(hz + ht)), XMFLOAT3(hx, hy + t, ht) };
			if (wallPosZ && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, 0, hz + ht), XMFLOAT3(hx, hy + t, ht) };
			if (wallFloor && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, -(hy + ht), 0), XMFLOAT3(hx, ht, hz) };
			if (wallCeiling && count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, hy + ht, 0), XMFLOAT3(hx, ht, hz) };
			break;

		case Shape::Mesh:
			break;  // no slabs: the triangles come from the entity's own mesh

		case Shape::Box:
		default:
			if (count < maxSlabs)
				out[count++] = Slab{ XMFLOAT3(0, 0, 0), XMFLOAT3(hx, hy, hz) };
			break;
		}
		return count;
	}

	bool AudioGeometryComponent::ResolveMatrix(XMFLOAT4X4& out) const
	{
		if (scene == nullptr)
			return false;
		const TransformComponent* transform = scene->transforms.GetComponent(entity);
		if (transform == nullptr)
			return false;

		out = transform->world;
		if (useTransformScale)
			return true;

		// Strip the scale but keep the rotation and the position: normalize each basis row,
		// which is exactly the scale the world matrix baked in.
		for (int row = 0; row < 3; ++row)
		{
			float* axis = &out.m[row][0];
			const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
			if (length > 1e-6f)
			{
				axis[0] /= length;
				axis[1] /= length;
				axis[2] /= length;
			}
		}
		return true;
	}

	bool AudioGeometryComponent::SettingsChanged() const
	{
		return applied_.shape != shape
			|| applied_.sizeX != sizeX
			|| applied_.sizeY != sizeY
			|| applied_.sizeZ != sizeZ
			|| applied_.thickness != thickness
			|| applied_.useTransformScale != useTransformScale
			|| applied_.walls[0] != wallNegX
			|| applied_.walls[1] != wallPosX
			|| applied_.walls[2] != wallNegZ
			|| applied_.walls[3] != wallPosZ
			|| applied_.walls[4] != wallFloor
			|| applied_.walls[5] != wallCeiling
			|| applied_.material != material
			|| applied_.absorption[0] != absorptionLow
			|| applied_.absorption[1] != absorptionMid
			|| applied_.absorption[2] != absorptionHigh
			|| applied_.scattering != scattering
			|| applied_.transmission[0] != transmissionLow
			|| applied_.transmission[1] != transmissionMid
			|| applied_.transmission[2] != transmissionHigh;
	}

	void AudioGeometryComponent::RememberSettings()
	{
		applied_.shape = shape;
		applied_.sizeX = sizeX;
		applied_.sizeY = sizeY;
		applied_.sizeZ = sizeZ;
		applied_.thickness = thickness;
		applied_.useTransformScale = useTransformScale;
		applied_.walls[0] = wallNegX;
		applied_.walls[1] = wallPosX;
		applied_.walls[2] = wallNegZ;
		applied_.walls[3] = wallPosZ;
		applied_.walls[4] = wallFloor;
		applied_.walls[5] = wallCeiling;
		applied_.material = material;
		applied_.absorption[0] = absorptionLow;
		applied_.absorption[1] = absorptionMid;
		applied_.absorption[2] = absorptionHigh;
		applied_.scattering = scattering;
		applied_.transmission[0] = transmissionLow;
		applied_.transmission[1] = transmissionMid;
		applied_.transmission[2] = transmissionHigh;
	}

	void AudioGeometryComponent::ReleaseGeometry()
	{
		if (meshHandle_ != 0)
		{
			audio::Spatializer::Get().RemoveMesh(meshHandle_);
			meshHandle_ = 0;
		}
		triangleCount_ = 0;
	}

	void AudioGeometryComponent::RegisterGeometry()
	{
		ReleaseGeometry();

		audio::Spatializer& spatializer = audio::Spatializer::Get();
		spatializerWasUp_ = spatializer.IsInitialized();
		if (!spatializerWasUp_)
			return;  // the audio engine is not up yet; Update tries again when it is

		XMFLOAT4X4 matrix;
		if (!ResolveMatrix(matrix))
			return;  // no transform: there is no place to put this geometry

		// A material is added once and its index reused. The SDK has no "remove material",
		// so re-adding one per rebuild would grow the table for as long as the level runs -
		// hence the index survives a move and is only re-added when the material itself
		// was edited (which, in the inspector, is a handful of times, not per frame).
		const audio::Spatializer::MaterialDesc desc = ResolveMaterial();
		const bool materialEdited =
			applied_.material != material
			|| applied_.absorption[0] != absorptionLow
			|| applied_.absorption[1] != absorptionMid
			|| applied_.absorption[2] != absorptionHigh
			|| applied_.scattering != scattering
			|| applied_.transmission[0] != transmissionLow
			|| applied_.transmission[1] != transmissionMid
			|| applied_.transmission[2] != transmissionHigh;
		if (materialIndex_ < 0 || materialEdited)
			materialIndex_ = spatializer.AddMaterial(desc);

		const XMMATRIX world = XMLoadFloat4x4(&matrix);
		std::vector<float> vertices;
		std::vector<int32_t> indices;

		if ((Shape)shape == Shape::Mesh)
		{
			if (scene != nullptr)
				AppendEntityMesh(*scene, entity, world, vertices, indices);
		}
		else
		{
			Slab slabs[6];
			const int slabCount = BuildSlabs(slabs, 6);
			for (int i = 0; i < slabCount; ++i)
				AppendBox(vertices, indices, slabs[i].center, slabs[i].halfExtents, world);
		}

		if (indices.empty())
		{
			// A room with every face switched off, or a Mesh shape on an entity that has
			// no mesh. Not an error - but nothing is registered, and the inspector's
			// triangle readout says so.
			RememberSettings();
			lastMatrix_ = matrix;
			return;
		}

		const int triangles = (int)(indices.size() / 3);
		const std::vector<int32_t> materialIndices((size_t)triangles,
			materialIndex_ >= 0 ? materialIndex_ : 0);

		meshHandle_ = spatializer.AddStaticMesh(vertices.data(), (int)(vertices.size() / 3),
			indices.data(), triangles, materialIndices.data());
		triangleCount_ = (meshHandle_ != 0) ? triangles : 0;

		lastMatrix_ = matrix;
		const wi::scene::RenderOrigin& origin = wi::scene::GetRenderOrigin();
		lastOriginX_ = origin.x;
		lastOriginY_ = origin.y;
		lastOriginZ_ = origin.z;
		RememberSettings();
	}

	void AudioGeometryComponent::Start()
	{
		Bind(shape, "shape");
		Bind(sizeX, "sizeX");
		Bind(sizeY, "sizeY");
		Bind(sizeZ, "sizeZ");
		Bind(thickness, "thickness");
		Bind(useTransformScale, "useTransformScale");

		Bind(wallNegX, "wallNegX");
		Bind(wallPosX, "wallPosX");
		Bind(wallNegZ, "wallNegZ");
		Bind(wallPosZ, "wallPosZ");
		Bind(wallFloor, "wallFloor");
		Bind(wallCeiling, "wallCeiling");

		Bind(material, "material");
		Bind(absorptionLow, "absorptionLow");
		Bind(absorptionMid, "absorptionMid");
		Bind(absorptionHigh, "absorptionHigh");
		Bind(scattering, "scattering");
		Bind(transmissionLow, "transmissionLow");
		Bind(transmissionMid, "transmissionMid");
		Bind(transmissionHigh, "transmissionHigh");

		Bind(staticGeometry, "staticGeometry");
		Bind(rebuildRateHz, "rebuildRateHz");
		Bind(debugDraw, "debugDraw");

		rebuildRequested_ = true;
	}

	void AudioGeometryComponent::Update(float dt)
	{
		audio::Spatializer& spatializer = audio::Spatializer::Get();

		// The audio engine may have come up (or been torn down and restarted) since the
		// last attempt, and every handle and material index went with it.
		if (spatializer.IsInitialized() != spatializerWasUp_)
		{
			if (!spatializer.IsInitialized())
			{
				meshHandle_ = 0;      // the spatializer released it with the scene
				triangleCount_ = 0;
			}
			materialIndex_ = -1;
			rebuildRequested_ = true;
		}

		if (SettingsChanged())
			rebuildRequested_ = true;

		// A large-world rebase moves every origin-relative coordinate, so baked geometry has
		// to follow it whether or not the wall itself counts as static.
		const wi::scene::RenderOrigin& origin = wi::scene::GetRenderOrigin();
		if (meshHandle_ != 0 &&
			(origin.x != lastOriginX_ || origin.y != lastOriginY_ || origin.z != lastOriginZ_))
		{
			rebuildRequested_ = true;
		}

		if (!staticGeometry && !rebuildRequested_)
		{
			// Moving geometry: re-registering is a scene commit, so it is worth doing at a
			// door's speed rather than the frame rate.
			rebuildTimer_ += dt;
			const float period = rebuildRateHz > 0.0f ? 1.0f / rebuildRateHz : 0.0f;
			if (rebuildTimer_ >= period)
			{
				rebuildTimer_ = 0.0f;
				XMFLOAT4X4 matrix;
				if (ResolveMatrix(matrix) && !MatrixNearlyEqual(matrix, lastMatrix_))
					rebuildRequested_ = true;
			}
		}

		if (rebuildRequested_)
		{
			rebuildRequested_ = false;
			// Spatializer's geometry calls take their own lock, so this is safe from the
			// parallel Update - and the commit that makes it visible to the ray tracer
			// happens once per frame in AudioEngine::Update, not here.
			RegisterGeometry();
		}

		if (debugDraw)
			DrawOutline();
	}

	void AudioGeometryComponent::DrawOutline()
	{
		XMFLOAT4X4 matrix;
		if (!ResolveMatrix(matrix))
			return;

		const XMMATRIX world = XMLoadFloat4x4(&matrix);
		// Green once the simulator has the triangles, amber while it does not - the two
		// states a wall being placed needs to tell apart at a glance.
		const XMFLOAT4 color = (meshHandle_ != 0)
			? XMFLOAT4(0.2f, 0.9f, 0.4f, 1.0f)
			: XMFLOAT4(1.0f, 0.7f, 0.2f, 1.0f);

		// wi::renderer's debug cube spans -1..1, so a box matrix scales by the HALF extents.
		auto boxMatrix = [&world](const XMFLOAT3& center, const XMFLOAT3& halfExtents) {
			XMFLOAT4X4 result;
			XMStoreFloat4x4(&result,
				XMMatrixScaling(halfExtents.x, halfExtents.y, halfExtents.z) *
				XMMatrixTranslation(center.x, center.y, center.z) *
				world);
			return result;
		};

		// Built here, drawn on the main thread: the renderer's debug list is a plain global
		// with no lock, and this runs in the parallel Update. XMFLOAT4X4 rather than XMMATRIX
		// in the capture on purpose - a 16-byte-aligned type inside a std::function is not
		// guaranteed to stay aligned.
		std::vector<XMFLOAT4X4> boxes;
		if ((Shape)shape == Shape::Mesh)
		{
			// The entity's own mesh: outline what it actually occupies rather than a unit
			// cube around its origin.
			const MeshComponent* mesh = (scene != nullptr) ? scene->meshes.GetComponent(entity) : nullptr;
			if (mesh == nullptr && scene != nullptr)
			{
				if (const ObjectComponent* object = scene->objects.GetComponent(entity))
					mesh = scene->meshes.GetComponent(object->meshID);
			}
			if (mesh != nullptr)
				boxes.push_back(boxMatrix(mesh->aabb.getCenter(), mesh->aabb.getHalfWidth()));
		}
		else
		{
			Slab slabs[6];
			const int slabCount = BuildSlabs(slabs, 6);
			for (int i = 0; i < slabCount; ++i)
				boxes.push_back(boxMatrix(slabs[i].center, slabs[i].halfExtents));
		}
		if (boxes.empty())
			return;

		RunOnMainThread([boxes, color] {
			for (const XMFLOAT4X4& box : boxes)
				wi::renderer::DrawBox(box, color);
		});
	}

	void AudioGeometryComponent::OnEnable()
	{
		rebuildRequested_ = true;
	}

	void AudioGeometryComponent::OnDisable()
	{
		// A disabled wall stops blocking sound. That is the whole point of disabling it.
		ReleaseGeometry();
	}

	void AudioGeometryComponent::Destroy()
	{
		ReleaseGeometry();
	}

	void AudioGeometryComponent::DescribeParams(wi::vector<NativeParam>& out)
	{
		static const char* kShape = "Shape";
		static const char* kWalls = "Room: which faces exist";
		static const char* kMaterial = "Acoustic material";
		static const char* kRebuild = "Rebuild";
		static const char* kDebug = "Debug";

		out.push_back(NativeParam::Enum("shape", &shape,
			"Box (solid)\0Room (hollow)\0Wall (slab)\0Mesh (this entity's)\0",
			"Room builds walls around an interior of the size below; Mesh hands the ray tracer "
			"this entity's own rendered geometry.", kShape));
		out.push_back(NativeParam::Float("sizeX", &sizeX, 0.01f, 500.0f,
			"Metres. For a Room this is the INTERIOR - the walls go outside it.", kShape));
		out.push_back(NativeParam::Float("sizeY", &sizeY, 0.01f, 500.0f, nullptr, kShape));
		out.push_back(NativeParam::Float("sizeZ", &sizeZ, 0.01f, 500.0f, nullptr, kShape));
		out.push_back(NativeParam::Float("thickness", &thickness, 0.001f, 10.0f,
			"Slab depth for Room and Wall. Transmission is computed through the material, so a "
			"thin wall leaks and a thick one does not.", kShape));
		out.push_back(NativeParam::Bool("useTransformScale", &useTransformScale,
			"Multiply the sizes by the entity's scale. Off leaves the transform to position and "
			"rotate only.", kShape));

		out.push_back(NativeParam::Bool("wallNegX", &wallNegX, nullptr, kWalls));
		out.push_back(NativeParam::Bool("wallPosX", &wallPosX, nullptr, kWalls));
		out.push_back(NativeParam::Bool("wallNegZ", &wallNegZ, nullptr, kWalls));
		out.push_back(NativeParam::Bool("wallPosZ", &wallPosZ, nullptr, kWalls));
		out.push_back(NativeParam::Bool("wallFloor", &wallFloor, "The -Y face.", kWalls));
		out.push_back(NativeParam::Bool("wallCeiling", &wallCeiling,
			"The +Y face. Off is an open courtyard - no ceiling to reflect off.", kWalls));

		out.push_back(NativeParam::Enum("material", &material,
			"Generic\0Brick\0Concrete\0Ceramic\0Gravel\0Carpet\0Glass\0Plaster\0Wood\0Metal\0Rock\0Custom\0",
			"Steam Audio's material table. Absorption is what the reverb tail loses to the "
			"surface, transmission is what a sound going through the wall keeps.", kMaterial));
		out.push_back(NativeParam::Float("absorptionLow", &absorptionLow, 0.0f, 1.0f,
			"Custom only. 0 = perfectly reflective, 1 = dead.", kMaterial));
		out.push_back(NativeParam::Float("absorptionMid", &absorptionMid, 0.0f, 1.0f, nullptr, kMaterial));
		out.push_back(NativeParam::Float("absorptionHigh", &absorptionHigh, 0.0f, 1.0f, nullptr, kMaterial));
		out.push_back(NativeParam::Float("scattering", &scattering, 0.0f, 1.0f,
			"Custom only. 0 = mirror, 1 = fully diffuse.", kMaterial));
		out.push_back(NativeParam::Float("transmissionLow", &transmissionLow, 0.0f, 1.0f,
			"Custom only. Fraction of each band that passes through the material.", kMaterial));
		out.push_back(NativeParam::Float("transmissionMid", &transmissionMid, 0.0f, 1.0f, nullptr, kMaterial));
		out.push_back(NativeParam::Float("transmissionHigh", &transmissionHigh, 0.0f, 1.0f, nullptr, kMaterial));

		out.push_back(NativeParam::Bool("staticGeometry", &staticGeometry,
			"Registered once and never re-read. Turn it off only for geometry that actually "
			"moves - a door, a lift - because every rebuild re-commits the ray tracer's scene.",
			kRebuild));
		out.push_back(NativeParam::Float("rebuildRateHz", &rebuildRateHz, 1.0f, 60.0f,
			"Moving geometry only: how often the transform is checked and the mesh re-registered.",
			kRebuild));
		out.push_back(NativeParam::Action("Rebuild now", [](NativeComponent& self) {
			static_cast<AudioGeometryComponent&>(self).Rebuild();
		}, "Re-register the geometry from the current transform and settings.", kRebuild));
		out.push_back(NativeParam::Readout("triangles", [](NativeComponent& self) {
			return (float)static_cast<AudioGeometryComponent&>(self).GetTriangleCount();
		}, "%.0f in the simulator", false,
			"Zero means nothing is registered: no Steam Audio in this build, no transform, an "
			"empty mesh, or a room with every face switched off.", kRebuild));

		out.push_back(NativeParam::Bool("debugDraw", &debugDraw,
			"Outline the slabs in the world. Green once the simulator has them.", kDebug));
	}
}

// registration
// Same explicit-call reasoning as RegisterAudioComponents (Engine/ is a static library, so
// a static initializer in a translation unit nothing references never runs); this one is
// called from there.
namespace st
{
	void RegisterAudioGeometryComponents()
	{
		using namespace wi::scene;

		static bool registered = false;
		if (registered)
			return;
		registered = true;

		// Three names, one class: the name only picks the default shape, so a wall that
		// should have been a room is a dropdown away rather than a detach and re-attach.
		RegisterNativeComponent("stAudioGeometry",
			[] { return std::unique_ptr<NativeComponent>(new AudioGeometryComponent()); },
			GetNativeTypeID<AudioGeometryComponent>(), "Framework", "ST", "Audio");

		RegisterNativeComponent("stAudioWall",
			[] {
				auto component = std::make_unique<AudioGeometryComponent>();
				component->shape = (int)AudioGeometryComponent::Shape::Wall;
				component->sizeX = 4.0f;
				component->sizeY = 3.0f;
				component->sizeZ = 0.2f;
				return std::unique_ptr<NativeComponent>(component.release());
			},
			GetNativeTypeID<AudioGeometryComponent>(), "Framework", "ST", "Audio");

		RegisterNativeComponent("stAudioRoom",
			[] {
				auto component = std::make_unique<AudioGeometryComponent>();
				component->shape = (int)AudioGeometryComponent::Shape::Room;
				component->sizeX = 8.0f;
				component->sizeY = 3.0f;
				component->sizeZ = 6.0f;
				return std::unique_ptr<NativeComponent>(component.release());
			},
			GetNativeTypeID<AudioGeometryComponent>(), "Framework", "ST", "Audio");
	}
}
