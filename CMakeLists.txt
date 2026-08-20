cmake_minimum_required(VERSION 3.19)

# ══════════════════════════════════════════════════════════════════════════════
# Simtary — shared engine workspace.
#
# Everything more than one game needs lives here: the engine core (Engine/, the
# Simtary4 repo), the framework layer on top of it (Framework/), the vendored
# third-party libraries (libs/), the engine-side assets (assets/) and the crash
# reporter GUI.
#
# A game pulls this in through cmake/SimtaryBootstrap.cmake, which add_subdirectory()s
# it into that game's own build tree — so every project keeps its own
# <project>/build/<platform_arch> and can be configured on its own. What is genuinely
# SHARED lives here and is never duplicated per project:
#
#   deps/        the sentry-native + openal-soft git clones (the slow part of a cold
#                configure); FetchContent reuses them instead of re-cloning
#   shaders/     the compiled engine shader cache, staged into every game's output
#                so no project ever pays the ~360-shader cold compile
#   Engine/, Framework/, libs/, assets/, include/
#
# Build the engine (and optionally every game beside it) standalone with:
#   cmake --preset win_x86-64 && cmake --build --preset win_x86-64
# ══════════════════════════════════════════════════════════════════════════════

# enable "codegen" target if supported by CMake
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.31)
    cmake_policy(SET CMP0171 NEW)
    set(codegen CODEGEN)
endif()

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "In-source builds are not supported!\n"
        "Run `cmake --preset win_x86-64` (or `cmake -B build/<platform_arch>`) instead."
    )
endif()

project(Simtary VERSION 1.0.0 LANGUAGES CXX)

include(CheckIPOSupported)

# Absolute root of the shared engine, usable from any project that pulls this in.
set(SIMTARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Simtary engine root")
list(APPEND CMAKE_MODULE_PATH "${SIMTARY_ROOT}/cmake")
include(SimtaryPlatform)

option(WICKED_DYNAMIC_LIBRARY "Build the engine as a dynamic library" OFF)
option(WICKED_PIC "Build the engine as position-independent code" WICKED_DYNAMIC_LIBRARY)
option(USE_LIBCXX "Link the engine to llvm libc++ library - only available with the Clang compiler" OFF)

option(WICKED_ENABLE_IPO "Enable IPO/LTO in non-debug builds" NO)
option(WICKED_EMBED_SHADERS "Embed shaders into the library" NO)
option(WICKED_ENABLE_RTTI "Enable RTTI" NO)
if(UNIX)
    option(WICKED_ENABLE_ASAN "Enable AddressSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_UBSAN "Enable UndefinedBehaviourSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_TSAN "Enable ThreadSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_SAN_ALWAYS "Enable the selected sanitizers in all builds, not just debug" OFF)
endif()
if(UNIX)
    option(WICKED_LINUX_TEMPLATE "Build the engine Linux template" ON)
elseif(WIN32)
    option(WICKED_WINDOWS_TEMPLATE "Build the engine Windows template" ON)
endif()

if (CMAKE_HOST_WIN32)
    set(symlink_default OFF)
else()
    set(symlink_default ON)
endif()
option(WICKED_ENABLE_SYMLINKS "Prefer symlinking over copying directories" ${symlink_default})

# check that IPO is supported when turned on
if(WICKED_ENABLE_IPO)
    check_ipo_supported()
endif()

if(WICKED_ENABLE_SYMLINKS)
    # check for symlink support (on windows it requires admin or developer mode)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink CMakeLists.txt ${CMAKE_CURRENT_BINARY_DIR}/symlink-test
        ERROR_QUIET
        RESULT_VARIABLE symlink_check_result
    )
    if(symlink_check_result EQUAL 0)
        file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/symlink-test)
        set(COPY_OR_SYMLINK_DIR_CMD create_symlink)
    else()
        message(FATAL_ERROR "Symlinks are not supported. Either disable them using -DWICKED_ENABLE_SYMLINKS=OFF or enable Windows' developer mode")
    endif()
else()
    if(CMAKE_VERSION VERSION_LESS "3.26.0")
        set(COPY_OR_SYMLINK_DIR_CMD copy_directory)
    else()
        set(COPY_OR_SYMLINK_DIR_CMD copy_directory_if_different)
    endif()
endif()

# Configure CMake global variables
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ${WICKED_PIC})

