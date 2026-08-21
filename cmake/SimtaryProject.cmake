# SimtaryProject.cmake — read the .stpd project descriptor at configure time.
#
# The descriptor (<project>/assets/project.stpd) is the project's build-time manifest:
# display name, organization, copyright, version, icon. It is NBT, so CMake cannot
# read it directly; instead the reader tool is built once into the project's build
# tree and asked to emit set() lines, which are then include()d.
#
# Why not a text file CMake could parse? Because the whole Simtary data family is NBT
# (.stad options, .stcd saves, .staod animation descriptors), and a project manifest
# in a different format would be the odd one out.
#
# Why not a target of the main project? Chicken and egg: the values are needed while
# that project is being configured, before any of its targets can be built. Hence the
# standalone bootstrap project in tools/descriptor-bootstrap.
#
# Runtime properties are deliberately NOT in the descriptor. Baking a window size or a
# DevUI mode into the exe would mean rebuilding to change it, so those stay in
# st::AppConfig in src/main.cpp.
#
# Sets, when the corresponding key is present:
#   ST_PROJECT_NAME  ST_PROJECT_ORGANIZATION  ST_PROJECT_COPYRIGHT
#   ST_PROJECT_VERSION  ST_PROJECT_ICON  ST_PROJECT_TARGET_NAME

include_guard(GLOBAL)

# Build the reader once per build tree, then cache its path.
function(_simtary_descriptor_tool OUT_VAR)
    if (SIMTARY_DESCRIPTOR_TOOL AND EXISTS "${SIMTARY_DESCRIPTOR_TOOL}")
        set(${OUT_VAR} "${SIMTARY_DESCRIPTOR_TOOL}" PARENT_SCOPE)
        return()
    endif()

    set(_src   "${SIMTARY_ROOT}/tools/descriptor-bootstrap")
    set(_build "${CMAKE_BINARY_DIR}/_descriptor")

    message(STATUS "Simtary: building the project descriptor reader (one-off)")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${_src}" -B "${_build}"
        RESULT_VARIABLE _cfg
        OUTPUT_QUIET ERROR_VARIABLE _cfg_err
    )
    if (NOT _cfg EQUAL 0)
        message(WARNING "Simtary: could not configure the descriptor reader:\n${_cfg_err}")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${_build}" --config Release
        RESULT_VARIABLE _bld
        OUTPUT_QUIET ERROR_VARIABLE _bld_err
    )
    if (NOT _bld EQUAL 0)
        message(WARNING "Simtary: could not build the descriptor reader:\n${_bld_err}")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    # Multi-config generators drop it under <config>/ despite RUNTIME_OUTPUT_DIRECTORY.
    file(GLOB_RECURSE _found
        "${_build}/make_project_descriptor"
        "${_build}/make_project_descriptor.exe")
    if (NOT _found)
        message(WARNING "Simtary: descriptor reader built but not found under ${_build}")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()
    list(GET _found 0 _tool)

    set(SIMTARY_DESCRIPTOR_TOOL "${_tool}" CACHE FILEPATH "Project descriptor reader" FORCE)
    set(${OUT_VAR} "${_tool}" PARENT_SCOPE)
endfunction()

# Read DESCRIPTOR and define the ST_PROJECT_* variables in the CALLER's scope.
# A missing descriptor is not an error: the project simply keeps its CMake defaults.
function(simtary_read_project_descriptor DESCRIPTOR)
    if (NOT EXISTS "${DESCRIPTOR}")
        message(STATUS "Simtary: no project descriptor at ${DESCRIPTOR} - using CMake defaults")
        return()
    endif()

    _simtary_descriptor_tool(_tool)
    if (NOT _tool)
        message(WARNING "Simtary: descriptor found but unreadable - using CMake defaults")
        return()
    endif()

    set(_out "${CMAKE_CURRENT_BINARY_DIR}/project_descriptor.cmake")
    execute_process(
        COMMAND "${_tool}" "${DESCRIPTOR}" --cmake
        OUTPUT_FILE "${_out}"
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err
    )
    if (NOT _rc EQUAL 0)
        message(WARNING "Simtary: could not read ${DESCRIPTOR}:\n${_err}")
        return()
    endif()

    include("${_out}")
    message(STATUS "Simtary: project descriptor '${ST_PROJECT_NAME}' (${DESCRIPTOR})")

    # include() lands in this function's scope; hoist to the caller.
    foreach(_v NAME ORGANIZATION COPYRIGHT VERSION ICON TARGET_NAME)
        if (DEFINED ST_PROJECT_${_v})
            set(ST_PROJECT_${_v} "${ST_PROJECT_${_v}}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()
