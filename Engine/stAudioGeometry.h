#pragma once
// stAudioGeometry: the walls a sound has to get through
//
// Steam Audio's occlusion, transmission, reflections and pathing all ask the same
// question - what geometry is between these two points - and until something hands
// the simulator triangles the answer is always "nothing". A room full of emitters
// with occlusion switched on still sounds like an open field. This component is how
// geometry gets there, from the editor, with no code:
//
//	"stAudioRoom"      a hollow box: four walls, a floor and a ceiling, each one a
//	                   real slab with thickness, sized from the entity's transform.
//	                   Any of the six can be switched off to make a doorway, an open
//	                   roof, a corridor.
//	"stAudioWall"      one slab. A wall, a door, a partition, a sheet of glass.
//	"stAudioGeometry"  the same component with no shape opinion: a solid Box, or
//	                   Mesh, which hands the simulator the entity's OWN rendered mesh
//	                   so complicated geometry does not have to be approximated.
//
// All three are the same class; the name only picks the default shape, so switching a
// wall into a room is a dropdown rather than a re-attach.
//
//	NCI_0                  = "stAudioRoom"
//	NCA_0_sizeX            = 8.0        (metres of INTERIOR, before the entity's scale)
//	NCA_0_sizeY            = 3.0
//	NCA_0_sizeZ            = 5.0
//	NCA_0_thickness        = 0.25
//	NCA_0_material         = 2          (concrete)
//	NCA_0_wallPosZ         = false      (that side is a doorway)
//
// Material is Steam Audio's own table - per-band absorption, scattering and
// transmission - as presets plus Custom. Absorption is what the reverb tail hears,
// transmission is what a sound going THROUGH the wall keeps: concrete kills the highs
// and passes a muffled thud, glass passes far more of everything.
//
// Cost: geometry is static in the ray tracer's sense. A moving wall means removing and
// re-adding the mesh and re-committing the scene, so `staticGeometry` (the default)
// registers once at Start and never looks at the transform again. Turn it off for a
// door that swings and the rebuild is rate-limited to `rebuildRateHz`.
//
// Without a Steam Audio build (SIMTARY_HAS_STEAMAUDIO undefined) the component still
// attaches, still draws its outline and simply registers nothing - the same deal the
// rest of the spatial layer makes.

#include "stNativeComponent.h"
#include "stAudioSpatial.h"

#include <cstdint>

namespace st
{
	// Register stAudioGeometry / stAudioWall / stAudioRoom with the native component
	// registry. Called from RegisterAudioComponents(), for the linker reason spelled out
	// there: Engine/ is a static library, so a translation unit nothing references is
	// dropped along with its static initializers. Idempotent.
	void RegisterAudioGeometryComponents();

	struct AudioGeometryComponent : wi::scene::NativeComponent
	{
		enum class Shape : int
		{
			Box = 0,   // one solid block - a pillar, a crate, a lump of terrain
			Room,      // hollow box: up to six slabs around an interior of size*
			Wall,      // one slab, `thickness` deep along the entity's local Z
			Mesh,      // the entity's own MeshComponent, triangle for triangle
		};

		// Steam Audio's published material table, plus Custom for the fields below.
		enum class Preset : int
		{
			Generic = 0, Brick, Concrete, Ceramic, Gravel, Carpet, Glass, Plaster,
			Wood, Metal, Rock, Custom,
		};

		// shape
		int   shape = (int)Shape::Box;
		// Metres. For Room this is the INTERIOR - the walls go outside it - so a room
		// authored as 8 x 3 x 5 is 8 x 3 x 5 to stand in whatever the wall thickness is.
		float sizeX = 4.0f;
		float sizeY = 3.0f;
		float sizeZ = 4.0f;
		// Slab depth for Room and Wall. Thickness is not cosmetic: transmission is
		// computed over the path INSIDE the material, so a zero-thickness wall transmits
		// everything and a 2 m one is a bunker.
		float thickness = 0.2f;
		// Multiply the sizes by the entity's own scale. Off means the numbers above are
		// the whole story and the transform only positions and rotates the geometry.
		bool  useTransformScale = true;

