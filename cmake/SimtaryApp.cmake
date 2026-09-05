# SimtaryApp.cmake - simtary_add_app(): turn a project directory into a game.
#
# A project's CMakeLists.txt is about ten lines; everything below is what those ten
# lines expand to. Pulled in through cmake/SimtaryBootstrap.cmake.
#
#   simtary_add_app(
#       NAME           Milistry            # target + executable name (required)
#       ORGANIZATION   "PlatoonLabs"       # rc CompanyName / copyright holder
#       ICON           assets/appicon.ico  # Windows .ico, optional
#       SOURCE_DIR     src                 # default: <project>/src
#       ASSETS_DIR     assets              # default: <project>/assets
#       CONTENT_SUBDIR contents            # <assets>/<sub> lands in <exe>/assets
#       SCENE_SUBDIR   scenes              # <assets>/<sub> holds the .wiscene SOURCES
#       EXTRA_SOURCES  ...                 # sources outside SOURCE_DIR
#       EXTRA_INCLUDES ...
#       EXTRA_LIBS     ...
#       NO_SHADER_WARM                     # skip the engine shader pre-pass
#       NO_CRASH_REPORTER                  # do not ship SimtaryCrashReporter
#       MODULE                             # ship game code as a separate library
#       MODULE_NAME    application         # its file name; default "application"
#   )
#
# The framework (Simtary/Framework) is compiled INTO the app rather than linked as a
# shared static library: each app gets its own generated version.h (build counter,
# version, date) and its own AppConfig, so the objects are genuinely per-project.
#
# MODULE splits the OUTPUT in two without splitting the sources:
#
#   <NAME>.exe          engine + framework + the loader main()  (the HOST)
#   application.dll     the project's own src/                  (the MODULE)
#
# The same src/main.cpp builds either way - ST_APP_ENTRY (Framework/stModule.h) is a
# main() without MODULE and the module's exported descriptor with it. Editing a scene
# then relinks a small DLL instead of a 12 MB executable. Read Framework/stModule.h
# before turning it on: it is a matched-version boundary, not a stable ABI, and the
# two halves must ship together.

include_guard(GLOBAL)

include(SimtaryProject)

# CACHE INTERNAL, not a plain set(): this file is included from Simtary/CMakeLists.txt,
# which a game add_subdirectory()s from its own (parent) scope. A directory-scope
# variable set down there would not be visible back up in the game's CMakeLists.
set(SIMTARY_FRAMEWORK_DIR "${SIMTARY_ROOT}/Framework" CACHE INTERNAL "Simtary framework sources")

# copy_directory_if_different skips unchanged files, which matters for the ~380-file
# engine shader tree staged on every build. Added in CMake 3.26.
if (CMAKE_VERSION VERSION_LESS "3.26.0")
    set(_SIMTARY_COPY_DIR copy_directory)
else()
    set(_SIMTARY_COPY_DIR copy_directory_if_different)
endif()
set(SIMTARY_COPY_DIR_CMD "${_SIMTARY_COPY_DIR}" CACHE INTERNAL "cmake -E directory copy mode")

# build_number.txt is PROJECT-level: only a build of that project may advance it.
# Turn this OFF for compile-checks that are not real project builds - the engine-side
# sweep (SIMTARY_BUILD_PROJECTS) and CI both do, so they never inflate the counter.
# version.h is still generated at configure time either way, so ST_APP_BUILD_NUMBER
# always resolves.
option(SIMTARY_BUMP_BUILD_NUMBER "Advance <project>/build_number.txt on every build" ON)

