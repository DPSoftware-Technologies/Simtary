#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"
#include "StProjectorInterop.h"

// Square (and elliptical, and rounded) light projection.
//
// The engine's spot light is a cone: light_spot() in lightingHF.hlsli rejects
// everything outside a circular cutoff, so a spot light with a mask texture always
// lands as a circle with the image cropped inside it. This pass replaces the cone
// with a real projector gate evaluated in the projector's own NDC, where the image
// spans [-1,1] on both axes and the outline is whatever ST_PROJECTOR_SHAPE_ says.
//
// It runs as a RenderPath3D custom post process (Stage::BeforeTonemap), so it reads
// the depth buffer, adds into the HDR colour, and its light is still bloomed and
// tone mapped along with everything else. What it does NOT get, being screen space:
// shadow maps (there is a screen-space approximation instead), lighting of
// transparent surfaces, and any contribution to reflections or GI.
//
// params0.x  bindless index of the StProjector buffer
// params0.y  number of projectors in it

PUSHCONSTANT(postprocess, PostProcess);

Texture2D<float4> input : register(t0);

RWTexture2D<float4> output : register(u0);

inline StProjector load_projector(uint buffer_index, uint i)
{
	return bindless_buffers[descriptor_index(buffer_index)].Load<StProjector>(i * sizeof(StProjector));
}

// World position -> projector clip space. The matrix is stored as DirectXMath rows,
// so the position goes on the left (see StProjectorInterop.h).
inline float4 projector_clip(in StProjector projector, in float3 P)
{
	return P.x * projector.vp0 + P.y * projector.vp1 + P.z * projector.vp2 + projector.vp3;
}

// Optical corrections, all in NDC. Order matters and mirrors the light path through a
// real projector: the gate sits behind the lens, so shift moves the image first, then
// keystone (an off-axis screen) skews it, then the lens distorts it radially.
inline float2 projector_optics(in StProjector projector, float2 ndc)
{
	ndc -= projector.shift;

	// Trapezoid: each axis is scaled by how far along the other axis the sample is,
	// which is what tilting the projector against a flat screen does to the image.
	const float2 skewed = float2(
		ndc.x * (1.0 + projector.keystone.x * ndc.y),
		ndc.y * (1.0 + projector.keystone.y * ndc.x)
	);

	// Barrel (+) / pincushion (-).
	return skewed * (1.0 + projector.distortion * dot(skewed, skewed));
}

// 1 inside the gate, 0 outside, feathered by `softness` on the way out.
inline float projector_gate(in StProjector projector, in float2 ndc)
{
	float d;
	if (projector.shape == ST_PROJECTOR_SHAPE_ELLIPSE)
	{
		d = length(ndc);
	}
	else if (projector.shape == ST_PROJECTOR_SHAPE_ROUNDED)
	{
		const float r = saturate(projector.corner_radius);
		const float2 q = abs(ndc) - (1.0 - r);
		// Rounded-box SDF, rebased so that 1 is the edge like the other two shapes.
		d = 1.0 + length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
	}
	else // ST_PROJECTOR_SHAPE_RECT
	{
		d = max(abs(ndc.x), abs(ndc.y));
	}

	const float softness = max(1e-4, projector.softness);
	return 1.0 - smoothstep(1.0 - softness, 1.0, d);
}

// Brightness against throw distance. falloff = 0 keeps the image as bright far away
// as near (stylised); falloff = 1 is the physical inverse square, normalised so that
// focus_distance is the distance where the authored intensity is what you get.
inline float projector_attenuation(in StProjector projector, in float dist)
{
	const float ref = max(0.01, projector.focus_distance);
	const float inv_square = (ref * ref) / max(1e-4, dist * dist);
	const float att = lerp(1.0, inv_square, saturate(projector.falloff));
	return att * (1.0 - smoothstep(projector.range * 0.85, projector.range, dist));
}