		// Room only: which of the six faces exist. Dropping one is how a room gets a
		// doorway, an open roof, or becomes an L-shape built from two overlapping rooms.
		bool  wallNegX = true;
		bool  wallPosX = true;
		bool  wallNegZ = true;
		bool  wallPosZ = true;
		bool  wallFloor = true;    // -Y
		bool  wallCeiling = true;  // +Y

		// material
		int   material = (int)Preset::Generic;
		// Custom only. Per band: low / mid / high.
		float absorptionLow = 0.10f;
		float absorptionMid = 0.20f;
		float absorptionHigh = 0.30f;
		float scattering = 0.05f;        // 0 = mirror-like, 1 = fully diffuse
		float transmissionLow = 0.100f;
		float transmissionMid = 0.050f;
		float transmissionHigh = 0.030f;

		// rebuild policy
		// Registered once at Start and never re-read. Correct for anything that does not
		// move, which is nearly all acoustic geometry, and free.
		bool  staticGeometry = true;
		// Non-static only: the most often the mesh is torn down and re-registered when the
		// transform changes. Every rebuild re-commits the ray tracer's scene.
		float rebuildRateHz = 10.0f;

		// debug
		bool  debugDraw = false;

		// access
		// Triangles this component has in the simulator right now (0 when it registered
		// nothing - no Steam Audio, an empty mesh, a room with every face switched off).
		int  GetTriangleCount() const { return triangleCount_; }
		bool IsRegistered() const { return meshHandle_ != 0; }
		// Re-read the shape, material and transform and re-register the geometry. Called
		// automatically; call it by hand after moving a static wall from code.
		void Rebuild() { rebuildRequested_ = true; }

		void Start() override;
		void Update(float dt) override;
		void OnEnable() override;
		void OnDisable() override;
		void Destroy() override;
		void DescribeParams(wi::vector<NativeParam>& out) override;

	private:
		// One box in the entity's local space: everything but Mesh is a list of these.
		struct Slab
		{
			XMFLOAT3 center = XMFLOAT3(0, 0, 0);
			XMFLOAT3 halfExtents = XMFLOAT3(1, 1, 1);
		};

		void ReleaseGeometry();
		void RegisterGeometry();
		// Outline the slabs (or the mesh's bounds) in the world. Queues the actual draw onto
		// the main thread, so it is safe to call from the parallel Update.
		void DrawOutline();
		// The current shape as slabs. Empty for Mesh, and for a room with no faces left.
		int  BuildSlabs(Slab* out, int maxSlabs) const;
		// The material this component wants, resolved from the preset or the custom fields.
		audio::Spatializer::MaterialDesc ResolveMaterial() const;
		// World matrix to bake into the vertices, with the entity's scale stripped out
		// when useTransformScale is off. False when the entity has no transform.
		bool ResolveMatrix(XMFLOAT4X4& out) const;
		// Has anything that changes the triangles been edited since the last register?
		bool SettingsChanged() const;
		void RememberSettings();

		uint32_t meshHandle_ = 0;
		int      materialIndex_ = -1;
		int      triangleCount_ = 0;
		bool     rebuildRequested_ = true;
		// Whether the spatializer was up at the last attempt. A component that attached
		// before the audio engine finished starting has to try again, not sit inert.
		bool     spatializerWasUp_ = false;
		float    rebuildTimer_ = 0.0f;
		XMFLOAT4X4 lastMatrix_ = wi::math::IDENTITY_MATRIX;
		// The render origin the vertices were baked around. Geometry is handed over in
		// origin-relative space like everything else audio, so a large-world rebase moves
		// the whole world out from under a registered mesh - and that has to re-register
		// even when the wall itself is static and has not moved a millimetre.
		double   lastOriginX_ = 0.0;
		double   lastOriginY_ = 0.0;
		double   lastOriginZ_ = 0.0;

		// A copy of every field the triangles depend on, taken when they were last built.
		struct AppliedSettings
		{
			int   shape = -1;
			float sizeX = 0, sizeY = 0, sizeZ = 0, thickness = 0;
			bool  useTransformScale = true;
			bool  walls[6] = { true, true, true, true, true, true };
			int   material = -1;
			float absorption[3] = { 0, 0, 0 };
			float scattering = 0;
			float transmission[3] = { 0, 0, 0 };
		};
		AppliedSettings applied_;
	};
}
