#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"
#include "StLaserInterop.h"

// Laser beams, the mirrors and lenses they bounce through, and the impact points
// they leave behind.
//
// Unlike the projector pass next door, nothing here is ray marched. A laser is a
// cylinder with a radial brightness profile, and the integral of that profile along
// the view ray has a closed form - so one atan pair per segment gives the exact
// answer with no samples, no dither and no banding. That matters more for a laser
// than for a projector: the beam is a few millimetres across, and any march coarse
// enough to be affordable steps straight over it.
//
// The profile is the Lorentzian I(r) = R^2 / (R^2 + r^2). Along the view ray
// O + tD the squared distance to the beam's infinite line is a quadratic
//
//     r^2(t) = k t^2 + 2 m t + C
//
// so the integral over [ta, tb] is
//
//     R^2 / sqrt(k (R^2 + d^2)) * [ atan((k t + m) / sqrt(k (R^2 + d^2))) ]
//
// with d the closest-approach distance. The segment (rather than the line) is
// handled exactly too: position along the beam is affine in t, so clipping the
// beam's own extent is another interval clip on t - which is what gives each leg the
// flat ends a capsule test would round off. Legs are what a bounce produces, and the
// shader never has to know which laser or which mirror they came from.
//
// params0.x  bindless index of the StLaserSegment buffer
// params0.y  number of segments in it
// params0.z  bindless index of the StLaserDot buffer
// params0.w  number of dots in it

PUSHCONSTANT(postprocess, PostProcess);

Texture2D<float4> input : register(t0);

RWTexture2D<float4> output : register(u0);

inline StLaserSegment load_segment(uint buffer_index, uint i)
{
	return bindless_buffers[descriptor_index(buffer_index)].Load<StLaserSegment>(i * sizeof(StLaserSegment));
}

inline StLaserDot load_dot(uint buffer_index, uint i)
{
	return bindless_buffers[descriptor_index(buffer_index)].Load<StLaserDot>(i * sizeof(StLaserDot));
}

// Integral of R^2 / (R^2 + r^2(t)) dt over [ta, tb], where r^2(t) = k t^2 + 2 m t + C.
// k is 1 - (D.U)^2 for a line and exactly 1 for a point.
inline float lorentzian_integral(float R, float k, float m, float C, float ta, float tb)
{
	if (tb <= ta)
		return 0;

	const float R2 = R * R;

	// k -> 0 means the view ray runs parallel to the beam: r is then constant along
	// it and the atan form degenerates (the closest-approach parameter runs off to
	// infinity). This is the limit of the expression below, evaluated directly - it
	// is the case where you are sighting down the beam, so getting it wrong is very
	// visible.
	if (k < 1e-4)
	{
		return R2 / (R2 + max(C, 0.0)) * (tb - ta);
	}

	// Closest approach sits at t_c = -m/k, where r^2 = C - m^2/k.
	const float d2 = max(C - m * m / k, 0.0);
	const float scale = sqrt(k * (R2 + d2));

	// (t - t_c) / A, with A = sqrt((R^2 + d^2) / k), rearranged so that t_c never has
	// to be formed on its own.
	const float arg_a = (k * ta + m) / scale;
	const float arg_b = (k * tb + m) / scale;

	return (R2 / scale) * (atan(arg_b) - atan(arg_a));
}

// The stretch of the view ray that can see this segment: in front of the camera, not
// past whatever the camera already drew, and between the beam's own two ends.
// Returns false when nothing is left.
inline bool laser_span(in StLaserSegment segment, in float3 O, in float3 D, in float ray_length,
	out float k, out float m, out float C, out float ta, out float tb)
{
	k = 0;
	m = 0;
	C = 0;
	ta = 0;
	tb = 0;

	const float3 axis = segment.end - segment.start;
	const float extent = length(axis);
	if (extent < 1e-5)
		return false;

	const float3 U = axis / extent;
	const float3 W = O - segment.start;

	const float b = dot(D, U);
	const float e = dot(U, W);

	k = max(1.0 - b * b, 0.0);
	m = dot(D, W) - e * b;
	C = dot(W, W) - e * e;

	ta = 0;
	// A beam is only hidden by geometry the camera can see. Where the camera drew
	// nothing (sky) ray_length is the far plane and the beam runs its full course.
	tb = (segment.flags & ST_LASER_FLAG_OCCLUDED) ? ray_length : 1e16;

	// Position along the beam at view-ray parameter t is s(t) = e + t*b, which is
	// affine - so "inside the segment" is an exact interval in t, not a fit.
	if (abs(b) > 1e-5)
	{
		const float t0 = -e / b;
		const float t1 = (extent - e) / b;
		ta = max(ta, min(t0, t1));
		tb = min(tb, max(t0, t1));
	}
	else if (e < 0 || e > extent)
	{
		return false; // ray crosses the beam's axis entirely past one of its ends
	}

	return tb > ta;
}

