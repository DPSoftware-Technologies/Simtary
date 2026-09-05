cmake_minimum_required(VERSION 3.19)

# Simtary - shared engine workspace.
#
# Everything more than one game needs lives here: the engine core (Engine/, the
# Simtary4 repo), the framework layer on top of it (Framework/), the vendored
# third-party libraries (libs/), the engine-side assets (assets/) and the crash
# reporter GUI.
#
# A game pulls this in through cmake/SimtaryBootstrap.cmake, which add_subdirectory()s
# it into that game's own build tree - so every project keeps its own
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

# C is listed alongside CXX deliberately. LUA and SimtaryModelIO are C targets, and a
# language that project() does not declare never gets its MSVC runtime-library
# abstraction initialised - those two then came out with an empty <RuntimeLibrary> and
# MSBuild's own default, the static CRT. See the CMAKE_MSVC_RUNTIME_LIBRARY block below.
project(Simtary VERSION 1.0.0 LANGUAGES C CXX)

include(CheckIPOSupported)

# Absolute root of the shared engine, usable from any project that pulls this in.
set(SIMTARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Simtary engine root")
list(APPEND CMAKE_MODULE_PATH "${SIMTARY_ROOT}/cmake")
include(SimtaryPlatform)

# Absolute entity positions in double instead of float. ON gives a 100x100 km world
# sub-micrometre precision at its far corner, at the cost of 12 bytes per
# TransformComponent and one double subtract per transform update. Turn it OFF for
# small-world projects, and on targets where fp64 is emulated or rate-limited - mobile
# GPUs and CPUs above all. Scene files stay compatible either way: the absolute position
# is always double on disk. See Engine/stWorldScalar.h.
option(SIMTARY_LARGE_WORLD "Store absolute world positions in 64-bit precision" ON)

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

# One C runtime for the whole workspace, stated rather than inherited.
#
# Before this, project() declared CXX only and the two C targets (LUA,
# SimtaryModelIO) came out with an EMPTY <RuntimeLibrary> - so MSBuild gave them its
# own default, the STATIC CRT, while every C++ target used the dynamic one. The
# visible symptom was just "LNK4098: defaultlib 'LIBCMT' conflicts". The invisible one
# is worse: two CRTs in one link means two heaps, and a block allocated by one and
# freed through the other corrupts them both. That is fatal for
# simtary_add_app(MODULE), where a project module and its host pass std:: containers
# across a DLL boundary constantly.
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Use solution folders to organize projects
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Crash reporting: sentry-native (bundles Crashpad), offline / local-only
# Fetched BEFORE the project-wide exception/RTTI/security flags below, so this
# third-party code builds with its own (exception-enabled) defaults instead of
# inheriting the engine's /EHsc- /GR- which would break it.
# Captures minidumps to a local "crashreports" folder; no network upload (gated by
# withheld user consent + a no-op transport). A separate cross-platform reporter
# GUI is launched on crash. See Framework/crash/ and crashreporter/.
include(FetchContent)

# Shared dependency source cache
# sentry-native and openal-soft are fetched from git. Cloning them once per project
# build tree is the single slowest part of a cold configure, so the clones live in
# Simtary/deps/ and every project reuses them: FetchContent skips the download when
# FETCHCONTENT_SOURCE_DIR_<NAME> points at an existing tree. Each project still
# BUILDS them in its own build directory (CMake cannot share a configured build tree
# between two top-level projects), but nothing is re-downloaded.
set(SIMTARY_DEPS_DIR "${SIMTARY_ROOT}/deps" CACHE PATH
    "Shared source cache for fetched third-party dependencies")
# The marker is what proves a clone finished rather than a directory merely existing.
# CMakeLists.txt serves for the two CMake projects; ufbx has none (it is two source
# files), so it is recognised by a file it does own.
set(_dep_markers "sentry=CMakeLists.txt" "openal=CMakeLists.txt"
                 "tinygltf=CMakeLists.txt" "ufbx=ufbx.c"
                 "steamaudio=include/phonon.h")
foreach(_entry IN LISTS _dep_markers)
    string(REPLACE "=" ";" _parts "${_entry}")
    list(GET _parts 0 _dep)
    list(GET _parts 1 _marker)
    string(TOUPPER "${_dep}" _depu)
    if (EXISTS "${SIMTARY_DEPS_DIR}/${_dep}/${_marker}")
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

# ZeroMQ (libzmq): C messaging library - static link, C API only
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

# OpenAL Soft: real-time audio output for the engine's Faust DSP streaming
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

# Steam Audio: the 3D audio layer (core SDK only)
# Steam Audio owns everything spatial - HRTF, distance, air absorption, occlusion,
# transmission, geometry-driven reflections and pathing - while OpenAL Soft above
# stays the output device and the 2D mixer. Engine/stAudioSpatial.cpp is the only
# translation unit that sees phonon.h.
#
# CORE ONLY, and deliberately from the release archive rather than a git clone:
# Valve ships the core SDK (phonon.h + phonon.dll) as its own zip, separate from the
# Unity / Unreal / FMOD / Wwise integrations, which is exactly the "core only" split
# wanted here. Building core/ from the git tree additionally needs Valve's own
# dependency bootstrap (flatbuffers, pffft, mysofa, IPP, Embree) fetched by scripts
# under core/build - a much slower and far more fragile cold configure for a library
# that is shipped prebuilt for every platform this engine targets.
#
# It lands in the same shared deps cache as sentry/openal, so it is downloaded once
# per workspace rather than once per project.
set(SIMTARY_STEAMAUDIO_VERSION "4.6.1" CACHE STRING "Steam Audio core SDK version")
option(SIMTARY_ENABLE_STEAMAUDIO "Fetch Steam Audio and build the 3D audio path against it (OFF falls back to distance attenuation plus stereo panning)" ON)

if (SIMTARY_ENABLE_STEAMAUDIO)
    FetchContent_Declare(
        steamaudio
        URL "https://github.com/ValveSoftware/steam-audio/releases/download/v${SIMTARY_STEAMAUDIO_VERSION}/steamaudio_${SIMTARY_STEAMAUDIO_VERSION}.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(steamaudio)

    # The archive unpacks to a "steamaudio" directory holding include/ and lib/.
    set(_sa_root "${steamaudio_SOURCE_DIR}")
    if (EXISTS "${_sa_root}/steamaudio/include/phonon.h")
        set(_sa_root "${_sa_root}/steamaudio")
    endif()

    # Platform subdirectory inside lib/. Only the desktop targets this workspace
    # builds for are mapped; add android-*/ios here if a project ever needs them.
    if (WIN32)
        if (CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_sa_libdir "windows-x64")
        else()
            set(_sa_libdir "windows-x86")
        endif()
        set(_sa_shared "${_sa_root}/lib/${_sa_libdir}/phonon.dll")
        set(_sa_implib "${_sa_root}/lib/${_sa_libdir}/phonon.lib")
    elseif (APPLE)
        set(_sa_libdir "osx")
        set(_sa_shared "${_sa_root}/lib/${_sa_libdir}/libphonon.dylib")
        set(_sa_implib "${_sa_shared}")
    else()
        set(_sa_libdir "linux-x64")
        set(_sa_shared "${_sa_root}/lib/${_sa_libdir}/libphonon.so")
        set(_sa_implib "${_sa_shared}")
    endif()

    if (EXISTS "${_sa_shared}" AND EXISTS "${_sa_root}/include/phonon.h")
        add_library(SteamAudio SHARED IMPORTED GLOBAL)
        set_target_properties(SteamAudio PROPERTIES
            IMPORTED_LOCATION "${_sa_shared}"
            IMPORTED_IMPLIB   "${_sa_implib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_sa_root}/include"
            INTERFACE_COMPILE_DEFINITIONS "SIMTARY_HAS_STEAMAUDIO=1"
        )
        # Consumed by SimtaryApp.cmake to stage the runtime next to the executable,
        # the same way OpenAL32.dll and SDL2.dll are.
        set(SIMTARY_STEAMAUDIO_RUNTIME "${_sa_shared}" CACHE INTERNAL "Steam Audio runtime library")
        set(SIMTARY_STEAMAUDIO_FOUND TRUE CACHE INTERNAL "Steam Audio is available")
        message(STATUS "Simtary: Steam Audio ${SIMTARY_STEAMAUDIO_VERSION} (${_sa_libdir})")
    else()
        set(SIMTARY_STEAMAUDIO_FOUND FALSE CACHE INTERNAL "Steam Audio is available")
        message(WARNING "Simtary: Steam Audio archive unpacked but no library for this platform at "
                        "${_sa_shared} - 3D audio falls back to stereo panning.")
    endif()
else()
    set(SIMTARY_STEAMAUDIO_FOUND FALSE CACHE INTERNAL "Steam Audio is available")
    message(STATUS "Simtary: Steam Audio disabled - 3D audio falls back to stereo panning.")
endif()

# Model importers: tinygltf (glTF/GLB) + ufbx (FBX/OBJ/MTL)
# Both are C, and that is why these two were picked over the obvious alternatives.
# This workspace builds with /EHsc- /GR- and _HAS_EXCEPTIONS=0 (see the app flags
# below), so a C++ loader that throws - tinygltf v2 on top of nlohmann/json, say
# would either have to be compiled with its own exception settings and marshalled
# across a C boundary, or run with JSON_NOEXCEPTION, which turns a malformed file
# into std::abort(). A C parser has neither problem: it reports errors by return
# code, which is the same contract Framework/io already uses.
#
#   tinygltf v3  pure-C, POD structs, arena allocation, no RTTI/exceptions
#   ufbx         pure C, and it reads Wavefront OBJ/MTL as well as FBX, so the two
#                libraries cover all four extensions between them
#
# SOURCE_SUBDIR points at a directory that does not exist ON PURPOSE: it stops
# FetchContent from add_subdirectory()ing the upstream projects, which would build
# their examples, tests and tools. Only the sources are wanted, and they are compiled
# into SimtaryModelIO below.
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        release
    SOURCE_SUBDIR  simtary-no-add-subdirectory
)
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG        master
    SOURCE_SUBDIR  simtary-no-add-subdirectory
)
FetchContent_MakeAvailable(tinygltf ufbx)

