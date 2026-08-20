cmake_minimum_required(VERSION 3.10)
project(GFX_Examples)

# Use a cleaner path reference
set(LIBGFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libgfx")
# If libgfx is the parent directory of this examples folder
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." libgfx_build)

# Define examples in a list
set(EXAMPLES 
    example_simple 
    example_input 
    example_paint
)

foreach(EXAMPLE ${EXAMPLES})
    add_executable(${EXAMPLE} ${EXAMPLE}.cpp)

    target_link_libraries(${EXAMPLE} PRIVATE libgfx)

    # Only keep this if libgfx doesn't export its own headers
    target_include_directories(${EXAMPLE} PRIVATE ${LIBGFX_DIR})
endforeach()

# Print build info
message(STATUS "GFX Examples configured:")
foreach(EXAMPLE ${EXAMPLES})
    message(STATUS "  - ${EXAMPLE}")
endforeach()