cmake_minimum_required(VERSION 3.10)
project(libgfx LANGUAGES CXX)

# ============================================================================
# Backend selection
#   Default  : Linux framebuffer /dev/fb0  (software render, no extra deps)
#   GFX_DRM_SUPPORT=ON : DRM/KMS dumb-buffer page-flip (raw kernel ioctls,
#              no Mesa / no libdrm runtime — just kernel UAPI headers)
#   GFX_SDL_SUPPORT=ON : SDL2 desktop preview window
#
# DRM and SDL are mutually exclusive; DRM takes priority if both are ON.
# ============================================================================
option(GFX_DRM_SUPPORT "Enable DRM/KMS backend (raw kernel ioctls, no Mesa)" OFF)
option(GFX_SDL_SUPPORT "Enable SDL2 desktop preview backend" ON)

# ============================================================================
# Windows / MinGW64: DRM and framebuffer are Linux-only; SDL2 is required.
# ============================================================================
if(WIN32 OR MINGW)
    if(GFX_DRM_SUPPORT)
        message(WARNING "GFX: DRM/KMS is Linux-only — disabling for Windows build")
        set(GFX_DRM_SUPPORT OFF CACHE BOOL "" FORCE)
    endif()
    if(NOT GFX_SDL_SUPPORT)
        message(FATAL_ERROR "GFX: Windows/MinGW64 builds require SDL2 (set GFX_SDL_SUPPORT=ON and install SDL2).")
    endif()
endif()

add_library(libgfx
    ${CMAKE_CURRENT_SOURCE_DIR}/GFX.cpp
)

target_include_directories(libgfx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(libgfx PUBLIC cxx_std_14)

# ============================================================================
# DRM/KMS backend — raw kernel ioctls, zero runtime library dependency.
# The inline UAPI definitions in GFX.cpp use _IOWR/_IOW from <sys/ioctl.h>.
# ============================================================================
if(GFX_DRM_SUPPORT)
    if(GFX_SDL_SUPPORT)
        message(STATUS "GFX: DRM_SUPPORT overrides SDL_SUPPORT")
        set(GFX_SDL_SUPPORT OFF)
    endif()
    target_compile_definitions(libgfx PUBLIC GFX_USE_DRM)
    message(STATUS "GFX: backend = DRM/KMS dumb-buffer (raw kernel ioctls, no Mesa)")
    return()  # skip SDL block below
endif()

# ============================================================================
# SDL2 desktop preview backend
# ============================================================================
if(GFX_SDL_SUPPORT)
    # A parent project may already provide SDL2 as a build target (Milistry adds the
    # libs/SDL2 submodule via add_subdirectory). find_package() would not see that and
    # would fail — or worse, pull in a second, unrelated system SDL2. Prefer the
    # existing target and only fall back to find_package for standalone builds.
    if(TARGET SDL2::SDL2)
        target_compile_definitions(libgfx PUBLIC GFXSDL)
        target_link_libraries(libgfx PUBLIC SDL2::SDL2)
        message(STATUS "GFX: backend = SDL2 desktop preview (using parent project's SDL2 target)")
        return()
    endif()

    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        target_compile_definitions(libgfx PUBLIC GFXSDL)
        target_link_libraries(libgfx PUBLIC SDL2::SDL2)
        target_include_directories(libgfx PUBLIC ${SDL2_INCLUDE_DIRS})
        message(STATUS "GFX: backend = SDL2 desktop preview")
    else()
        if(WIN32 OR MINGW)
            message(FATAL_ERROR
                "GFX: SDL2 not found — required for Windows/MinGW64 builds.\n"
                "Install SDL2 development libraries (e.g. via MSYS2: "
                "pacman -S mingw-w64-x86_64-SDL2) and re-run CMake.")
        else()
            message(WARNING "GFX: SDL2 not found — falling back to framebuffer backend")
        endif()
    endif()
else()
    if(WIN32 OR MINGW)
        message(FATAL_ERROR
            "GFX: No usable backend for Windows/MinGW64 (GFX_SDL_SUPPORT is OFF "
            "and framebuffer/DRM are Linux-only). Enable GFX_SDL_SUPPORT and install SDL2.")
    else()
        message(STATUS "GFX: backend = Linux framebuffer /dev/fb0 (default)")
    endif()
endif()