# Use solution folders to organize projects
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ─── Crash reporting: sentry-native (bundles Crashpad), offline / local-only ────
# Fetched BEFORE the project-wide exception/RTTI/security flags below, so this
# third-party code builds with its own (exception-enabled) defaults instead of
# inheriting the engine's /EHsc- /GR- which would break it.
# Captures minidumps to a local "crashreports" folder; no network upload (gated by
# withheld user consent + a no-op transport). A separate cross-platform reporter
# GUI is launched on crash. See Framework/crash/ and crashreporter/.
include(FetchContent)

# ─── Shared dependency source cache ────────────────────────────────────────────
# sentry-native and openal-soft are fetched from git. Cloning them once per project
# build tree is the single slowest part of a cold configure, so the clones live in
# Simtary/deps/ and every project reuses them: FetchContent skips the download when
# FETCHCONTENT_SOURCE_DIR_<NAME> points at an existing tree. Each project still
# BUILDS them in its own build directory (CMake cannot share a configured build tree
# between two top-level projects), but nothing is re-downloaded.
set(SIMTARY_DEPS_DIR "${SIMTARY_ROOT}/deps" CACHE PATH
    "Shared source cache for fetched third-party dependencies")
foreach(_dep sentry openal)
    string(TOUPPER "${_dep}" _depu)
    if (EXISTS "${SIMTARY_DEPS_DIR}/${_dep}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${_depu} "${SIMTARY_DEPS_DIR}/${_dep}" CACHE PATH
            "Pre-populated ${_dep} source (Simtary/deps)" FORCE)
        message(STATUS "Simtary: using cached ${_dep} source at ${SIMTARY_DEPS_DIR}/${_dep}")
    endif()
endforeach()

set(SENTRY_BACKEND           crashpad CACHE STRING "" FORCE)
set(SENTRY_BUILD_SHARED_LIBS OFF      CACHE BOOL   "" FORCE)
set(SENTRY_BUILD_TESTS       OFF      CACHE BOOL   "" FORCE)
set(SENTRY_BUILD_EXAMPLES    OFF      CACHE BOOL   "" FORCE)
FetchContent_Declare(
    sentry
    GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
    GIT_TAG        0.8.5
    GIT_SUBMODULES_RECURSE ON
)
FetchContent_MakeAvailable(sentry)

# ─── ZeroMQ (libzmq): C messaging library — static link, C API only ─────────────
# Added BEFORE the project-wide /EHsc- /GR- /_HAS_EXCEPTIONS=0 flags below (same
# reason as sentry): libzmq is C++ internally and must build with its own
# exception-enabled defaults instead of inheriting the engine's. Static archive →
# no runtime DLL to copy. The libzmq-static target propagates its include dir
# (zmq.h), the ZMQ_STATIC define, and the Windows system libs it needs
# (ws2_32/rpcrt4/iphlpapi) to anything that links it. Only the C API (zmq.h) is
# used from app code; cppzmq is intentionally NOT vendored because it throws and
# the app's translation units have exceptions disabled. Options are pre-seeded as
# FORCE cache entries so libzmq's own option() calls pick them up regardless of
# its policy stack (CMP0077).
set(BUILD_SHARED   OFF CACHE BOOL "" FORCE)  # build the static archive only
set(BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WITH_DOCS      OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK   OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS  OFF CACHE BOOL "" FORCE)  # stable API surface only
# libzmq v4.3.5 declares cmake_minimum_required(VERSION 3.0.2); CMake 4.x dropped
# <3.5 compatibility. Provide the floor for libzmq's scope only, then restore.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(libs/libzmq)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ─── OpenAL Soft: real-time audio output for the engine's Faust DSP streaming ───
# Added BEFORE the project-wide /EHsc- /GR- /_HAS_EXCEPTIONS=0 flags below (same
# reason as sentry/libzmq): openal-soft is C++ and uses exceptions/RTTI internally,
# so it must build with its own defaults instead of inheriting exceptions-off.
# Built as a shared library (OpenAL32.dll) that is copied next to the executable
# like SDL2.dll. Only the C API (AL/al.h, AL/alc.h) is consumed from engine code
# (Engine/stAudio.cpp), so the exceptions-off app TUs are unaffected. stAudio is
# the OpenAL audio subsystem meant to eventually replace the FAudio-based
# wi::audio path; for now it runs alongside it, driving a separate DSP output
# stream.
set(ALSOFT_UTILS          OFF CACHE BOOL "" FORCE)  # no makemhr/openal-info tools
set(ALSOFT_EXAMPLES       OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS          OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL        OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA   OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    openal
    GIT_REPOSITORY https://github.com/kcat/openal-soft.git
    GIT_TAG        1.24.2
)
FetchContent_MakeAvailable(openal)

