#pragma once
// Simtary extension: world-space position precision switch.
//
// The engine renders in float and always will - GPUs, the shader interop structs and
// the whole XMFLOAT3 scene graph are 32-bit. What this switch controls is the CPU-side
// *absolute* position of a root transform, and therefore how far from the world origin
// the game can go before float mantissa starts eating centimetres.
//
//   SIMTARY_WORLD_FLOAT64 defined   (CMake -DSIMTARY_LARGE_WORLD=ON, the default)
//       Absolute positions are double. A 100x100 km world keeps sub-micrometre
//       precision at its far corner. Costs 12 extra bytes per TransformComponent and
//       one double subtract per transform update.
//
//   SIMTARY_WORLD_FLOAT64 not defined  (-DSIMTARY_LARGE_WORLD=OFF)
//       Absolute positions are float, same as stock Wicked. Smaller components, no
//       fp64 anywhere in the transform system. Useful for small-world projects and
//       required in practice on mobile GPUs/CPUs, where doubles are emulated or
//       heavily rate-limited.
//
// The public API does not change shape between the two: setters take double and narrow,
// getters return double and widen. Game code compiles unmodified against either.
//
// Rendering stays float-safe in both modes through the *render origin*: the transform
// system subtracts a double-precision origin (normally the camera's world position)
// from each root transform's absolute position, and stores the small difference in
// TransformComponent::translation_local. Everything downstream of that - matrices,
// bounds, shaders - sees ordinary camera-local floats.
//
// Large-world positions are opt-in per transform, through TransformComponent::LARGE_WORLD.
// A transform picks the flag up the first time it is handed an absolute position, and only
// a flagged transform is ever rebased. Anything that drives translation_local directly
// wi::gui widgets do exactly that - therefore keeps stock Wicked behaviour instead of
// being dragged back to the origin behind its own back.

namespace wi::scene
{
#ifdef SIMTARY_WORLD_FLOAT64
	using world_float = double;
	inline constexpr bool WORLD_IS_FLOAT64 = true;
#else
	using world_float = float;
	inline constexpr bool WORLD_IS_FLOAT64 = false;
#endif // SIMTARY_WORLD_FLOAT64

	// The point in absolute world space that float 0,0,0 currently means.
	//
	// Set this once per frame, before the transform update system runs, to the position
	// the camera is at. Root transforms are then rebased around it, which is what keeps
	// float precision usable no matter how far the player has walked. Leaving it at the
	// default (0,0,0) reproduces stock absolute-float behaviour exactly.
	//
	// Read concurrently by transform update jobs, so only write it while no scene update
	// is in flight.
	struct RenderOrigin
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	void SetRenderOrigin(double x, double y, double z);
	void SetRenderOrigin(const RenderOrigin& origin);
	const RenderOrigin& GetRenderOrigin();

	// Have RenderPath3D::Update drive the render origin from the active camera's absolute
	// position every frame. OFF by default, and deliberately so: moving the origin moves
	// the float space that everything outside the transform system is expressed in, and
	// any subsystem holding its own copy of a position has to be rebased in step or it
	// will appear to teleport once per frame. In this engine that means at least the Jolt
	// physics bodies, the terrain's generated chunks and anything a game keeps in a
	// component of its own.
	//
	// Until those are handled, leave this off and the origin at (0,0,0): large-world
	// positions are still stored and serialized in full precision, they are simply
	// rendered from absolute float, which is exactly the stock behaviour.
	void SetRenderOriginFollowsCamera(bool value);
	bool GetRenderOriginFollowsCamera();
}