inline float henyey_greenstein(in float cos_theta, in float g)
{
	const float g2 = g * g;
	const float d = 1.0 + g2 - 2.0 * g * cos_theta;
	return (1.0 - g2) / (4.0 * PI * max(1e-4, d * sqrt(max(1e-4, d))));
}

// The image itself: gate, texture, vignette, gamma. Returns 0 outside the gate or
// behind the lens, and reports the gate so the caller can skip the rest.
inline float3 projector_image(in StProjector projector, in float3 P, out float gate)
{
	gate = 0;

	const float4 clip = projector_clip(projector, P);
	if (clip.w <= 1e-4)
		return 0; // behind the lens

	const float2 ndc = projector_optics(projector, clip.xy / clip.w);

	gate = projector_gate(projector, ndc);
	if (gate <= 0.001)
		return 0;

	float3 image = projector.color;

	[branch]
	if (projector.texture_index > 0)
	{
		const float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
		const float4 texel = bindless_textures[NonUniformResourceIndex(descriptor_index(projector.texture_index))]
			.SampleLevel(sampler_linear_clamp, uv, 0);
		image *= texel.rgb * texel.a;
	}

	if (projector.gamma != 1.0)
	{
		image = pow(max(image, 0.0), projector.gamma);
	}

	// Corner darkening, measured on the gate's own radius so it tracks the shape.
	gate *= 1.0 - saturate(projector.vignette) * saturate(dot(ndc, ndc) * 0.5);

	return image * projector.intensity;
}

// Screen-space stand-in for a shadow map: walk from the lit point back towards the
// lens and see whether anything already drawn sits in the way. It can only test what
// is on screen, so a blocker outside the frame does not shadow - the usual trade.
inline float projector_occlusion(in StProjector projector, in float3 P, in float3 L, in float dist)
{
	const uint steps = max(1u, min(projector.occlusion_samples, 32u));
	const float step_size = dist / (steps + 1);

	for (uint i = 1; i <= steps; ++i)
	{
		const float3 X = P + L * (step_size * i);

		const float4 clip = mul(GetCamera().view_projection, float4(X, 1));
		if (clip.w <= 1e-4)
			break;

		const float2 uv = clipspace_to_uv(clip.xy / clip.w);
		if (!is_saturated(uv))
			break; // walked off screen - nothing left to test against

		const float scene_depth = texture_depth.SampleLevel(sampler_point_clamp, uv, 0);
		const float delta = compute_lineardepth(clip.z / clip.w) - compute_lineardepth(scene_depth);

		// delta > 0: this step sits behind whatever was rendered there, so something
		// blocks the beam. The upper bound stops a distant wall from shadowing the
		// air in front of it (the depth buffer carries no thickness of its own).
		if (delta > 0.02 && delta < projector.occlusion_thickness)
			return 1.0 - saturate(projector.occlusion_strength);
	}

	return 1.0;
}

inline bool ray_sphere(in float3 ro, in float3 rd, in float3 center, in float radius, out float t0, out float t1)
{
	t0 = 0;
	t1 = 0;
	const float3 oc = ro - center;
	const float b = dot(oc, rd);
	const float c = dot(oc, oc) - radius * radius;
	const float h = b * b - c;
	if (h < 0)
		return false;
	const float sh = sqrt(h);
	t0 = -b - sh;
	t1 = -b + sh;
	return true;
}

