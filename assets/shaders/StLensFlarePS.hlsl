#include "StLensFlare.hlsli"

// Procedural screen-space lens flare.
//
// Everything is generated from the sun's screen position — no scene colour or depth
// is sampled, so the pass costs one fullscreen triangle and reads no textures. It is
// blended additively over the composed frame (BSTYPE_ADDITIVE = SRC_ALPHA/ONE, so the
// shader returns alpha = 1 to contribute fully).
//
// The flare is built from five layers, each modelling a different real lens artifact:
//   1. glow      — light scattering in the lens right at the source
//   2. streak    — anamorphic horizontal smear
//   3. starburst — diffraction spikes off the aperture blades
//   4. ghosts    — internal reflections, mirrored through the lens axis
//   5. halo      — a wide chromatic ring centred on the optical axis

// Bounds the dynamic ghost loop so a bad constant can't stall the GPU.
#define MI_LENSFLARE_MAX_GHOSTS 16

// Radial falloffs must be measured in a space where one unit of x and one unit of y
// are the same number of pixels, otherwise every circle below rasterises as an
// ellipse stretched by the window's aspect ratio.
float2 AspectCorrect(float2 v)
{
	return float2(v.x * aspect, v.y);
}

// Soft disc with a brightened rim: reads as a lens iris rather than a plain blob.
float Iris(float2 v, float radius)
{
	float d = saturate(length(v) / radius);

	float body = 1.0 - d;
	body *= body;                            // smooth falloff, reaching 0 at the edge
	float rim = smoothstep(0.55, 0.95, d);   // brighter towards the outside

	return body * (0.35 + 0.65 * rim);
}

// Thin bright ring of the given radius, centred where `d` was measured from.
float Ring(float d, float radius)
{
	const float thickness = 0.045;
	float x = (d - radius) / thickness;
	return exp(-x * x);
}

// One ghost, evaluated three times with a small offset along the flare axis so the
// channels separate at the edges the way a real uncorrected lens splits them.
float3 Ghost(float2 uv, float2 centre, float radius, float2 axis)
{
	float2 offset = axis * chromaOffset;
	return float3(
		Iris(AspectCorrect(uv - centre + offset), radius),
		Iris(AspectCorrect(uv - centre), radius),
		Iris(AspectCorrect(uv - centre - offset), radius));
}

[RootSignature(MI_LENSFLARE_ROOTSIG)]
float4 main(VertexOutput input) : SV_TARGET
{
	const float2 uv = input.uv;
	const float2 screenCentre = float2(0.5, 0.5);

	float2 toSun = AspectCorrect(uv - sunUV);
	float  dSun  = length(toSun);

	float3 col = float3(0.0, 0.0, 0.0);

	// ── 1. Glow ─────────────────────────────────────────────────────────────────
	// A tight core for the disc of the sun itself, plus a much wider, dimmer bloom
	// for the light scattered across the whole lens. Only the core is allowed to
	// clip to white — the bloom stays low so it tints the frame instead of erasing it.
	col += tint * glowIntensity * (exp(-dSun * dSun * 300.0) * 1.0 +
	                               exp(-dSun * 9.0) * 0.12);

	// ── 2. Anamorphic streak ────────────────────────────────────────────────────
	// Wide in x, razor thin in y.
	col += tint * streakIntensity *
	       exp(-abs(toSun.x) * 5.0) * exp(-abs(toSun.y) * 220.0);

	// ── 3. Starburst ────────────────────────────────────────────────────────────
	// Two spike sets at different frequencies so the count looks irregular. Rotating
	// slowly on `time` keeps it alive instead of looking like a decal.
	float angle = atan2(toSun.y, toSun.x) + time * 0.05;
	float spikes = pow(abs(cos(angle * 5.0)), 16.0) +
	               0.6 * pow(abs(cos(angle * 9.0 + 1.7)), 24.0);
	col += tint * starburstIntensity * spikes * exp(-dSun * 9.0);

	// ── 4. Ghosts ───────────────────────────────────────────────────────────────
	// Internal reflections land on the line through the sun and the optical axis
	// (the screen centre), so march along that vector and drop an iris at each step.
	float2 axis  = screenCentre - sunUV;
	float2 axisN = normalize(axis + 1e-6);

	int ghosts = clamp((int)ghostCount, 0, MI_LENSFLARE_MAX_GHOSTS);
	for (int i = 1; i <= ghosts; i++)
	{
		float t = (float)i;

		// The golden-ratio fractions vary size/brightness/hue per ghost without a
		// lookup table, so no two ghosts look stamped from the same die.
		float2 centre = sunUV + axis * (ghostSpacing * t);
		float  radius = 0.02 + 0.05 * frac(t * 0.618);
		float  weight = 0.35 + 0.65 * frac(t * 0.371);
		float3 ghostTint = lerp(tint, tint.bgr, frac(t * 0.271));

		col += ghostTint * Ghost(uv, centre, radius, axisN) * weight * 0.5;
	}

	// ── 5. Halo ─────────────────────────────────────────────────────────────────
	// Centred on the optical axis, not on the sun, and only visible when the sun is
	// near the middle of the frame — which is exactly when a real one blooms.
	float dCentre = length(AspectCorrect(uv - screenCentre));
	float3 halo = float3(
		Ring(dCentre, haloWidth + chromaOffset),
		Ring(dCentre, haloWidth),
		Ring(dCentre, haloWidth - chromaOffset));
	// Kept deliberately faint: additive light on a dark sky reads much stronger than
	// the raw number suggests, and a bright ring this large dominates the frame.
	float haloWeight = saturate(1.0 - length(AspectCorrect(sunUV - screenCentre)) * 1.5);
	col += tint * halo * haloWeight * 0.16;

	// `occlusion` folds in the CPU-side fades (sun behind the camera, sun off the
	// edge of the frame, sun below the horizon).
	col *= intensity * occlusion;

	return float4(col, 1.0);
}
