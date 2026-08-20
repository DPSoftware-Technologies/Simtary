# CMake Build Configuration for GFX Library

## Overview

The GFX library CMakeLists.txt has been updated with optional SDL2 support that allows building for either:
- **Desktop** (with SDL2) - Interactive graphics window with mouse input
- **Embedded Linux** (framebuffer) - Direct framebuffer rendering

## CMake Options

### GFX_SDL_SUPPORT
- **Type**: Boolean (ON/OFF)
- **Default**: ON
- **Description**: Enable SDL2 desktop backend support

## Building

### With SDL2 Support (Desktop)

#### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install libsdl2-dev

# macOS
brew install sdl2

# Fedora
sudo dnf install SDL2-devel

# Alpine
apk add sdl2-dev
```

#### Build Command
```bash
mkdir build
cd build
cmake .. -DGFX_SDL_SUPPORT=ON
make
```

#### Or use presets if available
```bash
cmake --preset=default
cmake --build build
```

### Without SDL2 (Embedded/Framebuffer Only)

#### Build Command
```bash
mkdir build
cd build
cmake .. -DGFX_SDL_SUPPORT=OFF
make
```

## CMakeLists.txt Details

### Configuration Flow

1. **Check Option**
   ```cmake
   option(GFX_SDL_SUPPORT "Enable SDL2 desktop backend support" ON)
   ```
   - Allows users to enable/disable SDL2 support
   - Default is ON (attempts to find SDL2, falls back to OFF if not found)

2. **Find SDL2**
   ```cmake
   find_package(SDL2 QUIET)
   ```
   - Uses QUIET to avoid loud warnings
   - Only searches if GFX_SDL_SUPPORT is ON

3. **Conditional Linking**
   ```cmake
   if(GFX_SDL_SUPPORT AND SDL2_FOUND)
       target_compile_definitions(libgfx PUBLIC GFXSDL)
       target_link_libraries(libgfx PUBLIC SDL2::SDL2)
       target_include_directories(libgfx PUBLIC ${SDL2_INCLUDE_DIRS})
   endif()
   ```
   - Adds GFXSDL compile definition when SDL2 is available
   - Links SDL2 library
   - Includes SDL2 headers

4. **Informative Messages**
   ```cmake
   message(STATUS "GFX: SDL2 support enabled")
   message(WARNING "GFX: SDL2 not found. SDL support disabled. Install SDL2-devel to enable.")
   message(STATUS "GFX: Compiled with SDL2 support")
   ```
   - Clear feedback on build configuration

## Usage in Projects

### Using libgfx in Your Application

#### CMakeLists.txt
```cmake
# Basic usage - automatically detects SDL2 support
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE libgfx)

# With explicit SDL control
set(GFX_SDL_SUPPORT ON CACHE BOOL "Enable GFX SDL support" FORCE)
add_subdirectory(libs/libgfx)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE libgfx)
```

#### Checking at Runtime
You can query the GFXSDL definition in your code:
```cpp
#ifdef GFXSDL
    // SDL backend compiled in
    LinuxGFX gfx("My App", 800, 600);
#else
    // Framebuffer backend only
    LinuxGFX gfx("/dev/fb0");
#endif
```

## CMake Build Variables

### Input Options
```cmake
-DGFX_SDL_SUPPORT=ON|OFF      # Enable/disable SDL2 support
```

### Output Variables
```cmake
SDL2_FOUND          # Boolean: Was SDL2 found?
SDL2_INCLUDE_DIRS   # Path to SDL2 headers
SDL2::SDL2          # CMake target for linking
```

## Troubleshooting

### SDL2 Not Found Error

**Problem:** CMake can't find SDL2
```
GFX: SDL2 not found. SDL support disabled. Install SDL2-devel to enable.
```

**Solution:** Install SDL2 development package
```bash
# Ubuntu/Debian
sudo apt-get install libsdl2-dev

# macOS
brew install sdl2

# Fedora
sudo dnf install SDL2-devel
```

Then reconfigure:
```bash
cd build
cmake .. -DGFX_SDL_SUPPORT=ON
make
```

### Force Framebuffer Backend

To compile without SDL2 even if it's installed:
```bash
cmake .. -DGFX_SDL_SUPPORT=OFF
```

### Clean SDL2-related CMake Cache

If SDL2 support is not detected after installation:
```bash
rm -rf build
mkdir build
cd build
cmake .. -DGFX_SDL_SUPPORT=ON
```

## Platform-Specific Notes

### Linux Desktop
```bash
# Ubuntu (recommended for development)
sudo apt-get install cmake build-essential libsdl2-dev

# Build with SDL2
mkdir build && cd build
cmake .. -DGFX_SDL_SUPPORT=ON
make
```

### Linux Embedded (RPi Zero 2W)
```bash
# No SDL2 needed for framebuffer backend
mkdir build && cd build
cmake .. -DGFX_SDL_SUPPORT=OFF
make
```

### macOS
```bash
# Install Homebrew if not present
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install build tools and SDL2
brew install cmake llvm sdl2

# Build
mkdir build && cd build
cmake .. -DGFX_SDL_SUPPORT=ON
make -j4
```

### Windows (MinGW/MSVC)
```bash
# Using vcpkg
vcpkg install sdl2

# Or download SDL2 development libraries manually
# https://github.com/libsdl-org/SDL/releases

mkdir build && cd build
cmake .. -DGFX_SDL_SUPPORT=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg_path>/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

## Example: Full Project Setup

### Project Structure
```
myproject/
├── CMakeLists.txt
├── src/
│   └── main.cpp
└── libs/
    └── libgfx/
        ├── CMakeLists.txt
        ├── GFX.h
        └── GFX.cpp
```

### Top-level CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.10)
project(MyGraphicsApp)

# Option to control GFX SDL support at project level
option(ENABLE_GFX_SDL "Enable SDL2 support in GFX library" ON)

# Add libgfx subdirectory
add_subdirectory(libs/libgfx)

# Create executable
add_executable(myapp src/main.cpp)

# Link against libgfx
target_link_libraries(myapp PRIVATE libgfx)

# Optional: Print configuration
message(STATUS "Build configuration:")
message(STATUS "  GFX SDL Support: ${GFX_SDL_SUPPORT}")
```

### Build Commands
```bash
# Desktop build with SDL2
mkdir build-desktop
cd build-desktop
cmake .. -DENABLE_GFX_SDL=ON
make

# Embedded build without SDL2
mkdir build-embedded
cd build-embedded
cmake .. -DENABLE_GFX_SDL=OFF
make
```

## CMake Installation Setup

### Installing the Library (Optional)

```cmake
# Add to libgfx/CMakeLists.txt
install(TARGETS libgfx
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES GFX.h
    DESTINATION include
)
```

Then install:
```bash
cmake --install build --prefix /usr/local
```

## Integration with CI/CD

### GitHub Actions Example
```yaml
name: Build GFX Library

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Install SDL2
        run: sudo apt-get install -y libsdl2-dev
      
      - name: Build with SDL2
        run: |
          mkdir build
          cd build
          cmake .. -DGFX_SDL_SUPPORT=ON
          make
      
      - name: Build without SDL2
        run: |
          mkdir build-fb
          cd build-fb
          cmake .. -DGFX_SDL_SUPPORT=OFF
          make
```

## Summary

The CMakeLists.txt configuration:
- ✅ Automatically detects SDL2 availability
- ✅ Provides fallback to framebuffer-only mode
- ✅ Clear messaging about build configuration
- ✅ Easy to override via command-line
- ✅ Compatible with all platforms (Linux, macOS, Windows)
- ✅ Supports both desktop and embedded builds
