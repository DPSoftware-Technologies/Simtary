cmake_minimum_required(VERSION 3.19)

# Standalone build of make_project_descriptor.
#
# The project descriptor is NBT, which CMake cannot parse, so the build has to run the
# reader tool at configure time -- which means the tool must exist before the main
# project is configured. It cannot be a target of that project (chicken and egg), so
# it gets its own tiny CMake project that SimtaryProject.cmake builds on demand.
#
# Two translation units, no engine, no third-party: Nbt.cpp depends on nothing but the
# standard library, which is what makes this cheap enough to do at configure time.

project(simtary_descriptor_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_framework "${CMAKE_CURRENT_LIST_DIR}/../../Framework" ABSOLUTE)

add_executable(make_project_descriptor
    ${CMAKE_CURRENT_LIST_DIR}/../make_project_descriptor.cpp
    ${_framework}/io/Nbt.cpp
)
target_include_directories(make_project_descriptor PRIVATE ${_framework})

# Single-config generators need this to land somewhere predictable; multi-config ones
# put it under <config>/, which SimtaryProject.cmake globs for.
set_target_properties(make_project_descriptor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