# One static library for both, compiled as C with warnings off: they are third-party
# and their diagnostics would bury the engine's under a few hundred of their own.
add_library(SimtaryModelIO STATIC
    ${tinygltf_SOURCE_DIR}/tiny_gltf_v3.c
    ${ufbx_SOURCE_DIR}/ufbx.c
)
target_include_directories(SimtaryModelIO PUBLIC
    ${tinygltf_SOURCE_DIR}
    ${ufbx_SOURCE_DIR}
)
set_target_properties(SimtaryModelIO PROPERTIES C_STANDARD 11 FOLDER "Simtary/ThirdParty")
if (MSVC)
    target_compile_options(SimtaryModelIO PRIVATE /w)
else()
    target_compile_options(SimtaryModelIO PRIVATE -w)
endif()
# Neither library is asked to touch the filesystem or decode an image on its own: the
# importers feed them bytes that came through wi::helper::FileRead, so a model inside a
# mounted asset package loads exactly like one on disk.
target_compile_definitions(SimtaryModelIO PUBLIC
    UFBX_NO_STDIO=1
    SIMTARY_HAS_MODEL_IMPORTERS=1
)

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

# Project-wide code-generation contract
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

# Engine + vendored libraries
add_subdirectory(libs/SDL2)
add_subdirectory(Engine)        # the Simtary4 engine core; defines the `Simtary` target

