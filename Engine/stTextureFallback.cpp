#include "stTextureFallback.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE // signed BC4/BC5
#include "Utility/bcdec.h"

#include <algorithm>
#include <cstring>

using namespace wi::graphics;

namespace st::texfallback
{
	Format DecodedFormat(Format format)
	{
		switch (format)
		{
		case Format::BC1_UNORM:
		case Format::BC2_UNORM:
		case Format::BC3_UNORM:
		case Format::BC7_UNORM:      return Format::R8G8B8A8_UNORM;
		case Format::BC1_UNORM_SRGB:
		case Format::BC2_UNORM_SRGB:
		case Format::BC3_UNORM_SRGB:
		case Format::BC7_UNORM_SRGB: return Format::R8G8B8A8_UNORM_SRGB;
		case Format::BC4_UNORM:      return Format::R8_UNORM;
		case Format::BC4_SNORM:      return Format::R8_SNORM;
		case Format::BC5_UNORM:      return Format::R8G8_UNORM;
		case Format::BC5_SNORM:      return Format::R8G8_SNORM;
		case Format::BC6H_UF16:
		case Format::BC6H_SF16:      return Format::R16G16B16A16_FLOAT;
		default:                     return format;
		}
	}

	namespace
	{
		// One 4x4 block into `dst`, whose rows are `pitch` bytes apart.
		void DecodeBlock(Format format, const uint8_t* block, uint8_t* dst, int pitch)
		{
			switch (format)
			{
			case Format::BC1_UNORM: case Format::BC1_UNORM_SRGB: bcdec_bc1(block, dst, pitch); break;
			case Format::BC2_UNORM: case Format::BC2_UNORM_SRGB: bcdec_bc2(block, dst, pitch); break;
			case Format::BC3_UNORM: case Format::BC3_UNORM_SRGB: bcdec_bc3(block, dst, pitch); break;
			case Format::BC7_UNORM: case Format::BC7_UNORM_SRGB: bcdec_bc7(block, dst, pitch); break;
			case Format::BC4_UNORM: bcdec_bc4(block, dst, pitch, 0); break;
			case Format::BC4_SNORM: bcdec_bc4(block, dst, pitch, 1); break;
			case Format::BC5_UNORM: bcdec_bc5(block, dst, pitch, 0); break;
			case Format::BC5_SNORM: bcdec_bc5(block, dst, pitch, 1); break;
			case Format::BC6H_UF16:
			case Format::BC6H_SF16:
			{
				// bcdec writes RGB halves; the texture is RGBA16F, so spread them and add A = 1.
				uint16_t rgb[4 * 4 * 3];
				bcdec_bc6h_half(block, rgb, 4 * 3, format == Format::BC6H_SF16 ? 1 : 0);
				for (int y = 0; y < 4; ++y)
				{
					uint16_t* row = (uint16_t*)(dst + y * pitch);
					for (int x = 0; x < 4; ++x)
					{
						row[x * 4 + 0] = rgb[(y * 4 + x) * 3 + 0];
						row[x * 4 + 1] = rgb[(y * 4 + x) * 3 + 1];
						row[x * 4 + 2] = rgb[(y * 4 + x) * 3 + 2];
						row[x * 4 + 3] = 0x3C00; // 1.0 as a half
					}
				}
				break;
			}
			default: break;
			}
		}
	}

	bool Decode(TextureDesc& desc, const SubresourceData* initial_data,
		std::vector<std::vector<uint8_t>>& storage, std::vector<SubresourceData>& subresources)
	{
		const Format source = desc.format;
		const Format target = DecodedFormat(source);
		if (target == source || initial_data == nullptr || desc.depth > 1)
			return false;

		const uint32_t mips = GetMipCount(desc);
		const uint32_t slices = std::max(1u, desc.array_size);
		const uint32_t blockBytes = GetFormatStride(source); // bytes per 4x4 block
		const uint32_t pixelBytes = GetFormatStride(target);

		storage.assign(size_t(mips) * slices, {});
		subresources.assign(size_t(mips) * slices, {});
		for (uint32_t slice = 0; slice < slices; ++slice)
		{
			for (uint32_t mip = 0; mip < mips; ++mip)
			{
				const uint32_t index = slice * mips + mip; // ComputeSubresource order
				const uint32_t w = std::max(1u, desc.width >> mip);
				const uint32_t h = std::max(1u, desc.height >> mip);
				const uint32_t bx = (w + 3) / 4;
				const uint32_t by = (h + 3) / 4;
				// Decoded into a whole-block canvas, then cut to the mip's real size: a 2x2
				// mip still arrives as one full 4x4 block.
				const uint32_t cw = bx * 4;
				std::vector<uint8_t> canvas(size_t(cw) * by * 4 * pixelBytes);
				const SubresourceData& src = initial_data[index];
				const uint32_t srcPitch = src.row_pitch != 0 ? src.row_pitch : bx * blockBytes;
				for (uint32_t y = 0; y < by; ++y)
				{
					const uint8_t* srcRow = (const uint8_t*)src.data_ptr + size_t(y) * srcPitch;
					for (uint32_t x = 0; x < bx; ++x)
					{
						uint8_t* dst = canvas.data() + (size_t(y) * 4 * cw + size_t(x) * 4) * pixelBytes;
						DecodeBlock(source, srcRow + size_t(x) * blockBytes, dst, int(cw * pixelBytes));
					}
				}
				std::vector<uint8_t>& out = storage[index];
				out.resize(size_t(w) * h * pixelBytes);
				for (uint32_t y = 0; y < h; ++y)
					std::memcpy(out.data() + size_t(y) * w * pixelBytes, canvas.data() + size_t(y) * cw * pixelBytes, size_t(w) * pixelBytes);
				subresources[index].data_ptr = out.data();
				subresources[index].row_pitch = w * pixelBytes;
				subresources[index].slice_pitch = w * h * pixelBytes;
			}
		}
		desc.format = target;
		desc.mip_levels = mips;
		return true;
	}
}
