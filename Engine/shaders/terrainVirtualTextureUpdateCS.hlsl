#include "globals.hlsli"
#include "BlockCompress.hlsli"
#include "ColorSpaceUtility.hlsli"

PUSHCONSTANT(push, TerrainVirtualTexturePush);

#if !defined(UPDATE_NORMALMAP) && !defined(UPDATE_SURFACEMAP) && !defined(UPDATE_EMISSIVEMAP)
#define UPDATE_BASECOLORMAP
#endif

static const uint2 block_offsets[16] = {
	uint2(0, 0), uint2(1, 0), uint2(2, 0), uint2(3, 0),
	uint2(0, 1), uint2(1, 1), uint2(2, 1), uint2(3, 1),
	uint2(0, 2), uint2(1, 2), uint2(2, 2), uint2(3, 2),
	uint2(0, 3), uint2(1, 3), uint2(2, 3), uint2(3, 3),
};

// Where a layer's texture is sampled. A layer material with texMulAdd.xy other than 1 (SIMTARY:
//	authored terrain sets it from the layer's tile size) repeats its textures that many times per
//	chunk at every distance, so the pattern keeps its size from near to far. Otherwise the engine's
//	own rule: one texel per virtual-texture pixel up close, once per chunk when the chunk's texture
//	is smaller than the material's - which changes the pattern's size with distance.
inline float2 LayerUV(ShaderMaterial material, Texture2D tex, float2 uv2, out float lod)
{
	float2 dim = 0;
	tex.GetDimensions(dim.x, dim.y);
	const float2 tiling = material.texMulAdd.xy;
	if (any(tiling != 1))
	{
		const float2 diff = dim * tiling * push.resolution_rcp;
		lod = log2(max(diff.x, diff.y));
		return uv2 * tiling + material.texMulAdd.zw;
	}
	const float2 diff = dim * push.resolution_rcp;
	lod = log2(max(diff.x, diff.y));
	const float2 overscale = lod < 0 ? diff : 1;
	return uv2 / overscale;
}

// SIMTARY: anti-tiling. `world` is the position in chunk units, continuous across chunks and
//	oriented like uv2 (so a mirrored normal map never appears). A second sample of the same
//	texture at an unrelated scale and offset is blended in where low-frequency world noise says
//	so - the repeat of either alone is broken up by the other. Only for authored layers (tile
//	size set); the engine's own rule keeps one sample.
static const float ANTITILE_SCALE = 0.43;
static const float ANTITILE_FREQUENCY = 3.0; // blend patches per chunk
inline half4 SampleLayer(ShaderMaterial material, Texture2D tex, float2 uv2, float2 world)
{
	float lod;
	const float2 layer_uv = LayerUV(material, tex, uv2, lod);
	const half4 a = tex.SampleLevel(sampler_linear_wrap, layer_uv, lod);
	if (all(material.texMulAdd.xy == 1))
		return a;
	const float2 uv_b = world * material.texMulAdd.xy * ANTITILE_SCALE + float2(0.37, 0.71);
	const half4 b = tex.SampleLevel(sampler_linear_wrap, uv_b, lod + log2(ANTITILE_SCALE));
	const float n = noise_gradient_3D(float3(world * ANTITILE_FREQUENCY, 0.5)) * 0.5 + 0.5;
	return lerp(a, b, (half)smoothstep(0.35, 0.65, n));
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= push.write_size || DTid.y >= push.write_size)
		return;

	Texture2DArray blendmap = bindless_textures2DArray[descriptor_index(push.blendmap_texture)];
	ByteAddressBuffer blendmap_buffer = bindless_buffers[descriptor_index(push.blendmap_buffer)];
	
#if defined(UPDATE_BASECOLORMAP) || defined(UPDATE_EMISSIVEMAP)
	RWTexture2D<uint2> output = bindless_rwtextures_uint2[descriptor_index(push.output_texture)];
#else
	RWTexture2D<uint4> output = bindless_rwtextures_uint4[descriptor_index(push.output_texture)];
#endif
		
#ifdef UPDATE_BASECOLORMAP
	float3 block_rgb[16];
#endif // UPDATE_BASECOLORMAP
		
#ifdef UPDATE_EMISSIVEMAP
	float3 block_rgb[16];
#endif // UPDATE_EMISSIVEMAP

#ifdef UPDATE_NORMALMAP
	float block_x[16];
	float block_y[16];
#endif // UPDATE_NORMALMAP

#ifdef UPDATE_SURFACEMAP
	float3 block_rgb[16];
	float block_a[16];