// The visible shaft of light in the air. Marches the view ray through the projector's
// bounding sphere, gating every sample the same way the surface path does - which is
// what makes the beam a rectangular pyramid instead of a cone.
inline float3 projector_beam(in StProjector projector, in float3 ray_origin, in float3 ray_dir, in float ray_length, in uint2 pixel)
{
	float t0, t1;
	if (!ray_sphere(ray_origin, ray_dir, projector.position, projector.range, t0, t1))
		return 0;

	t0 = max(t0, 0.0);
	t1 = min(t1, ray_length);

	// The sphere is generous: half of it sits behind the lens, where there is never
	// any light. Clip the march to the half-space in front, which on a wide-range
	// projector roughly doubles the samples that land inside the beam.
	const float facing = dot(ray_dir, projector.direction);
	const float behind = dot(projector.position - ray_origin, projector.direction);
	if (abs(facing) > 1e-4)
	{
		const float t_plane = behind / facing;
		if (facing > 0)
			t0 = max(t0, t_plane); // ray enters the front half-space here
		else
			t1 = min(t1, t_plane); // ... and leaves it here
	}
	else if (behind > 0)
	{
		return 0; // ray runs parallel to the gate, entirely behind the lens
	}

	if (t1 <= t0)
		return 0;

	const uint samples = max(2u, min(projector.beam_samples, 64u));
	const float step_size = (t1 - t0) / samples;

	// Jitter the entry point so undersampling reads as noise rather than as bands
	// across the shaft. Same dither the engine's own volumetric light passes use.
	float t = t0 + step_size * dither((min16uint2)pixel);

	float3 accumulation = 0;

	[loop]
	for (uint i = 0; i < samples; ++i)
	{
		const float3 X = ray_origin + ray_dir * t;

		float gate;
		const float3 image = projector_image(projector, X, gate);

		[branch]
		if (gate > 0.001)
		{
			const float3 to_sample = X - projector.position;
			const float dist = length(to_sample);
			const float3 forward = to_sample / max(1e-4, dist);

			accumulation += image * gate
				* projector_attenuation(projector, dist)
				* henyey_greenstein(dot(ray_dir, forward), clamp(projector.beam_anisotropy, -0.95, 0.95))
				* projector.beam_density * step_size;
		}

		t += step_size;
	}

	return accumulation;
}

// Normal from the depth buffer. The GBuffer normal texture is not guaranteed to still
// be bound this late in the frame, and one cross product of two neighbouring
// reconstructed positions is accurate enough to keep light off back-facing walls.
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
		return V; // degenerate (sky, or a depth discontinuity) - treat as facing us

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

	const uint buffer_index = (uint)postprocess.params0.x;
	const uint projector_count = min((uint)postprocess.params0.y, ST_PROJECTOR_MAX);

	if (projector_count > 0)
	{
		const float depth = texture_depth.SampleLevel(sampler_point_clamp, uv, 0);
		const float3 P = reconstruct_position(uv, depth);

		const float3 camera_position = GetCamera().position;
		const float3 ray = P - camera_position;
		const float ray_length = length(ray);
		const float3 ray_dir = ray_length > 1e-4 ? ray / ray_length : GetCamera().forward;

		// Sky pixels reconstruct out on the far plane; the beam march is clamped to
		// the projector's own bounding sphere anyway, so nothing runs away.
		const bool is_surface = depth > 0;

		const float3 V = -ray_dir;
		const float3 N = is_surface ? normal_from_depth(uv, P, V) : V;

		for (uint i = 0; i < projector_count; ++i)
		{
			const StProjector projector = load_projector(buffer_index, i);

			[branch]
			if (is_surface && (projector.flags & ST_PROJECTOR_FLAG_LIGHT_SURFACES))
			{
				float gate;
				const float3 image = projector_image(projector, P, gate);

				[branch]
				if (gate > 0.001)
				{
					float3 to_lens = projector.position - P;
					const float dist = length(to_lens);
					to_lens /= max(1e-4, dist);

					float contribution = gate * projector_attenuation(projector, dist);

					if (projector.flags & ST_PROJECTOR_FLAG_LAMBERT)
					{
						contribution *= saturate(dot(N, to_lens));
					}

					[branch]
					if (contribution > 0.001 && (projector.flags & ST_PROJECTOR_FLAG_OCCLUSION))
					{
						contribution *= projector_occlusion(projector, P, to_lens, dist);
					}

					color += image * contribution;
				}
			}

			[branch]
			if (projector.flags & ST_PROJECTOR_FLAG_BEAM)
			{
				color += projector_beam(projector, camera_position, ray_dir, ray_length, DTid.xy);
			}
		}
	}

	output[DTid.xy] = float4(color, 1);
}
