#ifndef MI_LENSFLARE_HLSLI
#define MI_LENSFLARE_HLSLI

// Shared declarations for the procedural lens flare pass (StLensFlareVS/PS.hlsl),
// drawn by mi::LensFlare (src/render/LensFlare.cpp).
//
// This pass deliberately does NOT include the engine's globals.hlsli. It needs no
// bindless resources and no scene data — the whole flare is generated from maths
// around a single screen-space sun position — so it declares its own root signature
// and stays independent of the engine's shader interop headers.

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
	"CBV(b0)"

// Mirrors mi::LensFlare::Constants in src/render/LensFlare.cpp — keep both in sync.
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
	float  occlusion;          // 0..1 visibility fade computed on the CPU
};

struct VertexOutput
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

#endif // MI_LENSFLARE_HLSLI
