#pragma once
// SIMTARY: a graphics device that executes nothing.
//
// For headless processes - a dedicated game server - that must run the WHOLE engine (scene
// update, terrain generation with its heightfield physics, particle bookkeeping, native
// components) on a machine with no GPU and no display. Every Create* succeeds; buffers and
// textures the CPU maps (UPLOAD / READBACK) get real memory so code writing instance data
// or reading statistics back works and reads zeros; command lists record nothing; shaders
// are never loaded (ShaderFormat::NONE - see wi::renderer::LoadShader).
//
// Nothing here is ever drawn, so nothing has to be correct beyond "does not crash and does
// not allocate GPU-sized memory for GPU-only resources".

#include "CommonInclude.h"
#include "wiGraphicsDevice.h"
#include "wiVector.h"

#include <memory>
#include <mutex>

namespace wi::graphics
{
	class GraphicsDevice_Null final : public GraphicsDevice
	{
		struct CommandList_Null
		{
			GPULinearAllocator frame_allocators[BUFFERCOUNT];
			RenderPassInfo renderpass_info;
			QUEUE_TYPE queue = QUEUE_GRAPHICS;
		};
		mutable std::mutex locker;
		wi::vector<std::unique_ptr<CommandList_Null>> commandlists;
		uint32_t cmd_count = 0;

		CommandList_Null& Get(CommandList cmd) { return *(CommandList_Null*)cmd.internal_state; }

	public:
		GraphicsDevice_Null();

		bool CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const override;
		bool CreateBuffer2(const GPUBufferDesc* desc, const std::function<void(void* dest)>& init_callback, GPUBuffer* buffer, const GPUResource* alias = nullptr, uint64_t alias_offset = 0ull) const override;
		bool CreateTexture(const TextureDesc* desc, const SubresourceData* initial_data, Texture* texture, const GPUResource* alias = nullptr, uint64_t alias_offset = 0ull) const override;
		bool CreateShader(ShaderStage stage, const void* shadercode, size_t shadercode_size, Shader* shader, const char* entrypoint = "main") const override;
		bool CreateSampler(const SamplerDesc* desc, Sampler* sampler) const override;
		bool CreateQueryHeap(const GPUQueryHeapDesc* desc, GPUQueryHeap* queryheap) const override;
		bool CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso, const RenderPassInfo* renderpass_info = nullptr) const override;

		int CreateSubresource(Texture* texture, SubresourceType type, uint32_t firstSlice, uint32_t sliceCount, uint32_t firstMip, uint32_t mipCount, const Format* format_change = nullptr, const ImageAspect* aspect = nullptr, const Swizzle* swizzle = nullptr, float min_lod_clamp = 0) const override { return 0; }
		int CreateSubresource(GPUBuffer* buffer, SubresourceType type, uint64_t offset, uint64_t size = ~0, const Format* format_change = nullptr, const uint32_t* structuredbuffer_stride_change = nullptr) const override { return 0; }
		void DeleteSubresources(GPUResource* resource) override {}

		int GetDescriptorIndex(const GPUResource* resource, SubresourceType type, int subresource = -1) const override { return -1; }
		int GetDescriptorIndex(const Sampler* sampler) const override { return -1; }

		CommandList BeginCommandList(QUEUE_TYPE queue = QUEUE_GRAPHICS) override;
		void SubmitCommandLists() override;
		void WaitForGPU() const override {}
		void ClearPipelineStateCache() override {}
		size_t GetActivePipelineCount() const override { return 0; }

		ShaderFormat GetShaderFormat() const override { return ShaderFormat::NONE; }
		Texture GetBackBuffer(const SwapChain* swapchain) const override;
		ColorSpace GetSwapChainColorSpace(const SwapChain* swapchain) const override { return ColorSpace::SRGB; }
		bool IsSwapChainSupportsHDR(const SwapChain* swapchain) const override { return false; }
		uint32_t GetMinOffsetAlignment(const GPUBufferDesc* desc) const override { return 256u; }
		MemoryUsage GetMemoryUsage() const override { return {}; }
		uint32_t GetMaxViewportCount() const override { return 16u; }
		const char* GetTag() const override { return "[Null]"; }

