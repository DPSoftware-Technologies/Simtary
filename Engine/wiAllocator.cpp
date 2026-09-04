#include "wiAllocator.h"

namespace wi::allocator
{
	// The ONE table for the process. See the comment on the declarations in
	// wiAllocator.h for why this cannot be an inline variable in the header.
	//
	// Function-local statics rather than namespace-scope ones, because a
	// SharedBlockAllocator registers itself from its own dynamic initializer: with
	// namespace-scope objects that is a static initialization order dependency across
	// translation units, and it is the kind that is only noticed once it has gone
	// wrong. A local static is constructed on first use, which is exactly the first
	// registration.
	SharedAllocator** shared_allocator_table()
	{
		static SharedAllocator* allocators[256] = {};
		return allocators;
	}

	static std::atomic<uint8_t>& allocator_counter()
	{
		static std::atomic<uint8_t> next_id{ 0 };
		return next_id;
	}

	uint8_t register_shared_allocator(SharedAllocator* allocator)
	{
		const uint8_t id = allocator_counter().fetch_add(1);
		shared_allocator_table()[id] = allocator;
		return id;
	}

	uint8_t get_shared_allocator_count()
	{
		return allocator_counter().load();
	}
}
