#include "wiECS.h"

namespace wi::ecs
{
	// The entity id counter lives HERE, in one translation unit, and is declared (not
	// defined) in wiECS.h. See the comment on the declaration: as an inline function its
	// function-local static was one counter per binary, so a host and a project module
	// issued overlapping entity ids.
	Entity CreateEntity()
	{
		static std::atomic<Entity> next{ INVALID_ENTITY + 1 };
		return next.fetch_add(1);
	}
}