		void WaitCommandList(CommandList cmd, CommandList wait_for) override {}
		void RenderPassBegin(const SwapChain* swapchain, CommandList cmd) override {}
		void RenderPassBegin(const RenderPassImage* images, uint32_t image_count, CommandList cmd, RenderPassFlags flags = RenderPassFlags::NONE) override {}
		void RenderPassEnd(CommandList cmd) override {}
		void BindScissorRects(uint32_t numRects, const Rect* rects, CommandList cmd) override {}
		void BindViewports(uint32_t NumViewports, const Viewport* pViewports, CommandList cmd) override {}
		void BindResource(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource = -1) override {}
		void BindResources(const GPUResource* const* resources, uint32_t slot, uint32_t count, CommandList cmd) override {}
		void BindUAV(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource = -1) override {}
		void BindUAVs(const GPUResource* const* resources, uint32_t slot, uint32_t count, CommandList cmd) override {}
		void BindSampler(const Sampler* sampler, uint32_t slot, CommandList cmd) override {}
		void BindConstantBuffer(const GPUBuffer* buffer, uint32_t slot, CommandList cmd, uint64_t offset = 0ull) override {}
		void BindVertexBuffers(const GPUBuffer* const* vertexBuffers, uint32_t slot, uint32_t count, const uint32_t* strides, const uint64_t* offsets, CommandList cmd) override {}
		void BindIndexBuffer(const GPUBuffer* indexBuffer, const IndexBufferFormat format, uint64_t offset, CommandList cmd) override {}
		void BindStencilRef(uint32_t value, CommandList cmd) override {}
		void BindBlendFactor(float r, float g, float b, float a, CommandList cmd) override {}
		void BindPipelineState(const PipelineState* pso, CommandList cmd) override {}
		void BindComputeShader(const Shader* cs, CommandList cmd) override {}
		void BindDepthBounds(float min_bounds, float max_bounds, CommandList cmd) override {}
		void Draw(uint32_t vertexCount, uint32_t startVertexLocation, CommandList cmd) override {}
		void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation, CommandList cmd) override {}
		void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation, CommandList cmd) override {}
		void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation, CommandList cmd) override {}
		void DrawInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override {}
		void DrawIndexedInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override {}
		void DrawInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd) override {}
		void DrawIndexedInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd) override {}
		void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd) override {}
		void DispatchIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override {}
		void CopyResource(const GPUResource* pDst, const GPUResource* pSrc, CommandList cmd) override {}
		void CopyBuffer(const GPUBuffer* pDst, uint64_t dst_offset, const GPUBuffer* pSrc, uint64_t src_offset, uint64_t size, CommandList cmd) override {}
		void CopyTexture(const Texture* dst, uint32_t dstX, uint32_t dstY, uint32_t dstZ, uint32_t dstMip, uint32_t dstSlice, const Texture* src, uint32_t srcMip, uint32_t srcSlice, CommandList cmd, const Box* srcbox = nullptr, ImageAspect dst_aspect = ImageAspect::COLOR, ImageAspect src_aspect = ImageAspect::COLOR) override {}
		void QueryBegin(const GPUQueryHeap* heap, uint32_t index, CommandList cmd) override {}
		void QueryEnd(const GPUQueryHeap* heap, uint32_t index, CommandList cmd) override {}
		void QueryResolve(const GPUQueryHeap* heap, uint32_t index, uint32_t count, const GPUBuffer* dest, uint64_t dest_offset, CommandList cmd) override {}
		void Barrier(const GPUBarrier* barriers, uint32_t numBarriers, CommandList cmd) override {}
		void PushConstants(const void* data, uint32_t size, CommandList cmd, uint32_t offset = 0) override {}
		void ClearUAV(const GPUResource* resource, uint32_t value, CommandList cmd) override {}
		void EventBegin(const char* name, CommandList cmd) override {}
		void EventEnd(CommandList cmd) override {}
		void SetMarker(const char* name, CommandList cmd) override {}

		RenderPassInfo GetRenderPassInfo(CommandList cmd) override { return Get(cmd).renderpass_info; }
		GPULinearAllocator& GetFrameAllocator(CommandList cmd) override { return Get(cmd).frame_allocators[GetBufferIndex()]; }
	};
}