# libgfx: Adafruit-GFX-compatible software 2D rasterizer
# Added AFTER the project-wide /EHsc- /GR- flags (unlike sentry/libzmq/openal): it
# uses neither exceptions nor RTTI, so inheriting them is correct and keeps its
# objects ABI-consistent with the rest of the app.
#
# Must come after libs/SDL2 so the SDL2::SDL2 target already exists - libgfx picks
# it up instead of running find_package() (see libs/libgfx/CMakeLists.txt).
#
# Two entry points, both compiled in:
#   GFXcanvas - off-screen ARGB8888/RGB565/… surface, no device. This is the one
#               that composes with game code: draw into it, then upload the buffer.
#   LinuxGFX  - owns a backend. Under GFXSDL it opens its OWN SDL window, which
#               would sit alongside the engine's, so it is not used by games.
set(GFX_DRM_SUPPORT OFF CACHE BOOL "" FORCE)  # Linux-only backend
set(GFX_SDL_SUPPORT ON  CACHE BOOL "" FORCE)  # required on Windows; also the GFXcanvas path
add_subdirectory(libs/libgfx)

# Dear ImGui (+ ImGuizmo)
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

# Crash reporter GUI (separate process, shown after a crash)
# One reporter binary serves every project; simtary_add_app() copies it next to
# the game executable.
add_executable(SimtaryCrashReporter crashreporter/main.cpp)
# stb_image.h (header-only PNG decoder) for the crash window icon/branding.
target_include_directories(SimtaryCrashReporter PRIVATE ${SIMTARY_ROOT}/Engine/Utility)
if (WIN32)
    # Windows uses the native Win32 message box (user32) - no SDL dependency.
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

# simtary_add_app()
include(SimtaryApp)

