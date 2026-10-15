#include "wiGraphicsDevice_Null.h"

namespace wi::graphics
{
	namespace
	{
		// The only thing a null resource owns: memory for what the CPU maps.
		struct Resource_Null
		{
			wi::vector<uint8_t> memory;
		};
	}

	GraphicsDevice_Null::GraphicsDevice_Null()
	{
		adapterName = "Null (headless)";
		driverDescription = "no GPU - nothing is rendered";
		adapterType = AdapterType::Cpu;
		capabilities = GraphicsDeviceCapability::NONE;
		TIMESTAMP_FREQUENCY = 1; // profiler divides by it
	}

	bool GraphicsDevice_Null::CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const
	{
		swapchain->internal_state = wi::allocator::make_shared<Resource_Null>();
		swapchain->desc = *desc;
		return true;
	}

	bool GraphicsDevice_Null::CreateBuffer2(const GPUBufferDesc* desc, const std::function<void(void* dest)>& init_callback, GPUBuffer* buffer, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Resource_Null>();
		buffer->internal_state = internal_state;
		buffer->type = GPUResource::Type::BUFFER;
		buffer->mapped_data = nullptr;
		buffer->mapped_size = 0;
		buffer->desc = *desc;

		const bool cpuVisible = desc->usage == Usage::UPLOAD || desc->usage == Usage::READBACK;
		if (cpuVisible)
		{
			internal_state->memory.resize((size_t)desc->size);
			buffer->mapped_data = internal_state->memory.data();
			buffer->mapped_size = (size_t)desc->size;
		}
		if (init_callback)
		{
			if (cpuVisible)
			{
				init_callback(internal_state->memory.data());
			}
			else
			{
				// GPU-only: the initial contents go nowhere, but the callback may have side
				//	effects the caller counts on, so it still runs against scratch memory.
				wi::vector<uint8_t> scratch((size_t)desc->size);
				init_callback(scratch.data());
			}
		}
		return true;
	}

	bool GraphicsDevice_Null::CreateTexture(const TextureDesc* desc, const SubresourceData* initial_data, Texture* texture, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Resource_Null>();
		texture->internal_state = internal_state;
		texture->type = GPUResource::Type::TEXTURE;
		texture->mapped_data = nullptr;
		texture->mapped_size = 0;
		texture->mapped_subresources = nullptr;
		texture->mapped_subresource_count = 0;
		texture->sparse_properties = nullptr;
		texture->desc = *desc;
		texture->desc.mip_levels = GetMipCount(texture->desc);

		if (desc->usage == Usage::UPLOAD || desc->usage == Usage::READBACK)
		{
			const size_t size = ComputeTextureMemorySizeInBytes(texture->desc);
			internal_state->memory.resize(size);
			texture->mapped_data = internal_state->memory.data();
			texture->mapped_size = size;
		}
		return true;
	}

	bool GraphicsDevice_Null::CreateShader(ShaderStage stage, const void* shadercode, size_t shadercode_size, Shader* shader, const char* entrypoint) const
	{
		shader->internal_state = wi::allocator::make_shared<Resource_Null>();
		shader->stage = stage;
		return true;
	}

	bool GraphicsDevice_Null::CreateSampler(const SamplerDesc* desc, Sampler* sampler) const
	{
		sampler->internal_state = wi::allocator::make_shared<Resource_Null>();
		sampler->desc = *desc;
		return true;
	}

	bool GraphicsDevice_Null::CreateQueryHeap(const GPUQueryHeapDesc* desc, GPUQueryHeap* queryheap) const
	{
		queryheap->internal_state = wi::allocator::make_shared<Resource_Null>();
		queryheap->desc = *desc;
		return true;
	}

	bool GraphicsDevice_Null::CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso, const RenderPassInfo* renderpass_info) const
	{
		pso->internal_state = wi::allocator::make_shared<Resource_Null>();
		pso->desc = *desc;
		return true;
	}

	CommandList GraphicsDevice_Null::BeginCommandList(QUEUE_TYPE queue)
	{
		std::scoped_lock lck(locker);
		if (cmd_count >= commandlists.size())
		{
			commandlists.push_back(std::make_unique<CommandList_Null>());
		}
		CommandList_Null& c = *commandlists[cmd_count++];
		c.queue = queue;
		c.renderpass_info = {};
		c.frame_allocators[GetBufferIndex()].reset();
		CommandList cmd;
		cmd.internal_state = &c;
		return cmd;
	}

	void GraphicsDevice_Null::SubmitCommandLists()
	{
		std::scoped_lock lck(locker);
		cmd_count = 0;
		FRAMECOUNT++;
	}

	Texture GraphicsDevice_Null::GetBackBuffer(const SwapChain* swapchain) const
	{
		Texture t;
		t.internal_state = swapchain->internal_state;
		t.type = GPUResource::Type::TEXTURE;
		t.desc.width = swapchain->desc.width;
		t.desc.height = swapchain->desc.height;
		t.desc.format = swapchain->desc.format;
		t.desc.bind_flags = BindFlag::RENDER_TARGET;
		t.desc.layout = ResourceState::RENDERTARGET;
		return t;
	}
}
