#pragma once
// HurricanesRenderer - st::render on Google Filament (libs/filament): the Petrol edition's
// renderer (EDITIONS.md). Built only when CMake's SIMTARY_RENDERERS lists "Hurricanes" and
// tools/build_filament.ps1 has installed Filament (SIMTARY_HAS_HURRICANES).
//
// How a Hurricanes run works: Wicked keeps running everything that is not drawing - scene
// update, physics, terrain, scripts - on GraphicsDevice_Null, exactly as the headless server
// does, and this renderer takes the real GPU on the same window. Every frame it mirrors the
// wi::scene into a Filament scene and renders it:
//
//   ObjectComponent + MeshComponent   renderable, one primitive per LOD-0 subset
//   MeshComponent                     vertex / index buffers, built once per mesh
//   MaterialComponent                 a StLit material instance (base colour, roughness,
//                                     metalness, emissive, base colour map)
//   directional LightComponent        the sun
//   the game camera                   Filament camera
//
// Wicked is left-handed (+Z forward), Filament right-handed: positions, normals and matrices
// are mirrored in Z on the way in, which also turns Wicked's clockwise front faces into
// Filament's counter-clockwise ones.
//
// 2D (game UI, RmlUi, ImGui) is the Overlay (HurricanesUI.h): one batch over the 3D.
//
// SPIKE (EDITIONS.md Phase 3): no debug shapes, no skinning, no textures but base colour, no
// point/spot lights, no UI world canvases. Those calls are no-ops.
//
// Main thread only. No exceptions, no RTTI.

#include "render/api/Renderer.h"

#include <memory>

namespace st::render::hurricanes {

class HurricanesRenderer final : public Renderer {
public:
    // `nativeWindow`: the window's HWND (Windows). False from Ready() when Filament could not
    //	start (logged) - the run then has no picture.
    explicit HurricanesRenderer (void* nativeWindow);
    ~HurricanesRenderer () override;
    bool Ready () const;

    // Once a frame, after everything else: mirror the scene, render, present.
    void RenderFrame (uint32_t width, uint32_t height);
    void Shutdown ();

    Caps GetCaps () const override;
    Draw2D& UI () override;
    void Update (float dt) override;
    void LoadSettings () override {}
    void SaveSettings () override {}
    void SetDebugOverlays (const DebugOverlays&) override {}
    const TextureObject* GameImage () override;
    XMUINT2 GameResolution () const override;
    bool SaveImage (const TextureObject&, const std::string&) override { return false; }
    wi::Resource ToResource (const TextureRef&) override { return wi::Resource(); }
    void* ImGuiTexture (const TextureObject& texture) override;
    void* ImGuiTexture (const wi::Resource& resource) override;
    wi::primitive::Ray PickRay (float x, float y, float viewWidth, float viewHeight,
                                const wi::scene::CameraComponent* camera) override;
    void DebugLine (const XMFLOAT3&, const XMFLOAT3&, const XMFLOAT4&, const XMFLOAT4&, bool) override {}
    void DebugBox (const XMFLOAT4X4&, const XMFLOAT4&, bool) override {}
    void DebugSphere (const wi::primitive::Sphere&, const XMFLOAT4&, bool) override {}
    int PipelinesCompiling () const override { return 0; }
    std::string AdapterName () const override;
    bool VideoMemory (uint64_t&, uint64_t&) const override { return false; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace st::render::hurricanes
