#pragma once
// SIMTARY: block-compressed textures on a GPU that cannot sample them.
//
// Every compressed texture in the content is BC (BC1-BC7). Mobile GPUs ship ASTC and
// ETC2 instead and report textureCompressionBC = false, so a BC texture there fails to
// create - and the engine's own built-ins (the water ripple is BC5) fail with it. Until
// the Android content is re-encoded, a graphics device without BC support decodes on the
// CPU at creation time: same size and mips, an uncompressed format of the same channels.
// It costs 4-8x the memory of the BC original, which is why it is a bridge, not the plan.

#include "wiGraphics.h"

#include <cstdint>
#include <vector>

namespace st::texfallback
{
	// The uncompressed format a BC format decodes to (RGBA8 / R8 / RG8 / RGBA16F, keeping
	// sRGB and signedness). Any other format comes back unchanged.
	wi::graphics::Format DecodedFormat(wi::graphics::Format format);

	// Decode every subresource of a BC texture. On success `desc.format` is the decoded
	// format, and `subresources` (pointing into `storage`) replaces the original initial
	// data. False = nothing to do (not BC, 3D, or no data) - create as asked.
	bool Decode(wi::graphics::TextureDesc& desc, const wi::graphics::SubresourceData* initial_data,
		std::vector<std::vector<uint8_t>>& storage, std::vector<wi::graphics::SubresourceData>& subresources);
}
