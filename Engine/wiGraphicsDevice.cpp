#include "wiGraphicsDevice.h"

namespace wi::graphics
{
	// The global device pointer lives HERE, in one translation unit, and is declared
	// (not defined) in wiGraphicsDevice.h. See the comment on the declaration: as an
	// inline function its function-local static was one instance per binary, so a DLL
	// held a second pointer that nothing ever set.
	GraphicsDevice*& GetDevice()
	{
		static GraphicsDevice* device = nullptr;
		return device;
	}
}