# simtary_compile_shader()
# Compile one HLSL file with dxc into the app's runtime shader folder
# (<exe>/shaders/hlsl6 on DX12, <exe>/shaders/spirv on Vulkan - the engine appends
# the backend subfolder itself, see Application::SetWindow).
#
#   simtary_compile_shader(TARGET MyGame SOURCE assets/shaders/FooPS.hlsl PROFILE ps_6_0)
#
# A missing dxc is a warning, never a hard error: SIMTARY_DXC is empty and the call
# is skipped, exactly like the engine's own shader tooling.
function(simtary_compile_shader)
    cmake_parse_arguments(SH "ENGINE_ENV" "TARGET;SOURCE;PROFILE;ENTRY;OUTPUT_NAME" "" ${ARGN})
    if (NOT SH_TARGET OR NOT SH_SOURCE OR NOT SH_PROFILE)
        message(FATAL_ERROR "simtary_compile_shader: TARGET, SOURCE and PROFILE are required")
    endif()
    if (NOT SIMTARY_DXC)
        return()
    endif()
    if (NOT SH_ENTRY)
        set(SH_ENTRY main)
    endif()
    if (NOT SH_OUTPUT_NAME)
        get_filename_component(SH_OUTPUT_NAME "${SH_SOURCE}" NAME_WE)
    endif()

    if (WIN32)
        set(_backend hlsl6)
        set(_spirv "")
    else()
        set(_backend spirv)
        set(_spirv -spirv)
    endif()

    # ENGINE_ENV: the shader includes globals.hlsli and reads the engine's bindless
    # heaps, camera and frame constants. That needs the same compiler environment
    # wi::shadercompiler builds for the engine's own shaders - the include path, the
    # default root signature on DX12, and the binding shifts + descriptor set numbers
    # on Vulkan (mirroring GraphicsDevice_Vulkan's VULKAN_BINDING_SHIFT_* and
    # DESCRIPTOR_SET_*). Runtime compilation is not an option here: dxcompiler.dll is
    # not shipped next to the exe, so a shader that is not built now is never built.
    set(_engine_args "")
    if (SH_ENGINE_ENV)
        list(APPEND _engine_args
            -I "${SIMTARY_ROOT}/Engine/shaders"
            -I "${SIMTARY_ROOT}/assets/shaders")
        if (WIN32)
            list(APPEND _engine_args -rootsig-define WICKED_ENGINE_DEFAULT_ROOTSIGNATURE)
        else()
            list(APPEND _engine_args
                -fspv-target-env=vulkan1.3
                -fvk-use-dx-layout
                -fvk-use-dx-position-w
                -fvk-b-shift 0 0
                -fvk-t-shift 1000 0
                -fvk-u-shift 2000 0
                -fvk-s-shift 3000 0
                -D DESCRIPTOR_SET_BINDLESS_SAMPLER=1
                -D DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER=2
                -D DESCRIPTOR_SET_BINDLESS_UNIFORM_TEXEL_BUFFER=3
                -D DESCRIPTOR_SET_BINDLESS_SAMPLED_IMAGE=4
                -D DESCRIPTOR_SET_BINDLESS_STORAGE_IMAGE=5
                -D DESCRIPTOR_SET_BINDLESS_STORAGE_TEXEL_BUFFER=6
                -D DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE=7)
        endif()
    endif()

    add_custom_command(TARGET ${SH_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${SH_TARGET}>/shaders/${_backend}
        COMMAND ${SIMTARY_DXC} -T ${SH_PROFILE} -E ${SH_ENTRY} ${_spirv} ${_engine_args}
            "${SH_SOURCE}"
            -Fo $<TARGET_FILE_DIR:${SH_TARGET}>/shaders/${_backend}/${SH_OUTPUT_NAME}.cso
        COMMENT "Compiling ${SH_OUTPUT_NAME} -> shaders/${_backend}/"
        VERBATIM
    )
endfunction()

# dxc lookup (once per configure)
# The bundled Engine/Utility/dxc only ships headers, so fall back to the copy that
# comes with the Vulkan SDK or the Windows 10/11 SDK.
if (NOT DEFINED SIMTARY_DXC)
    find_program(SIMTARY_DXC dxc
        HINTS
            ${SIMTARY_ROOT}/Engine/Utility/dxc
            "$ENV{VULKAN_SDK}/Bin"
    )
    if (NOT SIMTARY_DXC AND WIN32)
        file(GLOB _dxc_sdk_candidates
            "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe"
            "C:/Program Files/Windows Kits/10/bin/*/x64/dxc.exe"
        )
        if (_dxc_sdk_candidates)
            list(SORT _dxc_sdk_candidates)
            list(LENGTH _dxc_sdk_candidates _dxc_count)
            math(EXPR _dxc_last "${_dxc_count} - 1")
            list(GET _dxc_sdk_candidates ${_dxc_last} SIMTARY_DXC)
            set(SIMTARY_DXC "${SIMTARY_DXC}" CACHE FILEPATH "DirectX shader compiler" FORCE)
        endif()
    endif()
    if (SIMTARY_DXC)
        message(STATUS "Simtary: using DXC at ${SIMTARY_DXC}")
    elseif (WIN32)
        message(WARNING "dxc.exe not found - framework shaders won't be compiled. "
            "Install the Windows SDK or place dxc.exe in Simtary/Engine/Utility/dxc.")
    else()
        message(WARNING "dxc not found, framework shaders won't be compiled. "
            "Install with: sudo apt install directx-shader-compiler")
    endif()
endif()

# _simtary_collect_static_libs()
# Every static library that ends up on a target's link line, transitively.
#
# The direct list is not enough: the engine links Utility, Jolt and LUA as separate
# archives that Simtary.lib does NOT absorb (linking a static library to an OBJECT
# library does not merge the two), so a symbol like OffsetAllocator::free lives in a
# lib nobody names in simtary_add_app. The module export list has to see all of them
# or the module fails to link against symbols the host demonstrably has.
function(_simtary_collect_static_libs TARGET OUT_VAR)
    set(_seen "")
    set(_found "")
    set(_queue "${TARGET}")

    while (_queue)
        list(POP_FRONT _queue _item)

        # PRIVATE dependencies of an INTERFACE/static target arrive wrapped; anything
        # else built out of a generator expression cannot be resolved at configure
        # time and is skipped rather than guessed at.
        if (_item MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_item "${CMAKE_MATCH_1}")
        endif()
        if (_item MATCHES "\\$<")
            continue()
        endif()
        if (NOT TARGET ${_item})
            continue()   # a bare system library (user32, dbghelp, ...)
        endif()

        get_target_property(_aliased ${_item} ALIASED_TARGET)
        if (_aliased)
            set(_item "${_aliased}")
        endif()
        if (_item IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_item})

        get_target_property(_type ${_item} TYPE)
        if (_type STREQUAL "STATIC_LIBRARY")
            list(APPEND _found ${_item})
        endif()

        # INTERFACE libraries carry no LINK_LIBRARIES, and imported targets carry no
        # INTERFACE_LINK_LIBRARIES worth walking; both come back NOTFOUND and are
        # simply not queued.
        if (NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_direct ${_item} LINK_LIBRARIES)
            if (_direct)
                list(APPEND _queue ${_direct})
            endif()
        endif()
        get_target_property(_iface ${_item} INTERFACE_LINK_LIBRARIES)
        if (_iface)
            list(APPEND _queue ${_iface})
        endif()
    endwhile()

    set(${OUT_VAR} "${_found}" PARENT_SCOPE)
endfunction()

