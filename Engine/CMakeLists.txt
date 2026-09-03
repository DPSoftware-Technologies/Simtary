cmake_minimum_required(VERSION 3.19)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.31)
    cmake_policy(SET CMP0171 NEW)
    set(codegen CODEGEN)
endif()

set(MIN_OpenImageDenoise_VERSION "2.0")

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
set(INSTALL_LOCAL_CONFIGDIR "${CMAKE_BINARY_DIR}/cmake")
set(INSTALL_CONFIGDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Simtary")

# Use same options as Jolt
# Select X86 processor features to use (if everything is off it will be SSE2 compatible)
option(USE_SSE4_1 "Enable SSE4.1" ON)
option(USE_SSE4_2 "Enable SSE4.2" ON)
option(USE_AVX "Enable AVX" ON)
option(USE_AVX2 "Enable AVX2" OFF)
option(USE_AVX512 "Enable AVX512" OFF)
option(USE_LZCNT "Enable LZCNT" ON)
option(USE_TZCNT "Enable TZCNT" ON)
option(USE_F16C "Enable F16C" ON)
option(USE_FMADD "Enable FMADD" ON)

if (WICKED_DYNAMIC_LIBRARY)
    if (WIN32)
        message(WARNING "WICKED_DYNAMIC_LIBRARY is not supported on Windows/MSVC (no dllexport annotations). Falling back to static.")
        set(WICKED_LIBRARY_TYPE STATIC)
    else()
        set(WICKED_LIBRARY_TYPE SHARED)
        message(STATUS "Building Simtary as a shared library")
    endif()
else()
    set(WICKED_LIBRARY_TYPE STATIC)
    message(STATUS "Building Simtary as a static library")
endif()

if (WIN32)
    if (MSVC)
        add_compile_options(
            /bigobj
        )
    endif()
else ()
    find_package(SDL2 REQUIRED)
endif()

if (NOT WIN32)
    find_package(SDL2 REQUIRED)
    find_package(OpenImageDenoise "${MIN_OpenImageDenoise_VERSION}" QUIET)
    find_package(Threads REQUIRED)
    if(NOT ${OpenImageDenoise_FOUND})
        message("OpenImageDenoise>=${MIN_OpenImageDenoise_VERSION} not found, it will be disabled.")
    else()
        message("OpenImageDenoise ${OpenImageDenoise_VERSION} Found.")
    endif()

    if(NOT TARGET SDL2::SDL2)
        # using old SDL2 cmake, lets create a SDL2 target ourselves
        find_library(SDL2_LIBRARY_FILE_LOCATION SDL2 REQUIRED)

        add_library(SDL2::SDL2 SHARED IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SDL2_INCLUDE_DIRS}
            INTERFACE_LINK_LIBRARIES ${SDL2_LIBRARIES}
            IMPORTED_LOCATION ${SDL2_LIBRARY_FILE_LOCATION}
        )

        unset(SDL2_LIBRARY_FILE_LOCATION)
    endif()

    # minimum sdl version is 2.0.14 for controller LED support
    if((${SDL_VERSION_MAJOR} GREATER_EQUAL 2) AND (${SDL2_VERSION_MINOR} GREATER_EQUAL 0) AND (${SDL2_VERSION_PATCH} GREATER_EQUAL 14))
        add_compile_definitions(SDL2_FEATURE_CONTROLLER_LED=1)
    endif()
endif()

add_subdirectory(LUA)
add_subdirectory(Utility)

set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Jolt triggers a bug in some GCC versions, so we need to disable the warning
    target_compile_options(Jolt PRIVATE -Wno-stringop-overflow)
endif()

set_target_properties(Jolt PROPERTIES
    POSITION_INDEPENDENT_CODE ${WICKED_PIC}
)
add_compile_definitions(Jolt JPH_DEBUG_RENDERER)