#endif // UPDATE_SURFACEMAP

	const int2 chunk = int2((push.chunk_xz << 16) >> 16, push.chunk_xz >> 16);

	for(uint idx = 0; idx < 16; ++idx)
	{
		const uint2 block_offset = block_offsets[idx];
		const int2 pixel = push.offset + DTid.xy * 4 + block_offset;
		const float2 uv = (pixel.xy + 0.5f) * push.resolution_rcp;
		const float2 uv2 = float2(uv.x, 1 - uv.y);
		// uv runs along world +X / +Z inside the chunk, so chunk + uv is continuous across
		//	chunks; z is negated to match uv2's orientation.
		const float2 world = float2(chunk.x + uv.x, -(chunk.y + uv.y));
		
		half4 total_color = 0;
		half accumulation = 0;

		// Note: blending is front-to back with early exit like decals
		for(int blendmap_index = push.blendmap_layers - 1; blendmap_index >= 0; blendmap_index--)
		{
			float weight = blendmap.SampleLevel(sampler_linear_clamp, float3(uv, blendmap_index), 0).r;
			if(weight == 0)
				continue;

			uint materialIndex = blendmap_buffer.Load(push.blendmap_buffer_offset + blendmap_index * sizeof(uint));
			ShaderMaterial material = load_material(materialIndex);

#ifdef UPDATE_BASECOLORMAP
			float4 baseColor = material.GetBaseColor();
			[branch]
			if (material.textures[BASECOLORMAP].IsValid())
			{
				Texture2D tex = bindless_textures[descriptor_index(material.textures[BASECOLORMAP].texture_descriptor)];
				half4 baseColorMap = SampleLayer(material, tex, uv2, world);
				baseColor *= baseColorMap;
			}
			total_color = mad(1 - accumulation, weight * baseColor, total_color);
			accumulation = mad(1 - weight, accumulation, weight);
			if(accumulation >= 1)
				break;
#endif // UPDATE_BASECOLORMAP

#ifdef UPDATE_NORMALMAP
			[branch]
			if (material.textures[NORMALMAP].IsValid())
			{
				Texture2D tex = bindless_textures[descriptor_index(material.textures[NORMALMAP].texture_descriptor)];
				half2 normalMap = SampleLayer(material, tex, uv2, world).rg;
				total_color.rg = mad(1 - accumulation, weight * normalMap, total_color.rg);
				accumulation = mad(1 - weight, accumulation, weight);
				if(accumulation >= 1)
					break;
			}
#endif // UPDATE_NORMALMAP

#ifdef UPDATE_SURFACEMAP
			float4 surface = float4(1, material.GetRoughness(), material.GetMetalness(), material.GetReflectance());
			[branch]
			if (material.textures[SURFACEMAP].IsValid())
			{
				Texture2D tex = bindless_textures[descriptor_index(material.textures[SURFACEMAP].texture_descriptor)];
				half4 surfaceMap = SampleLayer(material, tex, uv2, world);
				surface *= surfaceMap;
			}
			total_color = mad(1 - accumulation, weight * surface, total_color);
			accumulation = mad(1 - weight, accumulation, weight);
			if(accumulation >= 1)
				break;
#endif // UPDATE_SURFACEMAP

#ifdef UPDATE_EMISSIVEMAP
			float4 emissiveColor = 0;
			[branch]
			if (material.textures[EMISSIVEMAP].IsValid())
			{
				Texture2D tex = bindless_textures[descriptor_index(material.textures[EMISSIVEMAP].texture_descriptor)];
				half4 emissiveMap = SampleLayer(material, tex, uv2, world);
				emissiveColor.rgb = emissiveMap.rgb * emissiveMap.a;
				emissiveColor.a = emissiveMap.a;
			}
			total_color = mad(1 - accumulation, weight * emissiveColor, total_color);
			accumulation = mad(1 - weight, accumulation, weight);
			if(accumulation >= 1)
				break;
#endif // UPDATE_EMISSIVEMAP
		}

#ifdef UPDATE_BASECOLORMAP
		// SIMTARY: macro variation - slow light/dark patches (two octaves, ~1 and ~3 per chunk)
		//	so a hillside is not one even repeat even where the anti-tiling blend agrees.
		const float macro = noise_gradient_3D(float3(world * 1.1, 2.5)) * 0.65 + noise_gradient_3D(float3(world * 3.3, 7.5)) * 0.35;
		total_color.rgb *= (half)lerp(0.85, 1.12, saturate(macro * 0.5 + 0.5));
		block_rgb[idx] = ApplySRGBCurve_Fast(total_color.rgb);
#endif // UPDATE_BASECOLORMAP

#ifdef UPDATE_NORMALMAP
		block_x[idx] = total_color.r;
		block_y[idx] = total_color.g;
#endif // UPDATE_NORMALMAP

#ifdef UPDATE_SURFACEMAP
		block_rgb[idx] = total_color.rgb;
		block_a[idx] = total_color.a;
#endif // UPDATE_SURFACEMAP

#ifdef UPDATE_EMISSIVEMAP
		block_rgb[idx] = ApplySRGBCurve_Fast(total_color.rgb);
#endif // UPDATE_EMISSIVEMAP
	}

	const uint2 write_coord = push.write_offset + DTid.xy;

#ifdef UPDATE_BASECOLORMAP
	output[write_coord] = CompressBC1Block(block_rgb);
#endif // UPDATE_BASECOLORMAP

#ifdef UPDATE_NORMALMAP
	output[write_coord] = CompressBC5Block(block_x, block_y);
#endif // UPDATE_NORMALMAP

#ifdef UPDATE_SURFACEMAP
	output[write_coord] = CompressBC3Block(block_rgb, block_a);
#endif // UPDATE_SURFACEMAP

#ifdef UPDATE_EMISSIVEMAP
	output[write_coord] = CompressBC1Block(block_rgb);
#endif // UPDATE_EMISSIVEMAP
}