# simtary_add_app()
function(simtary_add_app)
    set(_opts NO_SHADER_WARM NO_CRASH_REPORTER PACK_ASSETS PACK_ONLY MODULE)
    set(_one  NAME ORGANIZATION ICON SOURCE_DIR ASSETS_DIR CONTENT_SUBDIR SCENE_SUBDIR
              PACK_NAME PACK_PART_SIZE PACK_LEVEL MODULE_NAME)
    set(_multi EXTRA_SOURCES EXTRA_INCLUDES EXTRA_LIBS)
    cmake_parse_arguments(APP "${_opts}" "${_one}" "${_multi}" ${ARGN})

    if (NOT APP_NAME)
        message(FATAL_ERROR "simtary_add_app: NAME is required")
    endif()
    if (NOT APP_SOURCE_DIR)
        set(APP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    if (NOT APP_ASSETS_DIR)
        set(APP_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    endif()
    if (NOT DEFINED APP_CONTENT_SUBDIR)
        set(APP_CONTENT_SUBDIR "contents")
    endif()
    # Maps live BESIDE the content tree, not inside it. assets/contents/ is "everything
    # here becomes a packed resource"; a .wiscene is not a resource, it is the source a
    # .stsd is converted from, and shipping it would be a second copy of every map. Two
    # folders means neither step needs an exception list.
    if (NOT DEFINED APP_SCENE_SUBDIR)
        set(APP_SCENE_SUBDIR "scenes")
    endif()
    set(APP_SCENE_DIR "")
    if (APP_SCENE_SUBDIR AND EXISTS ${APP_ASSETS_DIR}/${APP_SCENE_SUBDIR})
        set(APP_SCENE_DIR ${APP_ASSETS_DIR}/${APP_SCENE_SUBDIR})
    endif()
    # project descriptor
    # assets/project.stpd is the build-time manifest (identity, icon, version). It is
    # read here, at configure time, and never at runtime - which is why it lives in
    # assets/ and not assets/contents/, the folder that ships with the game.
    # Explicit simtary_add_app() arguments still win over the file.
    simtary_read_project_descriptor("${CMAKE_CURRENT_SOURCE_DIR}/assets/project.stpd")

    if (NOT APP_ORGANIZATION AND ST_PROJECT_ORGANIZATION)
        set(APP_ORGANIZATION "${ST_PROJECT_ORGANIZATION}")
    endif()
    if (NOT APP_ICON AND ST_PROJECT_ICON)
        set(APP_ICON "${ST_PROJECT_ICON}")
    endif()
    if (NOT APP_ORGANIZATION)
        set(APP_ORGANIZATION "DPSoftware")
    endif()

    # Display name is what a player sees (window title, About box, exe metadata); the
    # CMake target name stays the exe filename and must be a valid identifier.
    if (ST_PROJECT_NAME)
        set(ST_APP_DISPLAY_NAME "${ST_PROJECT_NAME}")
    else()
        set(ST_APP_DISPLAY_NAME "${APP_NAME}")
    endif()
    if (ST_PROJECT_COPYRIGHT)
        set(ST_APP_COPYRIGHT "${ST_PROJECT_COPYRIGHT}")
    else()
        string(TIMESTAMP _year "%Y")
        set(ST_APP_COPYRIGHT "Copyright (C) ${_year} ${APP_ORGANIZATION}")
    endif()

    set(ST_APP_ORGANIZATION "${APP_ORGANIZATION}")

    # sources
    # The framework is rebuilt per app on purpose (see the header comment).
    file(GLOB_RECURSE _framework_sources CONFIGURE_DEPENDS
        ${SIMTARY_FRAMEWORK_DIR}/*.cpp
        ${SIMTARY_FRAMEWORK_DIR}/*.h
    )
    file(GLOB_RECURSE _app_sources CONFIGURE_DEPENDS
        ${APP_SOURCE_DIR}/*.cpp
        ${APP_SOURCE_DIR}/*.h
    )
    if (NOT _app_sources)
        message(FATAL_ERROR "simtary_add_app(${APP_NAME}): no sources found under ${APP_SOURCE_DIR}")
    endif()

    if (NOT _framework_sources)
        message(FATAL_ERROR "simtary_add_app(${APP_NAME}): no framework sources under ${SIMTARY_FRAMEWORK_DIR}")
    endif()

    # targets
    # Default layout: one executable holding engine, framework and game.
    #
    # MODULE layout: the game's own sources move into a separate shared library that
    # the executable loads at startup (Framework/stModule.h explains the boundary).
    # The split is by TARGET, not by source: the same src/main.cpp builds either way,
    # because ST_APP_ENTRY expands to a main() or to the module's exported descriptor
    # depending on ST_MODULE_BUILD.
    #
    # Both halves are compiled through OBJECT libraries rather than straight into
    # their final target. That is what lets the export list below be derived: the
    # game's object files have to exist, and be inspectable, before the host links.
    set(_module_target   "")
    set(_gamecode_target "")
    if (APP_MODULE)
        if (NOT APP_MODULE_NAME)
            # "application" by default, so the host does not have to know the project's
            # name to find it. Pass MODULE_NAME to use the project's name instead.
            set(APP_MODULE_NAME "application")
        endif()
        set(_module_target   ${APP_NAME}_Module)
        set(_gamecode_target ${APP_NAME}_GameCode)
        set(_framework_target ${APP_NAME}_Framework)

        add_library(${_framework_target} OBJECT ${_framework_sources})
        add_library(${_gamecode_target}  OBJECT ${_app_sources} ${APP_EXTRA_SOURCES})
        add_executable(${APP_NAME} $<TARGET_OBJECTS:${_framework_target}>)
        set_target_properties(${APP_NAME} PROPERTIES LINKER_LANGUAGE CXX)

        # The module is a MODULE library, not SHARED: nothing links against it, it is
        # opened with LoadLibrary/dlopen. PREFIX "" keeps the file called
        # "application.dll"/"application.so" on every platform instead of picking up
        # a "lib" on Unix, so AppConfig and the installer can name it once.
        # MODULE_NAME is free-form: "application" (the default), the project's own name,
        # or anything else. Two things have to be kept out of its way for that to hold.
        #
        # The import library and its .exp go to a build-only folder rather than next to
        # the exe. Nothing links them - the host opens the module with LoadLibrary and
        # GetProcAddress - and leaving them in the output directory means MODULE_NAME
        # Milistry would try to write the Milistry.lib the HOST already owns, quietly
        # replacing the import library the module itself links against.
        #
        # The PDB has the same collision and a worse failure: the two would overwrite
        # each other and one binary would symbolise as addresses. dbghelp finds a PDB
        # through the name recorded in the image, not by matching file names, so giving
        # the module's a suffix costs nothing.
        set(_module_pdb "${APP_MODULE_NAME}")
        if (APP_MODULE_NAME STREQUAL APP_NAME)
            set(_module_pdb "${APP_MODULE_NAME}_module")
        endif()

        add_library(${_module_target} MODULE $<TARGET_OBJECTS:${_gamecode_target}>)
        set_target_properties(${_module_target} PROPERTIES
            OUTPUT_NAME     ${APP_MODULE_NAME}
            PREFIX          ""
            LINKER_LANGUAGE CXX
            LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:${APP_NAME}>"
            RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:${APP_NAME}>"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/module"
            PDB_NAME        "${_module_pdb}"
        )
        set_target_properties(${_gamecode_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    else()
        set(_framework_target ${APP_NAME})
        add_executable(${APP_NAME} ${_app_sources} ${_framework_sources} ${APP_EXTRA_SOURCES})
    endif()

    # Every target that actually compiles C++ and therefore needs the include paths,
    # the flags and the engine's usage requirements. Linking a library to an OBJECT
    # library hands over those requirements without linking anything, which is exactly
    # what the two halves need.
    # The executable stays in the list even in MODULE mode: it compiles no C++ there,
    # but it is still the target that does the linking and so needs the same libraries.
    set(_compile_targets ${APP_NAME})
    if (APP_MODULE)
        list(APPEND _compile_targets ${_framework_target} ${_gamecode_target})
    endif()

    source_group(TREE ${SIMTARY_FRAMEWORK_DIR} PREFIX "Simtary/Framework" FILES ${_framework_sources})
    source_group(TREE ${APP_SOURCE_DIR}        PREFIX "src"               FILES ${_app_sources})

    # Descriptor version wins: it is the one a release is cut from.
    if (ST_PROJECT_VERSION)
        set(_app_version ${ST_PROJECT_VERSION})
    elseif (PROJECT_VERSION)
        set(_app_version ${PROJECT_VERSION})
    else()
        set(_app_version 1.0.0)
    endif()
    string(REPLACE "." ";" _vparts "${_app_version}")
    list(LENGTH _vparts _vcount)
    if (_vcount GREATER_EQUAL 3)
        set_target_properties(${APP_NAME} PROPERTIES VERSION ${_app_version})
        list(GET _vparts 0 _vmajor)
        set_target_properties(${APP_NAME} PROPERTIES SOVERSION ${_vmajor})
    endif()
    set(ST_APP_VERSION_STRING "${_app_version}")

    # build number / versioning
    # A persistent per-project counter (build_number.txt, gitignored) is
    # incremented before every build and baked into a generated version.h, which
    # the About window shows. The changing header forces sysui.cpp to recompile so
    # the number is always current.
    set(_counter  ${CMAKE_CURRENT_SOURCE_DIR}/build_number.txt)
    set(_template ${SIMTARY_FRAMEWORK_DIR}/version.h.in)
    set(_gendir   ${CMAKE_CURRENT_BINARY_DIR}/generated/${APP_NAME})
    set(_header   ${_gendir}/version.h)

    # Generate once at configure time (no bump) so IDEs/indexers and the very first
    # compile always find version.h.
    if (NOT EXISTS "${_header}")
        execute_process(COMMAND ${CMAKE_COMMAND}
            -DCOUNTER_FILE=${_counter}
            -DTEMPLATE=${_template}
            -DOUTPUT=${_header}
            -DPROJECT_VER=${_app_version}
            -DBUMP=OFF
            -P ${SIMTARY_ROOT}/cmake/IncrementBuild.cmake
        )
    endif()

    # Identity header generated from the descriptor, so main.cpp never repeats the
    # name/organization/copyright that the manifest already states. Unconditional: it is
    # not a build-number artefact, and generating it only when the counter may advance
    # left every SIMTARY_BUILD_PROJECTS sweep (which forces the bump OFF) without a
    # stProject.h to include.
    configure_file(
        ${SIMTARY_FRAMEWORK_DIR}/stProject.h.in
        ${_gendir}/stProject.h
        @ONLY
    )

    if (SIMTARY_BUMP_BUILD_NUMBER)
        add_custom_target(${APP_NAME}_BumpBuildNumber
            COMMAND ${CMAKE_COMMAND}
                -DCOUNTER_FILE=${_counter}
                -DTEMPLATE=${_template}
                -DOUTPUT=${_header}
                -DPROJECT_VER=${_app_version}
                -P ${SIMTARY_ROOT}/cmake/IncrementBuild.cmake
            BYPRODUCTS ${_header}
            COMMENT "Bumping ${APP_NAME} build number"
            VERBATIM
        )
        set_target_properties(${APP_NAME}_BumpBuildNumber PROPERTIES FOLDER "${APP_NAME}/Build")
        # Every target that compiles against the generated version.h, which in MODULE
        # mode is the two object libraries rather than the executable itself.
        foreach (_target IN LISTS _compile_targets)
            add_dependencies(${_target} ${APP_NAME}_BumpBuildNumber)
        endforeach()
    else()
        message(STATUS "${APP_NAME}: build number frozen (SIMTARY_BUMP_BUILD_NUMBER=OFF)")
    endif()

    # Windows resources (icon + version info)
    if (WIN32)
        string(TIMESTAMP PROJECT_YEAR "%Y")
        set(PROJECT_NAME "${APP_NAME}")           # consumed by app.rc.in
        set(PROJECT_VERSION "${_app_version}")
        list(GET _vparts 0 PROJECT_VERSION_MAJOR)
        list(GET _vparts 1 PROJECT_VERSION_MINOR)
        list(GET _vparts 2 PROJECT_VERSION_PATCH)
        if (APP_ICON)
            get_filename_component(ST_APP_ICON "${APP_ICON}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        else()
            set(ST_APP_ICON "")
        endif()
        if (ST_APP_ICON AND EXISTS "${ST_APP_ICON}")
            configure_file(
                ${SIMTARY_FRAMEWORK_DIR}/app.rc.in
                ${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.rc
                @ONLY
            )
            target_sources(${APP_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.rc)
        elseif (APP_ICON)
            message(WARNING "simtary_add_app(${APP_NAME}): ICON '${APP_ICON}' not found - no .rc generated")
        endif()
    endif()

    # include paths + libraries
    # Applied to every target in _compile_targets. For the executable that means a
    # real link; for the MODULE-mode object libraries it means the include paths and
    # compile definitions only, since an OBJECT library takes usage requirements
    # without linking anything. Both halves therefore compile against exactly the
    # same headers and flags, which is what the build id at the bottom then asserts.
    set(_app_link_libraries
        Simtary::AppFlags   # exceptions-off / RTTI-off contract, matching the engine
        Simtary
        SimtaryModelIO      # tinygltf + ufbx, for Framework/io/model (both are C)
        SDL2::SDL2
        ImGui_Lib
        sentry::sentry
        libzmq-static   # ZeroMQ C API (zmq.h); propagates include dir + ZMQ_STATIC + sys libs
        OpenAL          # openal-soft; also linked PUBLIC by Simtary_common (stAudio.cpp)
        libgfx          # software 2D rasterizer (GFX.h); propagates its include dir + GFXSDL
        ${APP_EXTRA_LIBS}
    )

    foreach (_target IN LISTS _compile_targets)
        target_include_directories(${_target} PRIVATE
            ${SIMTARY_FRAMEWORK_DIR}    # stApp.h, stRun.h, io/, sysui/, input/, ...
            ${SIMTARY_ROOT}/Engine      # Simtary.h and the rest of the engine core
            ${SIMTARY_ROOT}/include     # vendored headers (faust ABI, stb_image)
            ${SIMTARY_ROOT}/assets/shaders  # shader interop headers shared with C++ (StProjectorInterop.h, StLaserInterop.h)
            ${APP_SOURCE_DIR}           # the project's own scenes/ + components/
            ${_gendir}                  # generated version.h
            ${APP_EXTRA_INCLUDES}
        )
        target_link_libraries(${_target} PRIVATE ${_app_link_libraries})
    endforeach()

    if (WIN32)
        # dbghelp powers the symbolized crash summary (StackWalk64) in Framework/crash/CrashHandler.cpp
        target_link_libraries(${APP_NAME} PRIVATE dbghelp)
        # user32/gdi32/dwmapi: native Win32 loading window (Framework/SubWinStatus.cpp) + window styling
        target_link_libraries(${APP_NAME} PRIVATE user32 gdi32 dwmapi)
        # shell32/ole32: SHGetKnownFolderPath + CoTaskMemFree for the LocalLow user-data path
        target_link_libraries(${APP_NAME} PRIVATE shell32 ole32)
    elseif (UNIX)
        # X11: native Xlib loading window (Framework/SubWinStatus.cpp)
        find_package(X11 REQUIRED)
        target_include_directories(${APP_NAME} PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(${APP_NAME} PRIVATE ${X11_LIBRARIES})
    endif()

    if (MSVC)
        # Remove these if you don't need crash symbolization
        foreach (_target IN LISTS _compile_targets)
            target_compile_options(${_target} PRIVATE $<$<CONFIG:Release>:/Zi>)
        endforeach()
        target_link_options(${APP_NAME} PRIVATE
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    endif()

    # project module
    if (APP_MODULE)
        # Everything that decides how the shared C++ types are LAID OUT. Host and
        # module compare this string at load time and refuse to run as a mismatched
        # pair, because that mismatch has no other symptom: SIMTARY_LARGE_WORLD alone
        # changes sizeof(TransformComponent) by 12 bytes, and a host walking a scene
        # array at the module's stride reads plausible garbage rather than crashing.
        set(_engine_build_id
            "${_app_version}-${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_COMPILER_VERSION}-lw${SIMTARY_LARGE_WORLD}-rtti${WICKED_ENABLE_RTTI}")

        foreach (_target IN LISTS _compile_targets)
            target_compile_definitions(${_target} PRIVATE
                ST_ENGINE_BUILD_ID="${_engine_build_id}-$<CONFIG>"
                ST_MODULE_NAME="${APP_MODULE_NAME}"
            )
        endforeach()
        # ST_MODULE_HOST turns on Framework/stModuleMain.cpp (the loader main) and
        # Framework/stModuleHost.cpp; ST_MODULE_BUILD switches ST_APP_ENTRY in the
        # project's own source from an int main() to the exported descriptor.
        target_compile_definitions(${_framework_target} PRIVATE ST_MODULE_HOST)
        target_compile_definitions(${_gamecode_target}  PRIVATE ST_MODULE_BUILD)

        # The host has to export symbols for the module to bind to. On ELF that is
        # all this takes (CMake adds --export-dynamic); on Windows it also needs the
        # explicit export list built below, and it is what produces the import
        # library the module links against.
        set_target_properties(${APP_NAME} PROPERTIES ENABLE_EXPORTS TRUE)

        if (MSVC)
            # derived export list
            # NOT WINDOWS_EXPORT_ALL_SYMBOLS: the engine archive alone publishes about
            # 64,700 symbols and a PE export table holds 65,535, so exporting the
            # engine wholesale does not fit. cmake/simtary_module_def.ps1 intersects
            # the module's undefined externals with what the host defines instead,
            # which is a few thousand names and needs no dllexport annotation
            # anywhere in the engine.
            if (NOT SIMTARY_DUMPBIN)
                get_filename_component(_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
                find_program(SIMTARY_DUMPBIN dumpbin HINTS "${_msvc_bin}")
                if (NOT SIMTARY_DUMPBIN)
                    message(FATAL_ERROR
                        "simtary_add_app(${APP_NAME} MODULE): dumpbin.exe not found next to "
                        "the linker (${CMAKE_LINKER}). Set -DSIMTARY_DUMPBIN=<path>.")
                endif()
            endif()
            find_program(SIMTARY_POWERSHELL NAMES powershell.exe powershell)
            if (NOT SIMTARY_POWERSHELL)
                message(FATAL_ERROR "simtary_add_app(${APP_NAME} MODULE): powershell.exe not found.")
            endif()

            # Only STATIC inputs belong in the "defined by the host" set. A shared
            # library's import lib publishes __imp_ stubs, not code: the module links
            # SDL2 and OpenAL itself, and re-exporting their imports from the host
            # would bind the module to the wrong thing.
            set(_def_inputs "$<TARGET_OBJECTS:${_framework_target}>")
            set(_def_lib_files "")
            _simtary_collect_static_libs(${APP_NAME} _static_libs)
            foreach (_lib IN LISTS _static_libs)
                list(APPEND _def_inputs "$<TARGET_FILE:${_lib}>")
                list(APPEND _def_lib_files "$<TARGET_FILE:${_lib}>")
            endforeach()
            list(LENGTH _static_libs _static_lib_count)
            message(STATUS "${APP_NAME}: module export scan covers ${_static_lib_count} static libraries")

            set(_moduledir  ${CMAKE_CURRENT_BINARY_DIR}/module)
            set(_deffile    ${_moduledir}/${APP_NAME}_exports.def)
            set(_undeflist  ${_moduledir}/gamecode_objects.$<CONFIG>.txt)
            set(_deflist    ${_moduledir}/host_inputs.$<CONFIG>.txt)

            # Response files: the object lists are long and per-configuration, and
            # file(GENERATE) is the only place a $<TARGET_OBJECTS:> list can be turned
            # into text at configure time.
            file(GENERATE OUTPUT ${_undeflist}
                 CONTENT "$<JOIN:$<TARGET_OBJECTS:${_gamecode_target}>,\n>\n")
            file(GENERATE OUTPUT ${_deflist}
                 CONTENT "$<JOIN:${_def_inputs},\n>\n")

            # The .def is the OUTPUT and the generator leaves its timestamp alone when
            # the symbol set has not changed. Both halves of that matter: the step
            # itself re-runs on every build (it has to - only the fresh object files
            # know what the game now references), while the host relinks only when the
            # export list genuinely moved.
            add_custom_command(
                OUTPUT ${_deffile}
                COMMAND ${SIMTARY_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                        -File ${SIMTARY_ROOT}/cmake/simtary_module_def.ps1
                        -Dumpbin ${SIMTARY_DUMPBIN}
                        -UndefinedFrom ${_undeflist}
                        -DefinedFrom ${_deflist}
                        -Output ${_deffile}
                # FILE-level dependencies, not just the target names. With targets
                # alone the generator has no declared inputs, so the build treats an
                # existing .def as up to date forever: a symbol added to the engine
                # after the first configure never reaches the export list, and the
                # module fails to link against something the host plainly has.
                DEPENDS $<TARGET_OBJECTS:${_gamecode_target}>
                        $<TARGET_OBJECTS:${_framework_target}>
                        ${_def_lib_files}
                        ${_undeflist} ${_deflist}
                        ${SIMTARY_ROOT}/cmake/simtary_module_def.ps1
                        ${_gamecode_target} ${_framework_target}
                COMMENT "Deriving ${APP_NAME} module export list"
                VERBATIM
            )
            add_custom_target(${APP_NAME}_ModuleExports DEPENDS ${_deffile})
            set_target_properties(${APP_NAME}_ModuleExports PROPERTIES FOLDER "${APP_NAME}/Build")
            add_dependencies(${APP_NAME} ${APP_NAME}_ModuleExports)

            target_link_options(${APP_NAME} PRIVATE "/DEF:${_deffile}")
        endif()

        # What the module links for itself, and deliberately nothing else. SDL2 and
        # OpenAL are shared libraries, so both halves binding to the same DLL is one
        # copy and one set of globals. Every static library stays host-side: linking
        # ImGui here would give the module its own ImGui context, and linking the
        # engine here would give it a second wi::shared_ptr allocator table.
        target_link_libraries(${_module_target} PRIVATE
            ${APP_NAME}     # the host's import library - the whole point
            SDL2::SDL2
            OpenAL
        )

        if (MSVC)
            # The same crash-symbolization contract the host gets. Without it the module
            # ships with no PDB, and since the module is where the GAME lives, every
            # crash report and every stack walk stops at a bare address exactly where it
            # was about to become useful.
            target_link_options(${_module_target} PRIVATE
                $<$<CONFIG:Release>:/DEBUG>
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
            )
        endif()

        set_target_properties(${_framework_target} PROPERTIES FOLDER "${APP_NAME}")
        set_target_properties(${_gamecode_target}  PROPERTIES FOLDER "${APP_NAME}")
        set_target_properties(${_module_target}    PROPERTIES FOLDER "${APP_NAME}")

        message(STATUS "${APP_NAME}: module layout - host ${APP_NAME}, module ${APP_MODULE_NAME}")
    endif()

    # crash reporter
    # The reporter GUI and the Crashpad handler must sit next to the game exe;
    # Framework/crash/CrashHandler.cpp looks for them there at runtime.
    if (NOT APP_NO_CRASH_REPORTER)
        add_dependencies(${APP_NAME} SimtaryCrashReporter)
        add_custom_command(TARGET ${APP_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SimtaryCrashReporter> $<TARGET_FILE_DIR:${APP_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:crashpad_handler> $<TARGET_FILE_DIR:${APP_NAME}>
            COMMENT "Copying SimtaryCrashReporter + crashpad_handler to output directory"
            VERBATIM
        )
    endif()

    # framework shaders
    # ImGui's backend shaders and the framework's procedural lens flare. Named
    # StLensFlare* rather than LensFlare*: the engine ships its own lensFlareVS/PS
    # and both land in the same output folder, which is case-insensitive on Windows.
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE vs_6_0 SOURCE ${SIMTARY_ROOT}/assets/shaders/ImGuiVS.hlsl)
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE ps_6_0 SOURCE ${SIMTARY_ROOT}/assets/shaders/ImGuiPS.hlsl)
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE vs_6_0 SOURCE ${SIMTARY_ROOT}/assets/shaders/StLensFlareVS.hlsl)
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE ps_6_0 SOURCE ${SIMTARY_ROOT}/assets/shaders/StLensFlarePS.hlsl)

    # ENGINE_ENV: unlike the shaders above, these run inside the engine's frame - they
    # read the depth buffer, the camera constants and the bindless heaps, so they
    # include globals.hlsli and need the engine's compiler flags.
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE cs_6_0 ENGINE_ENV
        SOURCE ${SIMTARY_ROOT}/assets/shaders/StProjectorCS.hlsl)
    simtary_compile_shader(TARGET ${APP_NAME} PROFILE cs_6_0 ENGINE_ENV
        SOURCE ${SIMTARY_ROOT}/assets/shaders/StLaserCS.hlsl)

    # engine shader sources + shared compiled cache
    # The engine compiles its ~360 HLSL shaders on first launch and caches the DXIL
    # in "<exe>/shaders/hlsl6/" (Vulkan: "spirv/"). Simtary/shaders/ holds that cache
    # checked in at the engine level, so it is SHARED: every project starts from the
    # warm cache instead of paying the cold compile itself. Editing a widely-included
    # header (globals.hlsli) invalidates it, and the incremental pre-pass below
    # recompiles only what changed.
    add_custom_command(TARGET ${APP_NAME} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
            ${SIMTARY_ROOT}/Engine/shaders $<TARGET_FILE_DIR:${APP_NAME}>/shaders
        COMMENT "Staging engine shader sources -> <exe>/shaders/"
        VERBATIM
    )

    if (EXISTS ${SIMTARY_ROOT}/shaders)
        add_custom_command(TARGET ${APP_NAME} PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
                ${SIMTARY_ROOT}/shaders $<TARGET_FILE_DIR:${APP_NAME}>/shaders
            COMMENT "Staging the shared engine shader cache -> <exe>/shaders/"
            VERBATIM
        )
    endif()

    # Incremental pre-pass so the first launch is never a cold compile. Turn off with
    # NO_SHADER_WARM for the fastest possible build, at the cost of a slow first run.
    if (NOT APP_NO_SHADER_WARM)
        if (WIN32)
            set(_warm hlsl6) # DX12 default; pass spirv too if you run with 'vulkan'
        else()
            set(_warm spirv)
        endif()
        add_dependencies(${APP_NAME} offlineshadercompiler)
        add_custom_command(TARGET ${APP_NAME} POST_BUILD
            COMMAND offlineshadercompiler ${_warm} strip_reflection quiet
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${APP_NAME}>
            COMMENT "Warming engine shader cache (incremental) -> shaders/${_warm}/"
            USES_TERMINAL
            VERBATIM
        )

        # Opt-in: fold the freshly compiled shaders back into Simtary/shaders so the
        # next project (and the next clean build) starts warm. Never automatic - it
        # writes into the shared engine tree.
        if (NOT TARGET simtary_shadercache_update)
            add_custom_target(simtary_shadercache_update
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    $<TARGET_FILE_DIR:${APP_NAME}>/shaders/${_warm}
                    ${SIMTARY_ROOT}/shaders/${_warm}
                COMMENT "Publishing <exe>/shaders/${_warm} back into Simtary/shaders/${_warm}"
                VERBATIM
            )
            set_target_properties(simtary_shadercache_update PROPERTIES FOLDER "Simtary")
        endif()
    endif()

    # runtime DLLs
    # PRE_LINK, not POST_BUILD, and that ordering is load-bearing. The shader warm
    # step above runs offlineshadercompiler, which links the engine and therefore
    # imports OpenAL32.dll and phonon.dll; it finds them through its WORKING_DIRECTORY,
    # which is this output directory. Copying them in POST_BUILD puts them there AFTER
    # the tool has already tried to start, so a clean tree dies with exit code
    # -1073741515 (0xC0000135, STATUS_DLL_NOT_FOUND) before any DLL is staged. It only
    # ever appeared to work because a previous build had left OpenAL32.dll behind.
    add_custom_command(TARGET ${APP_NAME} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${APP_NAME}>
        COMMENT "Copying SDL2 runtime library to output directory"
        VERBATIM
    )
    # openal-soft is built shared; its runtime DLL (OpenAL32.dll) must sit next to
    # the executable so the audio engine can open the default output device.
    add_custom_command(TARGET ${APP_NAME} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL> $<TARGET_FILE_DIR:${APP_NAME}>
        COMMENT "Copying OpenAL runtime library to output directory"
        VERBATIM
    )
    # Steam Audio ships prebuilt; phonon.dll has to sit next to the executable for
    # the same reason. Absent (SIMTARY_ENABLE_STEAMAUDIO=OFF, or no build for this
    # platform) the engine runs its fallback panner and nothing needs staging.
    if (TARGET SteamAudio)
        add_custom_command(TARGET ${APP_NAME} PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SIMTARY_STEAMAUDIO_RUNTIME}" $<TARGET_FILE_DIR:${APP_NAME}>
            COMMENT "Copying Steam Audio runtime library to output directory"
            VERBATIM
        )
    endif()

    # assets
    # Two copies, deliberately:
    #   <build>/assets     the whole project assets tree (source shaders, dsp, ...)
    #   <exe>/assets       just the game content, which is what the running game
    #                      resolves paths against ("assets/splash.bmp", scenes, ...)
    #
    # BOTH live on a custom target, not on POST_BUILD. A POST_BUILD command only runs
    # when the executable itself is rebuilt, so editing nothing but a .wiscene left
    # the old copy sitting in the output: the build was "up to date" and the copy step
    # never fired. A custom target is always considered out of date, so `cmake --build`
    # re-syncs content whether or not a single line of C++ changed.
    if (EXISTS ${APP_ASSETS_DIR})
        set(_asset_copy_commands
            COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
                ${APP_ASSETS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assets
        )
        if (APP_CONTENT_SUBDIR AND EXISTS ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR})
            list(APPEND _asset_copy_commands
                # make_directory first: on a clean tree the output folder does not
                # exist yet when this runs (it runs BEFORE the link, not after).
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${APP_NAME}>/assets
            )
            # Packing and copying loose are ALTERNATIVES, not companions.
            #
            # stpack takes every regular file under the content directory (tools/stpack.cpp:
            # the only thing it skips is *.wiscene, and only because it has already
            # converted those into .stsd). So once the package is built, a loose copy of
            # that tree is a second copy of every asset - and a copy nothing will ever
            # read: AssetSystem::Install() puts the packs in front of the filesystem, so a
            # path the package holds always resolves out of the package. For Milistry that
            # was 56 MB of dead weight beside a 90 MB package, most of it one 53 MB .wav.
            #
            # It is also a staleness hazard in the one case it IS read: SDL loads the
            # splash straight off disk before the override exists (Framework/stRun.cpp), so
            # an out-of-date loose splash.bmp wins over the packed one.
            #
            # This replaces an older rule that copied the whole tree and then deleted just
            # the .wiscene and .staod files from the output. That left every other packed
            # asset duplicated, and it re-copied them on EVERY build: copy_if_different
            # sees the destination missing (we deleted it) and copies again.
            #
            # PACK_ONLY therefore now describes what PACK_ASSETS already does; it is kept
            # as an accepted spelling so existing projects keep configuring.
            if (NOT APP_PACK_ONLY AND NOT APP_PACK_ASSETS)
                list(APPEND _asset_copy_commands
                    COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
                        ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
                        $<TARGET_FILE_DIR:${APP_NAME}>/assets
                )
            endif()
        endif()

        # The maps. With PACK_ASSETS the packer writes <exe>/assets/<scenes>/*.stsd
        # itself, and copying the .wiscene sources beside them would be exactly the
        # duplicate this layout exists to remove. Without it the .wiscene IS the shipped
        # map, so it has to be there.
        if (APP_SCENE_DIR AND NOT APP_PACK_ASSETS AND NOT APP_PACK_ONLY)
            list(APPEND _asset_copy_commands
                COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
                    ${APP_SCENE_DIR}
                    $<TARGET_FILE_DIR:${APP_NAME}>/assets/${APP_SCENE_SUBDIR}
            )
        endif()

        add_custom_target(${APP_NAME}_Assets
            ${_asset_copy_commands}
            COMMENT "Syncing ${APP_NAME} assets -> <build>/assets and <exe>/assets"
            VERBATIM
        )
        set_target_properties(${APP_NAME}_Assets PROPERTIES FOLDER "${APP_NAME}/Build")
        add_dependencies(${APP_NAME} ${APP_NAME}_Assets)

        # copy_directory_if_different only ADDS and OVERWRITES; it never deletes. A
        # renamed or removed asset therefore lingers in the output and the game can
        # still load it, which hides the breakage until someone ships. This target
        # wipes the output copies first, so build it after deleting or renaming
        # content:  cmake --build <dir> --target ${APP_NAME}_AssetsResync
        if (APP_CONTENT_SUBDIR AND EXISTS ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR})
            set(_asset_resync_commands
                COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${APP_NAME}>/assets
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/assets
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${APP_ASSETS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assets
            )
            # Same rule as the ordinary copy above: a packed build ships the package, not
            # a loose second copy of everything in it.
            if (NOT APP_PACK_ONLY AND NOT APP_PACK_ASSETS)
                list(APPEND _asset_resync_commands
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
                        $<TARGET_FILE_DIR:${APP_NAME}>/assets
                )
            endif()
            if (APP_SCENE_DIR AND NOT APP_PACK_ASSETS AND NOT APP_PACK_ONLY)
                list(APPEND _asset_resync_commands
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        ${APP_SCENE_DIR}
                        $<TARGET_FILE_DIR:${APP_NAME}>/assets/${APP_SCENE_SUBDIR}
                )
            endif()
            if (APP_PACK_ASSETS OR APP_PACK_ONLY)
                # The wipe above took the package with it, so the stamp has to go too or
                # the next ordinary build thinks the pack is still current and never
                # rebuilds it. Dropping the stamp is enough; ${APP}_Pack does the rest.
                if (NOT APP_PACK_NAME)
                    set(_resync_pack_name "content")
                else()
                    set(_resync_pack_name ${APP_PACK_NAME})
                endif()
                list(APPEND _asset_resync_commands
                    COMMAND ${CMAKE_COMMAND} -E rm -f
                        ${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}_${_resync_pack_name}.packstamp
                )
            endif()

            add_custom_target(${APP_NAME}_AssetsResync
                ${_asset_resync_commands}
                COMMENT "Re-syncing ${APP_NAME} assets from scratch (drops removed files)"
                VERBATIM
            )
            set_target_properties(${APP_NAME}_AssetsResync PROPERTIES FOLDER "${APP_NAME}/Build")
        endif()
    endif()

    # asset package
    if (APP_PACK_ASSETS OR APP_PACK_ONLY)
        if (NOT APP_PACK_NAME)
            set(APP_PACK_NAME "content")
        endif()
        if (NOT APP_PACK_PART_SIZE)
            set(APP_PACK_PART_SIZE 50)
        endif()
        if (NOT APP_PACK_LEVEL)
            set(APP_PACK_LEVEL 9)
        endif()
        simtary_pack_assets(
            TARGET        ${APP_NAME}
            CONTENT_DIR   ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
            SCENE_SRC_DIR ${APP_SCENE_DIR}
            SCENE_SUBDIR  ${APP_SCENE_SUBDIR}
            NAME          ${APP_PACK_NAME}
            PART_SIZE     ${APP_PACK_PART_SIZE}
            LEVEL         ${APP_PACK_LEVEL}
        )
    endif()

    set_target_properties(${APP_NAME} PROPERTIES FOLDER "${APP_NAME}")
endfunction()

# simtary_pack_assets()
# Build a .strd + .stafp<N> package from a content directory and drop it next to the
# executable, converting every .wiscene it finds into a .stsd on the way.
#
#   simtary_pack_assets(TARGET Milistry CONTENT_DIR .../assets/contents
#                       [SCENE_SRC_DIR .../assets/scenes]
#                       [NAME content] [PART_SIZE 50] [LEVEL 9]
#                       [PACK_SUBDIR resources] [SCENE_SUBDIR scenes])
#
# Output layout under <exe>/assets/:
#
#   resources/content.strd      the index
#   resources/content.stafp1..N  the payload parts
#   scenes/<map>.stsd            one descriptor per converted .wiscene, LOOSE
#
# The maps stay out of the package on purpose. A .stsd is a few KB of NBT metadata in
# front of a compressed entity blob, so leaving it visible costs nothing and makes a
# map listable, diffable and hand-swappable without unpacking anything - while the
# bulk, the textures and meshes every map shares, is the part that belongs in a
# deduplicated package.
#
# SCENE_SRC_DIR is where the .wiscene SOURCES are read from, and it is deliberately a
# second directory rather than a corner of CONTENT_DIR: everything under CONTENT_DIR
# becomes a packed resource, and a .wiscene is not a resource - it is the thing a .stsd
# is converted FROM. Keeping the two apart is what lets the content rule stay
# exception-free and stops a 37 MB map from also shipping loose. Maps left inside
# CONTENT_DIR are still converted, so an older project layout keeps working.
#
# Unlike <APP>_Assets, this is a real add_custom_command with real DEPENDS rather than
# an always-out-of-date target. It has to be: repacking 76 MB of maps through zstd on
# every build, whether or not a single asset changed, is tens of seconds per build. The
# input list is globbed with CONFIGURE_DEPENDS, so adding or removing a content file
# re-runs configure and the dependency set follows.
#
# The stamp file exists because the real outputs are N part files whose count is not
# known until the packer has run, and CMake needs one name it can depend on.
function(simtary_pack_assets)
    set(_one TARGET CONTENT_DIR SCENE_SRC_DIR NAME PART_SIZE LEVEL PACK_SUBDIR SCENE_SUBDIR)
    cmake_parse_arguments(PACK "" "${_one}" "" ${ARGN})

    if (NOT PACK_TARGET)
        message(FATAL_ERROR "simtary_pack_assets: TARGET is required")
    endif()
    if (NOT PACK_NAME)
        set(PACK_NAME "content")
    endif()
    if (NOT PACK_PART_SIZE)
        set(PACK_PART_SIZE 50)
    endif()
    if (NOT PACK_LEVEL)
        set(PACK_LEVEL 9)
    endif()
    if (NOT DEFINED PACK_PACK_SUBDIR)
        set(PACK_PACK_SUBDIR "resources")
    endif()
    if (NOT DEFINED PACK_SCENE_SUBDIR)
        set(PACK_SCENE_SUBDIR "scenes")
    endif()

    if (NOT PACK_CONTENT_DIR OR NOT EXISTS ${PACK_CONTENT_DIR})
        message(STATUS "simtary_pack_assets(${PACK_TARGET}): no content directory, skipping")
        return()
    endif()

    file(GLOB_RECURSE _pack_inputs CONFIGURE_DEPENDS ${PACK_CONTENT_DIR}/*)

    # The scene sources are inputs too, or editing a map would not repack it.
    set(_scene_src_args "")
    if (PACK_SCENE_SRC_DIR AND EXISTS ${PACK_SCENE_SRC_DIR})
        file(GLOB_RECURSE _scene_inputs CONFIGURE_DEPENDS ${PACK_SCENE_SRC_DIR}/*.wiscene)
        list(APPEND _pack_inputs ${_scene_inputs})
        set(_scene_src_args --scene-src ${PACK_SCENE_SRC_DIR})
    endif()

    if (NOT _pack_inputs)
        message(STATUS "simtary_pack_assets(${PACK_TARGET}): nothing to pack, skipping")
        return()
    endif()

    set(_stamp     ${CMAKE_CURRENT_BINARY_DIR}/${PACK_TARGET}_${PACK_NAME}.packstamp)
    set(_pack_dir  $<TARGET_FILE_DIR:${PACK_TARGET}>/assets/${PACK_PACK_SUBDIR})
    set(_scene_dir $<TARGET_FILE_DIR:${PACK_TARGET}>/assets/${PACK_SCENE_SUBDIR})

    add_custom_command(
        OUTPUT ${_stamp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_pack_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_scene_dir}
        COMMAND $<TARGET_FILE:stpack> pack ${PACK_CONTENT_DIR}
                --out ${_pack_dir}
                --scene-dir ${_scene_dir}
                ${_scene_src_args}
                --name ${PACK_NAME}
                --part-size ${PACK_PART_SIZE}
                --level ${PACK_LEVEL}
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS ${_pack_inputs} stpack
        COMMENT "Packing ${PACK_TARGET} content -> ${PACK_PACK_SUBDIR}/${PACK_NAME}.strd + .stafp<N>, maps -> ${PACK_SCENE_SUBDIR}/*.stsd"
        VERBATIM
    )

    add_custom_target(${PACK_TARGET}_Pack DEPENDS ${_stamp})
    set_target_properties(${PACK_TARGET}_Pack PROPERTIES FOLDER "${PACK_TARGET}/Build")
    add_dependencies(${PACK_TARGET} ${PACK_TARGET}_Pack)

    # Forces a repack even when nothing changed, for when the packer options moved or a
    # part file was deleted by hand.
    add_custom_target(${PACK_TARGET}_Repack
        # The parts are wiped first: the packer never writes fewer files than last time,
        # so a build that drops content would otherwise leave an orphaned .stafpN next
        # to the new index, and the reader would refuse the whole set for a UUID it does
        # not recognise.
        COMMAND ${CMAKE_COMMAND} -E rm -f ${_stamp}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_pack_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_pack_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_scene_dir}
        COMMAND $<TARGET_FILE:stpack> pack ${PACK_CONTENT_DIR}
                --out ${_pack_dir}
                --scene-dir ${_scene_dir}
                ${_scene_src_args}
                --name ${PACK_NAME}
                --part-size ${PACK_PART_SIZE}
                --level ${PACK_LEVEL}
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS stpack
        COMMENT "Re-packing ${PACK_TARGET} content from scratch"
        VERBATIM
    )
    set_target_properties(${PACK_TARGET}_Repack PROPERTIES FOLDER "${PACK_TARGET}/Build")
endfunction()

# simtary_faust_regen()
# Optional AOT Faust codegen. The generated instrument C++ is checked in so the
# build works without the Faust compiler; if `faust` is on PATH this adds a target
# that regenerates it (mirrors how dxc is found for shaders - a missing compiler is
# a warning, never a hard error).
#
#   simtary_faust_regen(NAME organ CLASS OrganDSP
#                       DSP assets/signal_descriptors/organ.dsp
#                       OUTPUT src/audio/faust/processor/organ.gen.h)
function(simtary_faust_regen)
    cmake_parse_arguments(FA "" "NAME;CLASS;DSP;OUTPUT" "" ${ARGN})
    find_program(FAUST_PATH faust)
    if (NOT FAUST_PATH)
        message(STATUS "Faust compiler not found - using the checked-in ${FA_OUTPUT}. "
                       "Install a prebuilt 'faust' and re-run CMake to enable 'faust_regen_${FA_NAME}'.")
        return()
    endif()
    message(STATUS "Faust compiler: ${FAUST_PATH} (target 'faust_regen_${FA_NAME}')")
    add_custom_target(faust_regen_${FA_NAME}
        COMMAND ${FAUST_PATH} -lang cpp -cn ${FA_CLASS}
            -a ${SIMTARY_ROOT}/assets/signal_descriptors/faust_arch.h
            -I ${SIMTARY_ROOT}/libs/faust/libraries
            -o ${CMAKE_CURRENT_SOURCE_DIR}/${FA_OUTPUT}
            ${CMAKE_CURRENT_SOURCE_DIR}/${FA_DSP}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Regenerating ${FA_OUTPUT} from ${FA_DSP} via Faust"
        VERBATIM
    )
endfunction()
