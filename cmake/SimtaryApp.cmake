# SimtaryApp.cmake — simtary_add_app(): turn a project directory into a game.
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
#       EXTRA_SOURCES  ...                 # sources outside SOURCE_DIR
#       EXTRA_INCLUDES ...
#       EXTRA_LIBS     ...
#       NO_SHADER_WARM                     # skip the engine shader pre-pass
#       NO_CRASH_REPORTER                  # do not ship SimtaryCrashReporter
#   )
#
# The framework (Simtary/Framework) is compiled INTO the app rather than linked as a
# shared static library: each app gets its own generated version.h (build counter,
# version, date) and its own AppConfig, so the objects are genuinely per-project.

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
# Turn this OFF for compile-checks that are not real project builds — the engine-side
# sweep (SIMTARY_BUILD_PROJECTS) and CI both do, so they never inflate the counter.
# version.h is still generated at configure time either way, so ST_APP_BUILD_NUMBER
# always resolves.
option(SIMTARY_BUMP_BUILD_NUMBER "Advance <project>/build_number.txt on every build" ON)

# ── simtary_compile_shader() ──────────────────────────────────────────────────
# Compile one HLSL file with dxc into the app's runtime shader folder
# (<exe>/shaders/hlsl6 on DX12, <exe>/shaders/spirv on Vulkan — the engine appends
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

# ── dxc lookup (once per configure) ───────────────────────────────────────────
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

