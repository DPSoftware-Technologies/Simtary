# SimtaryRenderBoundary.cmake - keeps Framework code on the render interface.
#
# EDITIONS.md, Phase 1: Framework (and game) code draws through st::render
# (Framework/render/api/Renderer.h), never through Wicked's renderer directly, so a second
# renderer (Hurricanes) can replace the first without touching it. This script fails the
# build when a Framework file outside the list below reaches into Wicked's rendering.
#
# Scene DATA types that happen to be declared in renderer headers stay allowed - a
# SpriteComponent's wi::image::Params, a mesh's wi::graphics::ShadingRate - because the
# scene is the renderer-neutral description (rule 2). What is refused is the DEVICE and
# the RENDERER: their headers, wi::renderer:: calls, GetDevice(), GPU object types, and
# wi::image / wi::font drawing.
#
# Run as a build step (cmake -P), re-run whenever a source changes:
#   -DFRAMEWORK_DIR=<Simtary/Framework>  -DSTAMP=<file to touch on success>
#   -DAPP_DIR=<game src, optional: reported as warnings, never fatal>

# Files that may use Wicked's renderer directly, relative to Framework/. Each group says why.
set(SIMTARY_DETROIT_ONLY
    # The Detroit backend itself.
    "render/detroit/.*"
    # The wi::Application host: owns the graphics device, and Compose(CommandList) /
    # the OnPreCompose / OnCompose project hooks are its overrides. stRun creates the
    # device and points the shader path at its backend's folder.
    "stApp\\.h" "stApp\\.cpp" "stRun\\.cpp"
    # Effects that ride on RenderPath3D's post-process chain with their own shaders.
    "render/LensFlare\\..*" "render/Projector\\..*" "render/Laser\\..*" "render/Framebuffer\\..*"
    # Backend selection and device creation (DirectX 12 / Vulkan).
    "render/GraphicsAPI\\..*"
    # Bound to the D3D12 device: OpenXR session, hardware video encode.
    "xr/XRSystem\\..*" "stream/StreamHost\\..*"
    # The editor's own 3D views (each a RenderPath3D), template previews, terrain brush
    # picking in the editor view, and the Detroit graphics options panel.
    "devui/imeditor\\..*" "devui/imtemplatepreview\\..*" "devui/imterrain\\.cpp" "devui/imgraphicsettings\\..*"
)

set(_forbidden
    "#include[ \t]*\"wi(Renderer|GraphicsDevice[A-Za-z0-9_]*|RenderPath[A-Za-z0-9_]*|Image|Font)\\.h\""
    "#include[ \t]*\"render/detroit/"
    "wi::renderer::"
    "GetDevice[ \t]*\\(\\)"
    "wi::graphics::(Texture|GPUBuffer|CommandList|PipelineState|Shader|Sampler|GraphicsDevice|RenderPass|SwapChain|InputLayout)[^A-Za-z0-9_]"
    "wi::(image|font)::(Draw|SetCanvas)"
    "using[ \t]+namespace[ \t]+wi::graphics"
)

function(_st_boundary_scan dir allowList outVar)
    file(GLOB_RECURSE _files RELATIVE "${dir}" "${dir}/*.cpp" "${dir}/*.h" "${dir}/*.inl")
    set(_hits "")
    foreach(_f IN LISTS _files)
        set(_allowed FALSE)
        foreach(_pattern IN LISTS allowList)
            if (_f MATCHES "^${_pattern}$")
                set(_allowed TRUE)
                break()
            endif()
        endforeach()
        if (_allowed)
            continue()
        endif()
        file(READ "${dir}/${_f}" _text)
        # Comments are prose: "wi::renderer::X does Y" in one is not a dependency.
        string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
        foreach(_re IN LISTS _forbidden)
            string(REGEX MATCH "${_re}" _m "${_text}")
            if (_m)
                list(APPEND _hits "  ${_f}: ${_m}")
            endif()
        endforeach()
    endforeach()
    set(${outVar} "${_hits}" PARENT_SCOPE)
endfunction()

if (NOT FRAMEWORK_DIR)
    message(FATAL_ERROR "SimtaryRenderBoundary.cmake: pass -DFRAMEWORK_DIR=")
endif()

_st_boundary_scan("${FRAMEWORK_DIR}" "${SIMTARY_DETROIT_ONLY}" _framework_hits)
if (APP_DIR AND EXISTS "${APP_DIR}")
    _st_boundary_scan("${APP_DIR}" "" _app_hits)
    if (_app_hits)
        list(JOIN _app_hits "\n" _msg)
        message(WARNING "Game code using Wicked's renderer directly (works on Detroit only; "
                        "st::render in Framework/render/api/Renderer.h is the portable way):\n${_msg}")
    endif()
endif()

if (_framework_hits)
    list(JOIN _framework_hits "\n" _msg)
    message(FATAL_ERROR
        "Framework code outside the render interface (EDITIONS.md, Phase 1):\n${_msg}\n"
        "Draw through st::render (Framework/render/api/Renderer.h), or - if the file genuinely "
        "needs the Detroit device - add it to SIMTARY_DETROIT_ONLY in cmake/SimtaryRenderBoundary.cmake "
        "with the reason.")
endif()

if (STAMP)
    file(TOUCH "${STAMP}")
endif()