if(WICKED_ENABLE_ASAN AND WICKED_ENABLE_TSAN)
    message(FATAL_ERROR "WICKED_ENABLE_ASAN and WICKED_ENABLE_TSAN are mutually exclusive!")
endif()

if(WICKED_ENABLE_ASAN)
    set(SANITIZE_OPTIONS "-fsanitize=address")
endif()

if(WICKED_ENABLE_TSAN)
    set(SANITIZE_OPTIONS "${SANITIZE_OPTIONS} -fsanitize=thread")
endif()

if(WICKED_ENABLE_UBSAN)
    set(SANITIZE_OPTIONS "${SANITIZE_OPTIONS} -fsanitize=undefined")
endif()

if(WICKED_ENABLE_SAN_ALWAYS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_OPTIONS}")
    add_link_options(${SANITIZE_LINK_OPTIONS})
else()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZE_OPTIONS}")
    add_link_options($<$<CONFIG:Debug>:${SANITIZE_LINK_OPTIONS}>)
endif()

# ─── Project-wide code-generation contract ─────────────────────────────────────
# Exceptions and RTTI are OFF everywhere from here down. Engine, ImGui and app
# code must all agree on this, otherwise their objects are not ABI-compatible.
#
# The same flags are also stashed on the Simtary::AppFlags INTERFACE target below,
# because a game's CMakeLists.txt is a sibling directory (Milistry/, template/)
# and so does NOT inherit these directory-scope settings. simtary_add_app() links
# that target for you.
set(SIMTARY_APP_DEFINITIONS "")
set(SIMTARY_APP_OPTIONS "")

if (WIN32)
    set(PLATFORM "Windows")
    list(APPEND SIMTARY_APP_DEFINITIONS WIN32=1 _HAS_EXCEPTIONS=0 SDL2=1)
elseif (UNIX)
    set(PLATFORM "SDL2")
    list(APPEND SIMTARY_APP_DEFINITIONS SDL2=1 _GLIBCXX_USE_CXX11_ABI=1)
endif()

if (MSVC)
    list(APPEND SIMTARY_APP_OPTIONS
        /utf-8
        /W3 # warning level 3
        /MP # multi-processor compilation
        /EHsc- # exceptions disabled
        $<$<AND:$<NOT:$<BOOL:${ubsan_active}>>,$<NOT:$<BOOL:${WICKED_ENABLE_RTTI}>>>:/GR-> # runtime type information disabled
        $<$<CONFIG:Release>:/GS-> # security check disabled in Release
    )
