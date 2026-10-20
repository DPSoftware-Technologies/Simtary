#pragma once
// st::render - the render interface (EDITIONS.md, "The render interface", Phase 1).
//
// Everything in Framework (and a game) that DRAWS or talks to the GPU goes through here,
// never through wi::renderer / wi::graphics / wi::image / wi::font directly. Scene data
// (wi::scene components) is NOT part of this: it stays the one renderer-neutral
// description, and each renderer draws what it supports of it.
//
//   Renderer   the active renderer: caps, the game view's final image, readback, picking,
//              debug lines, textures handed to materials, ImGui image ids, GPU status.
//   Draw2D     immediate-mode 2D for the game UI (UISystem, RmlUi): textures, images with
//              9-slice / fill / rounding / gradient, SDF text, compiled triangle geometry,
//              scissor - into the game image or into an off-screen target.
//
// One implementation today: render/detroit (Wicked's wi::renderer, Vulkan 1.3 / DX12).
// A second (Hurricanes, EDITIONS.md) implements the same classes. Files that may include
// a Wicked renderer header are listed in cmake/SimtaryRenderBoundary.cmake; the build
// fails when anything else does.
//
// Handles are shared_ptr to an object the renderer subclasses. Code outside the renderer
// only reads `width`, `height` and `premultiplied` off them.
//
// Main thread only, unless a function says otherwise. No exceptions, no RTTI.

#include "wiECS.h"
#include "wiMath.h"
#include "wiPrimitive.h"
#include "wiResourceManager.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace wi::scene { struct MaterialComponent; struct CameraComponent; }

namespace st::render {

enum class Backend : uint8_t { Detroit, Hurricanes };

struct Caps {
    Backend     backend = Backend::Detroit;
    const char* name = "";
    // The edition this run IS (EDITIONS.md): a build can carry several renderers and start
    //	on any of them, so the About box asks the renderer, not the build.
    const char* codename = "";       // "Detroit", "Gumin", "Hurricanes", ...
    const char* architecture = "";   // "Diesel", "Petrol", ...
    const char* api = "";            // what it drives the GPU with: "DirectX 12", "Vulkan", "OpenGL ES 3.0"
    const char* basedOn = "";        // the rendering library underneath: "Wicked Engine 0.72.59"
    bool bindless = false;
    bool compute = false;
    bool rayTracing = false;
    bool gpuParticles = false;   // EmittedParticleSystem / HairParticleSystem simulate
    uint32_t maxTextureSize = 4096;
};

// ------------------------------------------------------------------------- handles ---

class TextureObject {
public:
    virtual ~TextureObject () = default;
    uint32_t width = 0;
    uint32_t height = 0;
    bool premultiplied = false;   // colour already multiplied by alpha (generated glyph atlases)
};
using TextureRef = std::shared_ptr<TextureObject>;

class GeometryObject {
public:
    virtual ~GeometryObject () = default;
};
using GeometryRef = std::shared_ptr<GeometryObject>;

// Same memory as Rml::Vertex: position, premultiplied RGBA8 colour, texture coordinate.
struct Vertex2D {
    XMFLOAT2 position;
    uint32_t color;
    XMFLOAT2 uv;
};

// --------------------------------------------------------------------------- 2D draw ---

enum class Align : uint8_t { Start, Center, End };

struct ImageDraw {
    XMFLOAT4 rect = XMFLOAT4(0, 0, 0, 0);    // x, y, width, height - target pixels, top left
    XMFLOAT2 pivot = XMFLOAT2(0, 0);          // what `rotation` turns around, fraction of rect
    float    rotation = 0;                    // radians
    XMFLOAT4 color = XMFLOAT4(1, 1, 1, 1);    // straight alpha
    const TextureObject* texture = nullptr;   // null: solid colour
    bool     useSource = false;               // sample only `source` (texels x, y, w, h)
    XMFLOAT4 source = XMFLOAT4(0, 0, 0, 0);
    float    cornerRadius = 0;                // pixels, already clamped to half the short side
    bool     gradient = false;                // linear: `color` at gradientFrom -> gradientColor at gradientTo
    XMFLOAT2 gradientFrom = XMFLOAT2(0, 0);   // in 0..1 of the rect
    XMFLOAT2 gradientTo = XMFLOAT2(1, 0);
    XMFLOAT4 gradientColor = XMFLOAT4(1, 1, 1, 1);
};

struct TextDraw {
    const char* text = "";
    XMFLOAT2 position = XMFLOAT2(0, 0);       // the anchor; the alignments say which part of the text sits on it
    Align    hAlign = Align::Start;
    Align    vAlign = Align::Start;
    int      size = 16;                       // pixels
    XMFLOAT4 color = XMFLOAT4(1, 1, 1, 1);
    int      font = 0;                        // Draw2D::LoadFont; 0 = the default face
    float    bolden = 0;
    float    softness = 0;
    XMFLOAT2 spacing = XMFLOAT2(0, 0);        // extra pixels between letters / lines
    float    rotation = 0;                    // radians, around `position`
    float    wrapWidth = 0;                   // <= 0: no wrapping
    XMFLOAT4 shadowColor = XMFLOAT4(0, 0, 0, 0);   // alpha 0: no shadow
    XMFLOAT2 shadowOffset = XMFLOAT2(0, 0);
};

class Draw2D {
public:
    virtual ~Draw2D () = default;

