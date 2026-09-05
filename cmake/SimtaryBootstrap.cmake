# SimtaryBootstrap.cmake - the one line a game's CMakeLists.txt needs.
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../Simtary/cmake/SimtaryBootstrap.cmake)
#
# After that, simtary_add_app() and simtary_compile_shader() are available and every
# engine target (Simtary, SDL2::SDL2, ImGui_Lib, sentry::sentry, OpenAL, libgfx,
# libzmq-static, SimtaryCrashReporter, offlineshadercompiler) exists.
#
# The engine is add_subdirectory()'d into this project's build tree under _simtary/.
# That is deliberate: two independent top-level CMake projects cannot share one
# configured build directory, so what gets shared is everything *upstream* of the
# compile - the sources, the fetched dependency clones (Simtary/deps) and the
# compiled engine shader cache (Simtary/shaders). Each game still owns its own
# <project>/build/<platform_arch>, which is what makes them independently
# configurable and separately shippable.
#
# Set SIMTARY_ROOT_DIR before including this to point at a Simtary checkout that is
# not the sibling directory.

include_guard(GLOBAL)

if (NOT SIMTARY_ROOT_DIR)
    get_filename_component(SIMTARY_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if (NOT EXISTS "${SIMTARY_ROOT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Simtary engine not found at '${SIMTARY_ROOT_DIR}'.\n"
        "Point SIMTARY_ROOT_DIR at your Simtary checkout, e.g.\n"
        "  cmake --preset win_x86-64 -DSIMTARY_ROOT_DIR=E:/path/to/Simtary")
endif()

list(APPEND CMAKE_MODULE_PATH "${SIMTARY_ROOT_DIR}/cmake")
include(SimtaryPlatform)

if (NOT TARGET Simtary)
    # EXCLUDE_FROM_ALL: building the game must not also build the engine's own test
    # and tool targets. Everything the game links is pulled in by dependency.
    add_subdirectory(${SIMTARY_ROOT_DIR} ${CMAKE_BINARY_DIR}/_simtary EXCLUDE_FROM_ALL)
endif()