else()
    set(ubsan_active
         "$<AND:$<BOOL:${WICKED_ENABLE_UBSAN}>,$<OR:$<CONFIG:Debug>,$<BOOL:${WICKED_ENABLE_SAN_ALWAYS}>>>"
    )
    # Common compiler options and warning level for CLANG and GCC:
    list(APPEND SIMTARY_APP_OPTIONS
        -Wall # warning level: all

        # some warnings are disabled to better match MSVC warning level 3:
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-sign-compare

        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> # exceptions disabled
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<BOOL:${ubsan_active}>>,$<NOT:$<BOOL:${WICKED_ENABLE_RTTI}>>>:-fno-rtti> # runtime type information disabled

        # security checks disabled in Release:
        $<$<CONFIG:Release>:-fno-stack-protector>
        $<$<CONFIG:Release>:-fcf-protection=none>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:Windows>>>:-fno-stack-clash-protection> # not supported on Windows
        $<$<CONFIG:Release>:-fno-stack-check>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:Windows>>>:-fno-asynchronous-unwind-tables>  # seems to crash LUA evaluation on Windows
    )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # CLANG specific compile options:
    list(APPEND SIMTARY_APP_OPTIONS
        -fdeclspec
        -fms-extensions

        -Wno-nullability-completeness
        -Wno-unused-private-field

        # Increase Ccache hit rate, note that you also need to set config
        # options in your Ccache config
        # see https://ccache.dev/manual/latest.html#_precompiled_headers
        "SHELL:-Xclang -fno-pch-timestamp"
    )
    if (USE_LIBCXX)
        list(APPEND SIMTARY_APP_OPTIONS $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
        add_link_options(-stdlib=libc++)
    endif()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options:
    list(APPEND SIMTARY_APP_OPTIONS -Wno-strict-aliasing)
endif()

add_compile_definitions(${SIMTARY_APP_DEFINITIONS})
add_compile_options(${SIMTARY_APP_OPTIONS})

add_library(Simtary_AppFlags INTERFACE)
add_library(Simtary::AppFlags ALIAS Simtary_AppFlags)
# SDL_MAIN_HANDLED is app-only, NOT global: every project owns a plain int main() in
# its own src/main.cpp and SDL must not #define main away there. Setting it globally
# would also reach SDL's own SDL2main sources, which are what implement that wrapper.
target_compile_definitions(Simtary_AppFlags INTERFACE ${SIMTARY_APP_DEFINITIONS} SDL_MAIN_HANDLED)
target_compile_options(Simtary_AppFlags INTERFACE ${SIMTARY_APP_OPTIONS})
target_compile_features(Simtary_AppFlags INTERFACE cxx_std_17)

# ─── Engine + vendored libraries ───────────────────────────────────────────────
add_subdirectory(libs/SDL2)
add_subdirectory(Engine)        # the Simtary4 engine core; defines the `Simtary` target

# ─── libgfx: Adafruit-GFX-compatible software 2D rasterizer ────────────────────
# Added AFTER the project-wide /EHsc- /GR- flags (unlike sentry/libzmq/openal): it
# uses neither exceptions nor RTTI, so inheriting them is correct and keeps its
# objects ABI-consistent with the rest of the app.
#
# Must come after libs/SDL2 so the SDL2::SDL2 target already exists — libgfx picks
# it up instead of running find_package() (see libs/libgfx/CMakeLists.txt).
#
# Two entry points, both compiled in:
#   GFXcanvas — off-screen ARGB8888/RGB565/… surface, no device. This is the one
#               that composes with game code: draw into it, then upload the buffer.
#   LinuxGFX  — owns a backend. Under GFXSDL it opens its OWN SDL window, which
#               would sit alongside the engine's, so it is not used by games.
set(GFX_DRM_SUPPORT OFF CACHE BOOL "" FORCE)  # Linux-only backend
set(GFX_SDL_SUPPORT ON  CACHE BOOL "" FORCE)  # required on Windows; also the GFXcanvas path
add_subdirectory(libs/libgfx)

# ─── Dear ImGui (+ ImGuizmo) ───────────────────────────────────────────────────
set(IMGUI_FILES
    libs/ImGui/imconfig.h
    libs/ImGui/imgui.cpp
    libs/ImGui/imgui.h
    libs/ImGui/imgui_demo.cpp
    libs/ImGui/imgui_draw.cpp
    libs/ImGui/imgui_internal.h
    libs/ImGui/imgui_tables.cpp
    libs/ImGui/imgui_widgets.cpp
    libs/ImGui/imstb_rectpack.h
    libs/ImGui/imstb_textedit.h
    libs/ImGui/imstb_truetype.h
    libs/ImGui/backends/imgui_impl_sdl2.cpp
    libs/ImGui/backends/imgui_impl_sdl2.h
)

# ImGuizmo: gizmo + node/curve/sequencer widgets. Only depends on imgui.h, so it
# builds inside ImGui_Lib (shares the ImGui include dir).
set(IMGUIZMO_FILES
    libs/ImGuizmo/src/ImGuizmo.cpp
    libs/ImGuizmo/src/ImGuizmo.h
    libs/ImGuizmo/src/GraphEditor.cpp
    libs/ImGuizmo/src/GraphEditor.h
    libs/ImGuizmo/src/ImCurveEdit.cpp
    libs/ImGuizmo/src/ImCurveEdit.h
    libs/ImGuizmo/src/ImGradient.cpp
    libs/ImGuizmo/src/ImGradient.h
    libs/ImGuizmo/src/ImSequencer.cpp
    libs/ImGuizmo/src/ImSequencer.h
    libs/ImGuizmo/src/ImZoomSlider.h
)

# STATIC, not OBJECT: an OBJECT library's objects have to be added to every target
# that uses it, which does not survive being consumed from a sibling project.
add_library(ImGui_Lib STATIC ${IMGUI_FILES} ${IMGUIZMO_FILES})
target_include_directories(ImGui_Lib PUBLIC
    ${SIMTARY_ROOT}/libs/ImGui
    ${SIMTARY_ROOT}/libs/ImGui/backends
    ${SIMTARY_ROOT}/libs/ImGuizmo/src
    ${SIMTARY_ROOT}/Engine
)
target_link_libraries(ImGui_Lib PRIVATE SDL2::SDL2)
set_target_properties(ImGui_Lib PROPERTIES FOLDER "Simtary")

# ─── Crash reporter GUI (separate process, shown after a crash) ────────────────
# One reporter binary serves every project; simtary_add_app() copies it next to
# the game executable.
add_executable(SimtaryCrashReporter crashreporter/main.cpp)
# stb_image.h (header-only PNG decoder) for the crash window icon/branding.
target_include_directories(SimtaryCrashReporter PRIVATE ${SIMTARY_ROOT}/Engine/Utility)
if (WIN32)
    # Windows uses the native Win32 message box (user32) — no SDL dependency.
    target_link_libraries(SimtaryCrashReporter PRIVATE user32 shell32)
endif()
target_link_libraries(SimtaryCrashReporter PRIVATE SDL2::SDL2)
set_target_properties(SimtaryCrashReporter PROPERTIES FOLDER "Simtary")

# crashreportico.png must sit next to the reporter exe (SDL_GetBasePath at runtime).
add_custom_command(TARGET SimtaryCrashReporter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SIMTARY_ROOT}/crashreporter/crashreportico.png
        $<TARGET_FILE_DIR:SimtaryCrashReporter>
    COMMENT "Copying crashreportico.png next to SimtaryCrashReporter"
    VERBATIM
)