    // --- resources (any time, main thread)
    // A file through the resource manager (so out of a mounted package too). Null on failure.
    virtual TextureRef LoadTexture (const std::string& path) = 0;
    // The texture of a resource someone already loaded (a material's, an editor field's).
    virtual TextureRef FromResource (const wi::Resource& resource) = 0;
    virtual TextureRef CreateTexture (const void* rgba8, uint32_t width, uint32_t height, bool premultiplied) = 0;
    // Something Begin() can draw into and a material / DrawImage can then show. RGBA8.
    virtual TextureRef CreateRenderTarget (uint32_t width, uint32_t height) = 0;
    // A font file; the id goes into TextDraw::font. 0 when it could not be loaded.
    virtual int LoadFont (const std::string& path) = 0;
    // Triangle lists for DrawGeometry (RmlUi). False from GeometryReady() when the backend
    //	cannot draw them at all (its shaders are missing) - reported once, by the backend.
    virtual bool GeometryReady () = 0;
    virtual GeometryRef CompileGeometry (const Vertex2D* vertices, size_t vertexCount,
                                         const int* indices, size_t indexCount) = 0;

    // --- drawing: Begin, any number of draws, End. Nothing nests.
    // `clear` fills the target with its clear colour first (opaque black for a
    //	CreateRenderTarget target). The scissor starts as the whole target.
    virtual bool Begin (const TextureObject& target, bool clear, const char* label = nullptr) = 0;
    virtual void End () = 0;
    virtual XMUINT2 TargetSize () const = 0;
    // Left, top, right, bottom in target pixels; later draws are cut to it.
    virtual void SetScissor (const XMFLOAT4& ltrb) = 0;
    virtual void DrawImage (const ImageDraw& image) = 0;
    virtual void DrawString (const TextDraw& text) = 0;
    // `toTarget` takes the geometry's vertices to target pixels (row vectors, the RmlUi
    //	convention); the backend adds its own projection. `texture` null: untextured.
    virtual void DrawGeometry (const GeometryObject& geometry, const TextureObject* texture,
                               const XMFLOAT4X4& toTarget) = 0;
};

// ------------------------------------------------------------------------- renderer ---

// The renderer's own debug drawing of scene data (the DevUI's Debug menu). A backend
//	draws what it can and ignores the rest.
struct DebugOverlays {
    bool boneLines = false;
    bool cameras = false;
    bool colliders = false;
    bool emitters = false;
    bool envProbes = false;
    bool forceFields = false;
    bool partitionTree = false;
    bool springs = false;
    bool voxelHelper = false;
    int  voxelClipmapLevel = 1;
    bool gridHelper = false;

    bool operator== (const DebugOverlays& o) const {
        return boneLines == o.boneLines && cameras == o.cameras && colliders == o.colliders &&
               emitters == o.emitters && envProbes == o.envProbes && forceFields == o.forceFields &&
               partitionTree == o.partitionTree && springs == o.springs && voxelHelper == o.voxelHelper &&
               voxelClipmapLevel == o.voxelClipmapLevel && gridHelper == o.gridHelper;
    }
    bool operator!= (const DebugOverlays& o) const { return !(*this == o); }
};

class Renderer {
public:
    virtual ~Renderer () = default;

    virtual Caps GetCaps () const = 0;
    virtual Draw2D& UI () = 0;

    // Once a frame, after the scene update: renderer-side effects that follow entities.
    virtual void Update (float dt) = 0;
    // The renderer's options in options.stad (st::SettingsManager): read at startup,
    //	written at exit.
    virtual void LoadSettings () = 0;
    virtual void SaveSettings () = 0;
    virtual void SetDebugOverlays (const DebugOverlays& overlays) = 0;

    // The game view's final image of this frame (post processed; the game UI is drawn onto
    //	it after the 3D). Null before the first frame. Valid until the next call.
    virtual const TextureObject* GameImage () = 0;
    // What the game view renders at (render scale applied).
    virtual XMUINT2 GameResolution () const = 0;

    // Waits for the GPU and writes `texture` to an image file (png / jpg / dds by extension).
    virtual bool SaveImage (const TextureObject& texture, const std::string& path) = 0;

    // A renderer-made texture where scene data keeps textures: MaterialComponent slots,
    //	emitter / sprite resources. The resource keeps the texture alive.
    virtual wi::Resource ToResource (const TextureRef& texture) = 0;

    // What ImGui::Image takes for a texture (an ImTextureID).
    virtual void* ImGuiTexture (const TextureObject& texture) = 0;
    virtual void* ImGuiTexture (const wi::Resource& resource) = 0;

    // A ray from `camera` (null: the game camera) through `x, y` - pixels of a view
    //	`viewWidth` x `viewHeight` showing it (the window, or an editor panel's image of it).
    virtual wi::primitive::Ray PickRay (float x, float y, float viewWidth, float viewHeight,
                                        const wi::scene::CameraComponent* camera = nullptr) = 0;

    // Debug shapes, drawn once in the next frame's game view. `depth`: hidden behind
    //	geometry. Thread safe.
    virtual void DebugLine (const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& fromColor,
                            const XMFLOAT4& toColor, bool depth = false) = 0;
    virtual void DebugBox (const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color, bool depth = true) = 0;
    virtual void DebugSphere (const wi::primitive::Sphere& sphere, const XMFLOAT4& color, bool depth = true) = 0;

    // GPU status for the loading screen, the menu bar and the video options.
    virtual int PipelinesCompiling () const = 0;
    virtual std::string AdapterName () const = 0;
    virtual bool VideoMemory (uint64_t& used, uint64_t& budget) const = 0;
};

// The active renderer. Get() is only valid after the app has installed one (st::App does
//	it in Initialize, before any scene loads).
Renderer& Get ();
bool Installed ();
void Install (Renderer* renderer);

// Shorthands for the common calls.
inline void DebugLine (const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& color, bool depth = false) {
    if (Installed()) Get().DebugLine(from, to, color, color, depth);
}

} // namespace st::render
