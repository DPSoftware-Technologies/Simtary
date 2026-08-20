#pragma once
// stb_image is already vendored AND implemented by Wicked Engine
// (Simtary/Utility/stb_image.h, implementation emitted in
// Simtary/Utility/utility_common.cpp → Utility.lib, which Milistry links via
// Simtary). Both copies are stb_image v2.30, so the declarations here match the
// linked implementation exactly.
//
// This forwarding header exists only to give app code a stable top-level
// `#include "stb_image.h"` (include/ is on Milistry's include path) without
// reaching into engine internals and WITHOUT defining a second
// STB_IMAGE_IMPLEMENTATION (that causes LNK2005 multiply-defined stbi_* symbols
// against Utility.lib). Do NOT define STB_IMAGE_IMPLEMENTATION anywhere in src/.
#include "Utility/stb_image.h"