file(GLOB HEADER_FILES CONFIGURE_DEPENDS *.h)
file(GLOB SOURCE_FILES CONFIGURE_DEPENDS *.cpp)
list(REMOVE_ITEM SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/offlineshadercompiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wiRenderer.cpp
    # wiAudio.cpp is replaced by stAudioLegacy.cpp, which implements the same
    # wi::audio API on top of the OpenAL + Steam Audio engine. Both in the build
    # would be duplicate symbols; the XAudio2/FAudio backend is what the engine
    # moved off, and it crashed on any scene with a SoundComponent because
    # wi::audio::Initialize() is never called.
    ${CMAKE_CURRENT_SOURCE_DIR}/wiAudio.cpp
)

file(GLOB SHADER_FILES CONFIGURE_DEPENDS shaders/*.hlsl shaders/*.hlsli)

# these are all Wicked files except wiRenderer.cpp, which can be compiled
# with embedded shaders or not. If embedded shaders are enabled in the build,
# we will create 2 libraries, one version without embedded shaders, and the normal
# one. The "no shaders" is needed because the offlineshadercompiler that compiles
# the shaders depends on the wicked library as well, so we have a chicken-and-egg
# problem.
add_library(Simtary_common OBJECT ${SOURCE_FILES} ${HEADER_FILES})

# the version without embedded shaders
add_library(Simtary_ext_shaders ${WICKED_LIBRARY_TYPE}
    wiRenderer.cpp
)

# the version with embedded shaders. This target might not end up being
# used if WICKED_EMBED_SHADERS is not set, though it could still be
# used in other CMakeLists as an explicit target instead of the "Simtary"
# alias.
# Note that we will later set specific include directory that contains the
# generated wiShaderDump.h file. Since wiRenderer.cpp uses __hasinclude
# the end result will actually differ.
# This is also the reason for the "common" object library; since include
# directories are per-target, we would compile every file twice, but this
# is only necessary for wiRenderer.cpp
add_library(Simtary_emb_shaders EXCLUDE_FROM_ALL ${WICKED_LIBRARY_TYPE}
    wiRenderer.cpp
	${CMAKE_CURRENT_BINARY_DIR}/wiShaderDump.h
)

# all the flag does is define what the Simtary target refers to
if(WICKED_EMBED_SHADERS)
    set(TARGET_NAME Simtary_emb_shaders)
else()
    set(TARGET_NAME Simtary_ext_shaders)
endif()

add_library(Simtary ALIAS ${TARGET_NAME})

# aliases for backwards compatibility
if(WIN32)
    add_library(Simtary_Windows ALIAS ${TARGET_NAME})
else()
    add_library(Simtary_Linux ALIAS ${TARGET_NAME})
endif()

set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME "Simtary"
)


target_precompile_headers(Simtary_common PRIVATE Simtary.h)
target_precompile_headers(Simtary_emb_shaders REUSE_FROM Simtary_common)
target_precompile_headers(Simtary_ext_shaders REUSE_FROM Simtary_common)

set_target_properties(Simtary_common PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
    POSITION_INDEPENDENT_CODE ${WICKED_PIC}
    FOLDER "Simtary"
    PROJECT_LABEL "common files"
)

set_target_properties(Simtary_ext_shaders PROPERTIES
    FOLDER "Simtary"
    PROJECT_LABEL "compile shaders at runtime variant"
)

set_target_properties(Simtary_emb_shaders PROPERTIES
    FOLDER "Simtary"
    PROJECT_LABEL "embedded shaders variant"
)

target_include_directories(Simtary_common SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Simtary>
)

set(Simtary_STATIC_LIBRARIES
    Jolt
    LUA
    Utility
)

if (USE_AVX512 OR USE_AVX2)
    target_compile_definitions(Simtary_common PUBLIC _XM_AVX2_INTRINSICS_)
endif()
if (USE_AVX)
    target_compile_definitions(Simtary_common PUBLIC _XM_AVX_INTRINSICS_)
endif()
if (USE_SSE4_1 OR USE_SSE4_2)
    target_compile_definitions(Simtary_common PUBLIC _XM_SSE4_INTRINSICS_)
endif()
if (USE_F16C)
    target_compile_definitions(Simtary_common PUBLIC _XM_F16C_INTRINSICS_)
endif()
if (USE_FMADD)
    target_compile_definitions(Simtary_common PUBLIC _XM_FMA3_INTRINSICS_)
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    if ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x86" OR "${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
        if (USE_AVX512)
            target_compile_options(Simtary_common PUBLIC /arch:AVX512)
        elseif (USE_AVX2)
            target_compile_options(Simtary_common PUBLIC /arch:AVX2)
        elseif (USE_AVX)
            target_compile_options(Simtary_common PUBLIC /arch:AVX)
        endif()
    endif()
elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "amd64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i386")
    if (USE_AVX512)
        target_compile_options(Simtary_common PUBLIC -mavx512f -mavx512vl -mavx512dq -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c)
    elseif (USE_AVX2)
        target_compile_options(Simtary_common PUBLIC -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c)
    elseif (USE_AVX)
        target_compile_options(Simtary_common PUBLIC -mavx -mpopcnt)
    elseif (USE_SSE4_2)
        target_compile_options(Simtary_common PUBLIC -msse4.2 -mpopcnt)
    elseif (USE_SSE4_1)
        target_compile_options(Simtary_common PUBLIC -msse4.1)
    else()
        target_compile_options(Simtary_common PUBLIC -msse2)
    endif()
    if (USE_LZCNT)
        target_compile_options(Simtary_common PUBLIC -mlzcnt)
    endif()
    if (USE_TZCNT)
        target_compile_options(Simtary_common PUBLIC -mbmi)
    endif()
    if (USE_F16C)
        target_compile_options(Simtary_common PUBLIC -mf16c)
    endif()
    if (USE_FMADD)
        target_compile_options(Simtary_common PUBLIC -mfma)
    endif()

    target_compile_options(Simtary_common PUBLIC -mfpmath=sse)
endif()

target_compile_definitions(Simtary_common PUBLIC WICKED_CMAKE_BUILD)

# Declared by the workspace CMakeLists normally; repeated here so the engine repo still
# defaults to large-world when it is configured on its own.
if (NOT DEFINED SIMTARY_LARGE_WORLD)
    option(SIMTARY_LARGE_WORLD "Store absolute world positions in 64-bit precision" ON)
endif()

# World position precision. PUBLIC so game code compiled against the engine agrees with
# it on the size and layout of TransformComponent - getting this out of sync is an ODR
# violation that shows up as garbled positions, not a link error.
if (SIMTARY_LARGE_WORLD)
    target_compile_definitions(Simtary_common PUBLIC SIMTARY_WORLD_FLOAT64=1)
    message(STATUS "Simtary: world positions are 64-bit (SIMTARY_LARGE_WORLD=ON)")
else()
    message(STATUS "Simtary: world positions are 32-bit (SIMTARY_LARGE_WORLD=OFF)")
endif()


if (WIN32)
    target_compile_definitions(Simtary_common PUBLIC
        UNICODE _UNICODE
    )

    set(LIBDXCOMPILER "dxcompiler.dll")
    
    target_link_libraries(Simtary_common PUBLIC
        SDL2::SDL2
    )
else ()
    # `ska::flat_hash_map` has issues on linux because of the hash function being identity
    # in same cases. Use `robin_hood::unordered_flat_map` instead
    target_compile_definitions(Simtary_common PUBLIC WI_UNORDERED_MAP_TYPE=2)

    target_link_libraries(Simtary_common PUBLIC
        Threads::Threads
        SDL2::SDL2
        $<$<BOOL:${OpenImageDenoise_FOUND}>:OpenImageDenoise> # links OpenImageDenoise only if it's found
    )

    set_target_properties(FAudio PROPERTIES
        POSITION_INDEPENDENT_CODE ${WICKED_PIC}
        UNITY_BUILD NO
    )
    set(Simtary_STATIC_LIBRARIES ${Simtary_STATIC_LIBRARIES} FAudio)

    target_link_libraries(Simtary_common PUBLIC dl)

    set(LIBDXCOMPILER "libdxcompiler.so")
endif()

target_link_libraries(Simtary_common PUBLIC ${Simtary_STATIC_LIBRARIES})

# openal-soft (declared in the top-level CMakeLists before the exceptions-off
# flags). Backs stAudio.cpp's OpenAL output stream. PUBLIC so the
# AL/al.h include dir propagates to consumers of the engine.
target_link_libraries(Simtary_common PUBLIC OpenAL)

# Steam Audio (declared in the top-level CMakeLists). PUBLIC so phonon.h's include
# dir and the SIMTARY_HAS_STEAMAUDIO define reach every consumer of the engine -
# stAudioSpatial.h switches its whole implementation on that define, and a game TU
# that saw a different value than the engine did would disagree about the layout of
# Spatializer::Impl. When the SDK is absent the target simply does not exist and the
# fallback panner compiles instead.
if (TARGET SteamAudio)
    target_link_libraries(Simtary_common PUBLIC SteamAudio)
endif()

if (PLATFORM MATCHES "SDL2")
    target_compile_definitions(Simtary_common PUBLIC SDL2=1)
endif()

add_library(dxcompiler SHARED IMPORTED)
set_property(TARGET dxcompiler PROPERTY
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/${LIBDXCOMPILER} )

add_executable(offlineshadercompiler
    offlineshadercompiler.cpp
)

# Copy the shader library next to the executable
add_custom_command(
    TARGET Simtary_ext_shaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${LIBDXCOMPILER} ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E ${COPY_OR_SYMLINK_DIR_CMD} ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_BINARY_DIR}/shaders
)

set_property(SOURCE
    wiOcean.cpp
    wiPhysics_Jolt.cpp
    wiGraphicsDevice_Vulkan.cpp
    wiGraphicsDevice_DX12.cpp
    stAudio.cpp
    stAudioEngine.cpp
    stAudioSpatial.cpp
    stAudioClip.cpp
    stAudioComponents.cpp
    stAudioLegacy.cpp

    PROPERTY SKIP_UNITY_BUILD_INCLUSION TRUE
)
set_property(TARGET LUA PROPERTY UNITY_BUILD NO)


if(WICKED_ENABLE_IPO)
    set_target_properties(
        ${Simtary_STATIC_LIBRARIES}
        Simtary_common
        offlineshadercompiler

        PROPERTIES

        INTERPROCEDURAL_OPTIMIZATION ON
        INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF
)
endif()


if(DEFINED ENV{VERBOSE})
    set(verbose 1)
else()
    set(verbose $<BOOL:${CMAKE_VERBOSE_MAKEFILE}>)
endif()

# define a command that will call the offlineshadercompiler and generate wiShaderDump.h
# we simply depend on every shader file, offlineshadercompiler is smart enough to only
# recompile files that have actually changed and those that depend on them.
add_custom_command(
    ${codegen}
    OUTPUT wiShaderDump.h
    COMMAND offlineshadercompiler $<$<BOOL:${WIN32}>:hlsl6> spirv shaderdump strip_reflection $<$<NOT:${verbose}>:quiet>
    DEPENDS offlineshadercompiler ${SHADER_FILES}
    USES_TERMINAL
)

target_link_libraries(Simtary_ext_shaders Simtary_common)
target_link_libraries(Simtary_emb_shaders Simtary_common)

target_link_libraries(offlineshadercompiler
    PUBLIC Simtary_ext_shaders
)

# only this target will see the wiShaderDump.h file, so the _ext_shaders target will not have embedded shaders.
target_include_directories(Simtary_emb_shaders PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
