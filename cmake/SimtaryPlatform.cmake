# SimtaryPlatform.cmake — the "<os>_<arch>" tag every build directory is keyed by.
#
# Produces SIMTARY_PLATFORM_ARCH, e.g. win_x86-64 / linux_x86-64 / linux_arm64 /
# macos_arm64. Build trees live at <project>/build/${SIMTARY_PLATFORM_ARCH}, and the
# shared engine tree at Simtary/build/${SIMTARY_PLATFORM_ARCH}, so several targets can
# coexist side by side without clobbering each other.
#
# CMakePresets.json hardcodes the same names so `cmake --preset win_x86-64` lands in
# the directory this file computes.

if(DEFINED SIMTARY_PLATFORM_ARCH)
    return()
endif()

if(WIN32)
    set(_st_os win)
elseif(APPLE)
    set(_st_os macos)
elseif(UNIX)
    set(_st_os linux)
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _st_os)
endif()

# Prefer the target architecture the toolchain actually emits. On MSVC multi-config
# generators CMAKE_SIZEOF_VOID_P is only known after the compiler test, which has
# already run by the time any project() call includes this.
if(CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _st_arch_raw)
elseif(CMAKE_OSX_ARCHITECTURES)
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _st_arch_raw)
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _st_arch_raw)
endif()

if(_st_arch_raw MATCHES "^(x64|amd64|x86_64|x86-64)$")
    set(_st_arch x86-64)
elseif(_st_arch_raw MATCHES "^(arm64|aarch64)$")
    set(_st_arch arm64)
elseif(_st_arch_raw MATCHES "^(x86|win32|i.86)$")
    set(_st_arch x86)
elseif(_st_arch_raw STREQUAL "")
    # No hint at all: fall back to pointer width.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_st_arch x86-64)
    else()
        set(_st_arch x86)
    endif()
else()
    set(_st_arch "${_st_arch_raw}")
endif()

set(SIMTARY_PLATFORM_ARCH "${_st_os}_${_st_arch}" CACHE STRING
    "Platform/architecture tag used to key build directories")
message(STATUS "Simtary platform tag: ${SIMTARY_PLATFORM_ARCH}")

unset(_st_os)
unset(_st_arch)
unset(_st_arch_raw)