inline float3 laser_segment_light(in StLaserSegment segment, in float3 O, in float3 D, in float ray_length)
{
	float k, m, C, ta, tb;
	if (!laser_span(segment, O, D, ray_length, k, m, C, ta, tb))
		return 0;

	// Radial reject before any atan. The Lorentzian never truly reaches zero, so
	// without this every segment costs two transcendentals at every pixel on screen -
	// affordable for one laser, ruinous for an array projecting sixty-four of them.
	// Six glow radii is far enough out that the tail is below a bit of colour.
	const float reach = max(segment.glow_radius, segment.core_radius) * 6.0;
	const float d2 = (k < 1e-4) ? max(C, 0.0) : max(C - m * m / k, 0.0);
	if (d2 > reach * reach)
		return 0;

	float weight = 0;

	if (segment.flags & ST_LASER_FLAG_CORE)
	{
		weight += segment.intensity * lorentzian_integral(max(segment.core_radius, 1e-4), k, m, C, ta, tb);
	}
	if (segment.flags & ST_LASER_FLAG_GLOW)
	{
		weight += segment.glow_intensity * lorentzian_integral(max(segment.glow_radius, 1e-4), k, m, C, ta, tb);
	}

	if (weight <= 0)
		return 0;

	// Fade towards the far end of the leg. Measured at the midpoint of the visible
	// stretch, which is where the light this pixel receives is centred.
	[branch]
	if (segment.attenuation > 0)
	{
		const float3 axis = segment.end - segment.start;
		const float extent2 = max(dot(axis, axis), 1e-8);
		const float3 mid = O + D * ((ta + tb) * 0.5) - segment.start;
		const float s = saturate(dot(mid, axis) / extent2);
		weight *= lerp(1.0, 1.0 - s, saturate(segment.attenuation));
	}

	return segment.color * weight;
}

// The glow hanging in the air at an impact point. Same integral, with the beam's
// line replaced by a single point: r^2(t) = t^2 - 2 t (D.P) + |P|^2, so k is 1.
inline float3 laser_dot_air(in StLaserDot spot, in float3 O, in float3 D, in float ray_length)
{
	if (spot.intensity <= 0 || spot.radius <= 1e-5)
		return 0;

	const float3 W = O - spot.position;
	const float m = dot(D, W);
	const float C = dot(W, W);

	const float reach = spot.radius * 6.0;

	// The closest approach sits at t = -m. Behind the camera by more than the glow
	// reaches means nothing of it is in front - without this test a spot behind you
	// integrates over the stretch of ray in FRONT of you and glows in the wrong place.
	if (m > reach)
		return 0;

	// Cheap reject before the atan: closest approach is further out than the glow
	// reaches, so this pixel sees nothing of it. With a persistence trail there are
	// dozens of these per frame and almost all of them fail here.
	const float d2 = max(C - m * m, 0.0);
	if (d2 > reach * reach)
		return 0;

	const float tb = min(ray_length, max(0.0, -m) + reach);

	return spot.color * spot.intensity * lorentzian_integral(spot.radius, 1.0, m, C, 0.0, tb);
}

// The mark the beam leaves on the surface it landed on. Screen space: the pixel
// already knows the world position the camera drew there, so this is a plain
// distance test against the recorded impact - no projection and no shadow map.
inline float3 laser_dot_surface(in StLaserDot spot, in float3 P, in float3 N)
{
	if (spot.surface_intensity <= 0 || spot.surface_radius <= 1e-5)
		return 0;

	const float3 diff = P - spot.position;
	const float r2 = dot(diff, diff);
	const float radius2 = spot.surface_radius * spot.surface_radius;
	if (r2 > radius2 * 9.0)
		return 0;

	// Keep the mark on the face the beam actually struck. Without this, a dot on one
	// side of a thin wall bleeds through to the other.
	const float facing = saturate(dot(N, spot.normal) * 0.5 + 0.5);
	if (facing < 0.05)
		return 0;

	return spot.color * spot.surface_intensity * exp(-r2 / radius2) * facing;
}

// Normal from the depth buffer - the GBuffer normal texture is not guaranteed to
// still be bound this late in the frame. Same reconstruction the projector pass uses.
inline float3 normal_from_depth(in float2 uv, in float3 P, in float3 V)
{
	const float2 texel = postprocess.resolution_rcp;

	const float zx = texture_depth.SampleLevel(sampler_point_clamp, uv + float2(texel.x, 0), 0);
	const float zy = texture_depth.SampleLevel(sampler_point_clamp, uv + float2(0, texel.y), 0);

	const float3 Px = reconstruct_position(uv + float2(texel.x, 0), zx);
	const float3 Py = reconstruct_position(uv + float2(0, texel.y), zy);

	float3 N = cross(Py - P, Px - P);
	const float len = length(N);
	if (len < 1e-6)
		return V; // sky, or a depth discontinuity - treat as facing us

	N /= len;
	return dot(N, V) < 0 ? -N : N;
}

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= postprocess.resolution.x || DTid.y >= postprocess.resolution.y)
		return;

	const float2 uv = (DTid.xy + 0.5f) * postprocess.resolution_rcp;

	float3 color = input.SampleLevel(sampler_linear_clamp, uv, 0).rgb;

	const uint segment_buffer = (uint)postprocess.params0.x;
	const uint segment_count = min((uint)postprocess.params0.y, ST_LASER_SEGMENT_MAX);
	const uint dot_buffer = (uint)postprocess.params0.z;
	const uint dot_count = min((uint)postprocess.params0.w, ST_LASER_DOT_MAX);

	const float depth = texture_depth.SampleLevel(sampler_point_clamp, uv, 0);
	const float3 P = reconstruct_position(uv, depth);
	const bool is_surface = depth > 0;

	const float3 O = GetCamera().position;
	const float3 ray = P - O;
	const float ray_length = length(ray);
	const float3 D = ray_length > 1e-4 ? ray / ray_length : GetCamera().forward;

	for (uint i = 0; i < segment_count; ++i)
	{
		color += laser_segment_light(load_segment(segment_buffer, i), O, D, ray_length);
	}

	[branch]
	if (dot_count > 0)
	{
		const float3 N = is_surface ? normal_from_depth(uv, P, -D) : -D;

		for (uint j = 0; j < dot_count; ++j)
		{
			const StLaserDot spot = load_dot(dot_buffer, j);

			color += laser_dot_air(spot, O, D, ray_length);

			[branch]
			if (is_surface)
			{
				color += laser_dot_surface(spot, P, N);
			}
		}
	}

	output[DTid.xy] = float4(color, 1);
}