# stpack: the asset packer
# Converts .wiscene maps + loose content into .stsd / .strd / .stafp<N> at BUILD time
# (simtary_pack_assets in cmake/SimtaryApp.cmake drives it), and inspects or reverses
# the result by hand.
#
# Not inside SIMTARY_BUILD_TESTS: a game configures this workspace with
# EXCLUDE_FROM_ALL, so nothing here is built unless something depends on it - and a
# game's pack target does. Gating it on the test option would make the tool vanish for
# exactly the projects that need it.
#
# It links Framework/io only, never the engine, so it builds in seconds and can run
# before any engine target exists. zstd is compiled into it directly for the same
# reason (tools/stpack_zstd.cpp).
add_executable(stpack
    tools/stpack.cpp
    tools/stpack_zstd.cpp
    Framework/io/Nbt.cpp
    Framework/io/asset/AssetPack.cpp
    Framework/io/asset/AssetPackWriter.cpp
    Framework/io/asset/SceneDescriptor.cpp
)
target_include_directories(stpack PRIVATE Framework Engine)
target_link_libraries(stpack PRIVATE Simtary::AppFlags)
set_target_properties(stpack PROPERTIES FOLDER "Simtary/Tools")
# zstd is third-party C: it must not inherit the /W3 the app flags carry, or every
# build of it buries the real warnings under a few hundred of its own.
if (MSVC)
    set_source_files_properties(tools/stpack_zstd.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(tools/stpack_zstd.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Framework tests + tools
# Small console executables exercising self-contained framework modules under the
# same /EHsc- /GR- flags as a game. Run via the exe or `ctest`.
option(SIMTARY_BUILD_TESTS "Build the framework test executables" ON)
if (SIMTARY_BUILD_TESTS)
    enable_testing()

    # The third-party half of Framework/io/model: tinygltf and ufbx, no engine and no
    # graphics device. What breaks on a dependency bump is the API shape and the parse
    # options, and none of that needs a device to check.
    add_executable(model_import_test tests/model_import_test.cpp)
    target_link_libraries(model_import_test PRIVATE Simtary::AppFlags SimtaryModelIO)
    set_target_properties(model_import_test PROPERTIES FOLDER "Simtary/Tests")
    add_test(NAME model_import_test COMMAND model_import_test)

    add_executable(nbt_test tests/nbt_test.cpp Framework/io/Nbt.cpp)
    target_include_directories(nbt_test PRIVATE Framework)
    target_link_libraries(nbt_test PRIVATE Simtary::AppFlags)
    set_target_properties(nbt_test PROPERTIES FOLDER "Simtary/Tests")
    add_test(NAME nbt_test COMMAND nbt_test)

    # Round-trips the asset package formats in memory: pack -> read back -> verify, and
    # .wiscene split -> merge -> byte-identical. No engine, no files beyond a temp dir.
    add_executable(asset_pack_test
        tests/asset_pack_test.cpp
        tools/stpack_zstd.cpp
        Framework/io/Nbt.cpp
        Framework/io/asset/AssetPack.cpp
        Framework/io/asset/AssetPackWriter.cpp
        Framework/io/asset/SceneDescriptor.cpp
    )
    target_include_directories(asset_pack_test PRIVATE Framework Engine)
    target_link_libraries(asset_pack_test PRIVATE Simtary::AppFlags)
    set_target_properties(asset_pack_test PROPERTIES FOLDER "Simtary/Tests")
    add_test(NAME asset_pack_test COMMAND asset_pack_test)

    # Generator for the sample player animation descriptor (writes an NBT file). Not
    # run automatically; build + run it from a project root to (re)create one.
    add_executable(make_player_descriptor tools/make_player_descriptor.cpp Framework/io/Nbt.cpp)
    target_include_directories(make_player_descriptor PRIVATE Framework)
    target_link_libraries(make_player_descriptor PRIVATE Simtary::AppFlags)
    set_target_properties(make_player_descriptor PROPERTIES FOLDER "Simtary/Tools")

    # Authoring tool for the build-time project manifest (assets/project.stpd). The
    # descriptor is NBT, so this is how a project edits it; it links Nbt.cpp only, so
    # it builds in seconds without the engine.
    add_executable(make_project_descriptor tools/make_project_descriptor.cpp Framework/io/Nbt.cpp)
    target_include_directories(make_project_descriptor PRIVATE Framework)
    target_link_libraries(make_project_descriptor PRIVATE Simtary::AppFlags)
    set_target_properties(make_project_descriptor PROPERTIES FOLDER "Simtary/Tools")
endif()

# Games in this workspace
# Only when Simtary is the top-level project: `cmake --preset win_x86-64` here
# builds the engine plus every game beside it, which is the fast way to check that
# a framework change still compiles everywhere. Each game can equally be configured
# on its own (that is the normal workflow) - see cmake/SimtaryBootstrap.cmake.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # top-level: CMake 3.19 has no PROJECT_IS_TOP_LEVEL
    option(SIMTARY_BUILD_PROJECTS "Also build the sibling game projects" OFF)
    if (SIMTARY_BUILD_PROJECTS)
        # This is a compile-check of the workspace, not a build OF those games, so it
        # must not touch their project-level build counters.
        set(SIMTARY_BUMP_BUILD_NUMBER OFF CACHE BOOL "" FORCE)
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
