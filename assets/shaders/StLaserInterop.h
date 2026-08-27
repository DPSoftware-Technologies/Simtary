#ifndef ST_SHADERINTEROP_LASER_H
#define ST_SHADERINTEROP_LASER_H

// Shared layout for st::LaserSystem (Framework/render/Laser.cpp) and StLaserCS.hlsl.
// Included from both sides, exactly like StProjectorInterop.h next to it - the
// include path differs per language:
//   C++  : Simtary/assets/shaders is on the app's include path
//   HLSL : -I assets/shaders is passed by simtary_compile_shader(... ENGINE_ENV)
#ifdef __cplusplus
#include "shaders/ShaderInterop.h"
#else
#include "ShaderInterop.h"
#endif

// A laser is uploaded as a flat list of SEGMENTS, not as one emitter: a beam that
// bounces off two mirrors and through a lens is four straight legs, and the shader
// has no reason to know which laser they came from. The optics live on the CPU
// (Framework/render/Optics.h), which is also where the geometry raycast that ends
// the beam happens.
//
// Both ceilings are per-pixel loop bounds, so they are cost limits rather than memory
// ones. They are this high to leave room for array projection - one laser in array
// mode is an 8x8 grid of rays, and each ray is at least one segment - and they are
// only affordable because both loops reject on a squared distance before they reach
// any transcendental.
static const uint ST_LASER_SEGMENT_MAX = 128;

// Impact points, including the persistence trail. A laser sweeping across a wall
// leaves one of these behind every `trailSpacing` metres and they fade out over
// `trailLife` seconds - which is what makes a moving beam draw a picture instead of
// a single dot that vanishes the moment it moves.
static const uint ST_LASER_DOT_MAX = 256;

static const uint ST_LASER_FLAG_CORE = 1u << 0;      // the thin bright filament
static const uint ST_LASER_FLAG_GLOW = 1u << 1;      // the wide soft halo around it
static const uint ST_LASER_FLAG_OCCLUDED = 1u << 2;  // hidden where scene geometry is nearer

struct StLaserSegment
{
	float3 start;
	float core_radius; // metres; the filament. Sub-centimetre is normal.

	float3 end;
	float glow_radius; // metres; the halo. Typically 10-40x the core.

	float3 color;
	float intensity; // core brightness, per metre of beam crossed

	float glow_intensity;
	float attenuation; // 0 = even along the leg, 1 = fades out towards `end`
	float pad0;
	uint flags;
};

struct StLaserDot
{
	float3 position;
	float radius; // spherical glow in the air at the impact point

	float3 color;
	float intensity;

	float3 normal;        // surface normal at the impact, for the splash
	float surface_radius; // how far the burn mark spreads across the surface, metres

	float surface_intensity;
	float pad0;
	float pad1;
	float pad2;
};

#ifdef __cplusplus
static_assert(sizeof(StLaserSegment) == 64, "StLaserSegment must stay 16-byte-row aligned for the shader");
static_assert(sizeof(StLaserDot) == 64, "StLaserDot must stay 16-byte-row aligned for the shader");
#endif // __cplusplus

#endif // ST_SHADERINTEROP_LASER_H
