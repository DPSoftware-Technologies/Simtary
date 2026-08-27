// The one translation unit that compiles the vendored zstd amalgamation for stpack.
//
// The engine gets zstd through Engine/Utility/utility_common.cpp, but stpack links no
// engine at all — that is the whole point of it being a build-step tool — so it needs
// its own copy. Same trick as utility_common.cpp: include the .c inside extern "C" so
// the symbols keep C linkage and ASAN does not trip over a mismatched mangling.
//
// Engine/Utility/CMakeLists.txt marks zstd.c HEADER_FILE_ONLY, but that property is
// scoped to that directory, so this target is free to compile it.

extern "C" {
#include "Utility/zstd/zstd.c"
}
