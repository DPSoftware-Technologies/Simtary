// RmlUi geometry for Framework/ui. Output is premultiplied; the pipeline blends ONE /
// INV_SRC_ALPHA. Textures RmlUi generated (glyphs) are premultiplied already, files loaded
// through the resource manager are not - Flags.x says which.
struct VertexOutput
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
};

cbuffer UiConstants : register(b0)
{
	float4x4 Transform;
	float4   Flags;
};

Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=22, b999), CBV(b0), DescriptorTable(SRV(t0)), DescriptorTable(Sampler(s0))")]
float4 main(VertexOutput input) : SV_TARGET
{
	float4 t = texture0.Sample(sampler0, input.uv);
	if (Flags.x > 0.5f)
		t.rgb *= t.a;
	return input.col * t;
}