# ── simtary_add_app() ─────────────────────────────────────────────────────────
function(simtary_add_app)
    set(_opts NO_SHADER_WARM NO_CRASH_REPORTER PACK_ASSETS PACK_ONLY)
    set(_one  NAME ORGANIZATION ICON SOURCE_DIR ASSETS_DIR CONTENT_SUBDIR
              PACK_NAME PACK_PART_SIZE PACK_LEVEL)
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
    # ── project descriptor ────────────────────────────────────────────────────
    # assets/project.stpd is the build-time manifest (identity, icon, version). It is
    # read here, at configure time, and never at runtime — which is why it lives in
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

    # ── sources ───────────────────────────────────────────────────────────────
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

    add_executable(${APP_NAME} ${_app_sources} ${_framework_sources} ${APP_EXTRA_SOURCES})
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

    # ── build number / versioning ─────────────────────────────────────────────
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

    if (SIMTARY_BUMP_BUILD_NUMBER)
        # Identity header generated from the descriptor, so main.cpp never repeats the
    # name/organization/copyright that the manifest already states.
    configure_file(
        ${SIMTARY_FRAMEWORK_DIR}/stProject.h.in
        ${_gendir}/stProject.h
        @ONLY
    )

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
        add_dependencies(${APP_NAME} ${APP_NAME}_BumpBuildNumber)
    else()
        message(STATUS "${APP_NAME}: build number frozen (SIMTARY_BUMP_BUILD_NUMBER=OFF)")
    endif()

    # ── Windows resources (icon + version info) ───────────────────────────────
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

    # ── include paths ─────────────────────────────────────────────────────────
    target_include_directories(${APP_NAME} PRIVATE
        ${SIMTARY_FRAMEWORK_DIR}    # stApp.h, stRun.h, io/, sysui/, input/, ...
        ${SIMTARY_ROOT}/Engine      # Simtary.h and the rest of the engine core
        ${SIMTARY_ROOT}/include     # vendored headers (faust ABI, stb_image)
        ${SIMTARY_ROOT}/assets/shaders  # shader interop headers shared with C++ (StProjectorInterop.h, StLaserInterop.h)
        ${APP_SOURCE_DIR}           # the project's own scenes/ + components/
        ${_gendir}                  # generated version.h
        ${APP_EXTRA_INCLUDES}
    )

    target_link_libraries(${APP_NAME} PRIVATE
        Simtary::AppFlags   # exceptions-off / RTTI-off contract, matching the engine
        Simtary
        SDL2::SDL2
        ImGui_Lib
        sentry::sentry
        libzmq-static   # ZeroMQ C API (zmq.h); propagates include dir + ZMQ_STATIC + sys libs
        OpenAL          # openal-soft; also linked PUBLIC by Simtary_common (stAudio.cpp)
        libgfx          # software 2D rasterizer (GFX.h); propagates its include dir + GFXSDL
        ${APP_EXTRA_LIBS}
    )

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
        target_compile_options(${APP_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(${APP_NAME} PRIVATE
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    endif()

    # ── crash reporter ────────────────────────────────────────────────────────
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

    # ── framework shaders ─────────────────────────────────────────────────────
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

    # ── engine shader sources + shared compiled cache ─────────────────────────
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
        # next project (and the next clean build) starts warm. Never automatic — it
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

    # ── runtime DLLs ──────────────────────────────────────────────────────────
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${APP_NAME}>
        COMMENT "Copying SDL2 runtime library to output directory"
        VERBATIM
    )
    # openal-soft is built shared; its runtime DLL (OpenAL32.dll) must sit next to
    # the executable so stAudio.cpp can open the default output device at runtime.
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL> $<TARGET_FILE_DIR:${APP_NAME}>
        COMMENT "Copying OpenAL runtime library to output directory"
        VERBATIM
    )

    # ── assets ────────────────────────────────────────────────────────────────
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
            # PACK_ONLY ships the .strd/.stafp set and nothing loose. The copy is
            # skipped rather than made and deleted, because the package lands in the
            # same folder and a delete would have to know which files it may touch.
            if (NOT APP_PACK_ONLY)
                list(APPEND _asset_copy_commands
                    COMMAND ${CMAKE_COMMAND} -E ${SIMTARY_COPY_DIR_CMD}
                        ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
                        $<TARGET_FILE_DIR:${APP_NAME}>/assets
                )

                # ...then take the .wiscene sources back out, but ONLY when they have
                # been packed. Without PACK_ASSETS the .wiscene IS the shipped map and
                # removing it would leave the game with nothing to load.
                #
                # copy_directory has no filter, so the tree copy above brings the maps
                # along with everything else. Once the packer has converted them, a
                # .wiscene in the output is ~37 MB of the SAME content the .stsd and the
                # package already hold: it doubles the build size, and it is a second
                # copy that can go stale and still be found first. The source of truth
                # stays in assets/contents/; the output keeps only the converted form.
                #
                # This lives on ${APP}_Assets rather than on the pack step because the
                # pack step is incremental - when it is up to date it does not run, and
                # the copy above has just put the .wiscene back.
                if (APP_PACK_ASSETS)
                    file(GLOB_RECURSE _packed_sources CONFIGURE_DEPENDS
                         ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}/*.wiscene)
                    foreach (_src IN LISTS _packed_sources)
                        file(RELATIVE_PATH _rel ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR} ${_src})
                        list(APPEND _asset_copy_commands
                            COMMAND ${CMAKE_COMMAND} -E rm -f
                                $<TARGET_FILE_DIR:${APP_NAME}>/assets/${_rel}
                        )
                    endforeach()
                endif()
            endif()
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
            if (NOT APP_PACK_ONLY)
                list(APPEND _asset_resync_commands
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
                        $<TARGET_FILE_DIR:${APP_NAME}>/assets
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

    # ── asset package ─────────────────────────────────────────────────────────
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
            TARGET      ${APP_NAME}
            CONTENT_DIR ${APP_ASSETS_DIR}/${APP_CONTENT_SUBDIR}
            NAME        ${APP_PACK_NAME}
            PART_SIZE   ${APP_PACK_PART_SIZE}
            LEVEL       ${APP_PACK_LEVEL}
        )
    endif()

    set_target_properties(${APP_NAME} PROPERTIES FOLDER "${APP_NAME}")
endfunction()

# ── simtary_pack_assets() ─────────────────────────────────────────────────────
# Build a .strd + .stafp<N> package from a content directory and drop it next to the
# executable, converting every .wiscene it finds into a .stsd on the way.
#
#   simtary_pack_assets(TARGET Milistry CONTENT_DIR .../assets/contents
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
# map listable, diffable and hand-swappable without unpacking anything — while the
# bulk, the textures and meshes every map shares, is the part that belongs in a
# deduplicated package.
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
    set(_one TARGET CONTENT_DIR NAME PART_SIZE LEVEL PACK_SUBDIR SCENE_SUBDIR)
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
    if (NOT _pack_inputs)
        message(STATUS "simtary_pack_assets(${PACK_TARGET}): content directory is empty, skipping")
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

# ── simtary_faust_regen() ─────────────────────────────────────────────────────
# Optional AOT Faust codegen. The generated instrument C++ is checked in so the
# build works without the Faust compiler; if `faust` is on PATH this adds a target
# that regenerates it (mirrors how dxc is found for shaders — a missing compiler is
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
