// RmlUi geometry for Framework/ui: pixel-space vertices, one matrix (projection x document
// transform x translation, built on the CPU in row-vector order - see UIRml.cpp).
struct VertexInput
{
	float2 pos : POSITION;
	float4 col : COLOR0;     // premultiplied, as RmlUi 6 produces it
	float2 uv  : TEXCOORD0;
};

struct VertexOutput
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
};

cbuffer UiConstants : register(b0)
{
	float4x4 Transform;
	float4   Flags;          // x: 1 = the bound texture is straight alpha, premultiply it
};

[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=22, b999), CBV(b0), DescriptorTable(SRV(t0)), DescriptorTable(Sampler(s0))")]
VertexOutput main(VertexInput input)
{
	VertexOutput output;
	output.pos = mul(Transform, float4(input.pos.xy, 0.f, 1.f));
	output.uv = input.uv;
	output.col = input.col;
	return output;
}