if (MSVC)
    # GUI app (no console window) while keeping a plain int main()
    target_link_options(SimtaryCrashReporter PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# ─── simtary_add_app() ─────────────────────────────────────────────────────────
include(SimtaryApp)

# ─── Framework tests + tools ───────────────────────────────────────────────────
# Small console executables exercising self-contained framework modules under the
# same /EHsc- /GR- flags as a game. Run via the exe or `ctest`.
option(SIMTARY_BUILD_TESTS "Build the framework test executables" ON)
if (SIMTARY_BUILD_TESTS)
    enable_testing()

    add_executable(nbt_test tests/nbt_test.cpp Framework/io/Nbt.cpp)
    target_include_directories(nbt_test PRIVATE Framework)
    target_link_libraries(nbt_test PRIVATE Simtary::AppFlags)
    set_target_properties(nbt_test PROPERTIES FOLDER "Simtary/Tests")
    add_test(NAME nbt_test COMMAND nbt_test)

    # Generator for the sample player animation descriptor (writes an NBT file). Not
    # run automatically; build + run it from a project root to (re)create one.
    add_executable(make_player_descriptor tools/make_player_descriptor.cpp Framework/io/Nbt.cpp)
    target_include_directories(make_player_descriptor PRIVATE Framework)
    target_link_libraries(make_player_descriptor PRIVATE Simtary::AppFlags)
    set_target_properties(make_player_descriptor PROPERTIES FOLDER "Simtary/Tools")
endif()

# ─── Games in this workspace ───────────────────────────────────────────────────
# Only when Simtary is the top-level project: `cmake --preset win_x86-64` here
# builds the engine plus every game beside it, which is the fast way to check that
# a framework change still compiles everywhere. Each game can equally be configured
# on its own (that is the normal workflow) — see cmake/SimtaryBootstrap.cmake.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # top-level: CMake 3.19 has no PROJECT_IS_TOP_LEVEL
    option(SIMTARY_BUILD_PROJECTS "Also build the sibling game projects" OFF)
    if (SIMTARY_BUILD_PROJECTS)
        get_filename_component(_workspace ${SIMTARY_ROOT} DIRECTORY)
        file(GLOB _candidates RELATIVE ${_workspace} ${_workspace}/*)
        foreach(_c ${_candidates})
            if (NOT _c STREQUAL "Simtary" AND EXISTS ${_workspace}/${_c}/CMakeLists.txt)
                message(STATUS "Simtary: adding project ${_c}")
                add_subdirectory(${_workspace}/${_c} ${CMAKE_BINARY_DIR}/projects/${_c})
            endif()
        endforeach()
    endif()
endif()
