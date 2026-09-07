#ifndef MI_LENSFLARE_HLSLI
#define MI_LENSFLARE_HLSLI

// Shared declarations for the procedural lens flare pass (StLensFlareVS/PS.hlsl),
// drawn by mi::LensFlare (src/render/LensFlare.cpp).
//
// This pass deliberately does NOT include the engine's globals.hlsli. It needs no
// bindless resources and no scene data - the whole flare is generated from maths around
// a single screen-space sun position, plus a handful of depth taps at that position - so
// it declares its own root signature and stays independent of the engine's shader
// interop headers.

// VS and PS must declare an identical root signature (DX12 builds one root signature
// per PSO), hence the shared macro.
//
// RootConstants(b999) is not used by these shaders but must be present: the DX12
// backend builds an indirect draw command signature from rootsig_optimizer.PUSH
// every time a vertex shader is created (wiGraphicsDevice_DX12.cpp), and that
// lookup fails if the root parameter is missing. The count matches the ImGui
// shaders, which use the same convention.
#define MI_LENSFLARE_ROOTSIG \
	"RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
	"RootConstants(num32BitConstants=22, b999), " \
	"CBV(b0), " \
	"DescriptorTable(SRV(t0))"

// Mirrors mi::LensFlare::Constants in src/render/LensFlare.cpp - keep both in sync.
// Laid out as four tight 16-byte rows so the C++ struct maps 1:1 with no implicit
// HLSL padding.
cbuffer LensFlareCB : register(b0)
{
	float2 sunUV;              // sun in [0,1] UV space; may fall outside the viewport
	float  aspect;             // viewport width / height; keeps radial falloffs circular
	float  intensity;          // master multiplier

	float3 tint;               // base flare colour (tracks the sun's own colour)
	float  ghostSpacing;       // spacing of ghosts along the sun -> screen-centre axis

	float  ghostCount;         // ghost count; float keeps the 16-byte rows tidy
	float  haloWidth;          // radius of the halo ring, measured from screen centre
	float  streakIntensity;    // horizontal anamorphic streak strength
	float  glowIntensity;      // strength of the glow at the sun itself

	float  chromaOffset;       // per-channel radial split, in UV units
	float  starburstIntensity; // radial spike strength
	float  time;               // seconds; rotates the starburst so it shimmers
	float  occlusion;          // 0..1 fade computed on the CPU (off-screen, horizon)

	float  depthTest;          // 1 = test the sun against the scene depth, 0 = ignore it
	float  occlusionRadius;    // radius of the depth tap ring, in UV units
	float  pad0;
	float  pad1;
};

// The scene's depth, single-sampled, at the RENDER path's resolution - which is not the
// canvas resolution when render scaling is on, hence GetDimensions() rather than a
// resolution constant. Read with Load(), so this pass still needs no sampler.
Texture2D<float> sceneDepth : register(t0);

// How much of the sun's disc the scene is NOT covering, 0..1.
//
// A lens flare is light that reached the LENS, so it must not survive the object standing
// in front of the sun. One tap would pop the whole flare on and off as an edge crossed it;
// a nine-tap disc the size of the sun's apparent disc fades it out the way a real one goes
// as it slides behind a wall.
//
// Depth is reverse-Z, so the far plane - and with it the sky - is 0. Anything greater than
// zero at a tap is geometry between the camera and infinity, which is where the sun is.
// That is also why this needs no camera matrices and no linearisation.
float LensFlareSunVisibility()
{
	if (depthTest < 0.5)
		return 1.0;

	uint2 size;
	sceneDepth.GetDimensions(size.x, size.y);
	const float2 resolution = float2(size);
	const int2   maxTexel = int2(size) - 1;

	// Aspect-corrected, so the tap disc is round on screen rather than stretched into an
	// ellipse by the window shape.
	const float2 radius = float2(occlusionRadius / max(aspect, 1e-4), occlusionRadius);

	float visible = 0.0;
	[unroll]
	for (int i = 0; i < 9; i++)
	{
		// i == 8 is the centre tap; 0..7 are a ring, one every 45 degrees.
		const float  angle  = 0.7853981634 * (float)i;
		const float2 offset = (i == 8) ? float2(0.0, 0.0)
		                               : float2(cos(angle), sin(angle)) * radius;

		const int2 texel = clamp(int2((sunUV + offset) * resolution), int2(0, 0), maxTexel);
		// > 0 means something was drawn there; == 0 is the cleared far plane, i.e. sky.
		visible += (sceneDepth.Load(int3(texel, 0)) > 0.0) ? 0.0 : 1.0;
	}
	return visible * (1.0 / 9.0);
}

struct VertexOutput
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	// Constant across the triangle: sampled once per vertex rather than per pixel, since
	// every pixel would otherwise re-read the same nine texels.
	nointerpolation float visibility : TEXCOORD1;
};

#endif // MI_LENSFLARE_HLSLI
