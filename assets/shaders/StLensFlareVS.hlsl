#include "StLensFlare.hlsli"

// Fullscreen-triangle vertex shader for the procedural lens flare.
//
// No vertex or index buffer is bound: the corners come from SV_VertexID, so the
// draw is just device->Draw(3, 0, cmd). One oversized triangle is used rather than
// a two-triangle quad - it covers the viewport without a diagonal seam where the
// two halves meet, and rasterises marginally faster.

[RootSignature(MI_LENSFLARE_ROOTSIG)]
VertexOutput main(uint vertexID : SV_VertexID)
{
	VertexOutput output;

	// 0 -> (0,0), 1 -> (2,0), 2 -> (0,2). The [0,1] UV square is the visible quarter.
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// UV space (y down, origin top-left) -> clip space (y up, origin centre).
	output.pos = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

	return output;
}
