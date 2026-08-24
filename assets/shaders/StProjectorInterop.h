#ifndef ST_SHADERINTEROP_PROJECTOR_H
#define ST_SHADERINTEROP_PROJECTOR_H

// Shared layout for st::ProjectorSystem (Framework/render/Projector.cpp) and
// StProjectorCS.hlsl. Included from both sides, exactly like the engine's own
// ShaderInterop_*.h headers - the include path differs per language:
//   C++  : Simtary/assets/shaders is on the app's include path, Engine/ carries shaders/ShaderInterop.h
//   HLSL : this file is staged next to the engine shader sources in <exe>/shaders/
#ifdef __cplusplus
#include "shaders/ShaderInterop.h"
#else
#include "ShaderInterop.h"
#endif

// Hard ceiling on projectors uploaded per frame. The pass loops over them per
// pixel, so this is a cost limit, not a memory one.
static const uint ST_PROJECTOR_MAX = 8;

// Outline of the projected image, evaluated in the projector's own NDC where the
// image spans [-1,1] on both axes (the aspect ratio lives in the matrix).
static const uint ST_PROJECTOR_SHAPE_RECT = 0;    // square / rectangle - a real projector gate
static const uint ST_PROJECTOR_SHAPE_ELLIPSE = 1; // circle / ellipse - a spot light cone
static const uint ST_PROJECTOR_SHAPE_ROUNDED = 2; // rectangle with rounded corners

static const uint ST_PROJECTOR_FLAG_LIGHT_SURFACES = 1u << 0; // paint the image onto geometry
static const uint ST_PROJECTOR_FLAG_BEAM = 1u << 1;           // volumetric shaft through the air
static const uint ST_PROJECTOR_FLAG_OCCLUSION = 1u << 2;      // screen-space shadowing
static const uint ST_PROJECTOR_FLAG_LAMBERT = 1u << 3;        // modulate by N.L

struct StProjector
{
	// World -> projector clip. Stored as the four ROWS of the DirectXMath matrix, so
	// the shader must use row-vector order: mul(float4(P,1), vp). Loading a float4x4
	// out of a ByteAddressBuffer would depend on the compiler's majorness default;
	// four explicit float4s do not.
	float4 vp0;
	float4 vp1;
	float4 vp2;
	float4 vp3;

	float3 position;
	float range;

	float3 direction;
	float intensity;

	float3 color;
	float gamma;

	uint flags;
	uint shape;
	uint texture_index; // bindless SRV index of the projected image, 0 = untextured (white)
	float softness;     // edge feather, in NDC units inward from the gate

	float2 shift;    // lens shift, in image half-widths
	float2 keystone; // trapezoid correction, per axis

	float corner_radius; // ST_PROJECTOR_SHAPE_ROUNDED only, 0..1 of the half-extent
	float vignette;      // 0 = flat, 1 = fully dark at the image corners
	float distortion;    // + barrel, - pincushion
	float falloff;       // 0 = brightness independent of throw, 1 = inverse-square

	float focus_distance;  // throw distance at which falloff leaves brightness untouched
	float beam_density;    // scattering per metre of air
	float beam_anisotropy; // Henyey-Greenstein g: 0 isotropic, ->1 forward scattering
	uint beam_samples;

	float occlusion_strength;
	uint occlusion_samples;
	float occlusion_thickness; // how deep behind a surface a blocker still counts, metres
	float pad;
};

#ifdef __cplusplus
static_assert(sizeof(StProjector) == 192, "StProjector must stay 16-byte-row aligned for the shader");
#endif // __cplusplus

#endif // ST_SHADERINTEROP_PROJECTOR_H
