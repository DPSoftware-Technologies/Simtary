#pragma once
#include <imgui.h>
#include <Simtary.h>
#include "render/LensFlare.h"
#include "io/Nbt.h"

// Graphics settings window. Two tabs:
//   "Engine"  — every live graphics knob the engine exposes (RenderPath3D +
//               renderer globals + Application frame pacing).
//   "Content" — settings for effects Milistry owns itself, currently the
//               procedural lens flare (st::LensFlare).
//
// Engine tab edits go into a `pending_` snapshot; nothing touches the engine until
// the user clicks Apply. Reset discards unapplied edits (reverts pending_ to
// applied_). The Content tab is deliberately outside that model — those knobs are
// plain values read straight off st::LensFlare each frame, so they apply live and
// Apply/Reset do not affect them.
class GraphicsSettings {
public:
    // path = active 3D render path, app = owning application (for FPS / vsync),
    // lensFlare = Milistry's procedural flare, driven by the Content tab.
    void render(bool* isopen, wi::RenderPath3D& path, wi::Application& app, st::LensFlare& lensFlare);

    // ── persistence (options.stad via SettingsManager) ──────────────────────────
    // Serialize the current pending_ snapshot into an NBT compound, and load it back.
    void SaveTo(st::nbt::Tag& out) const;
    void LoadFrom(const st::nbt::Tag& in);
    // Load saved graphics (LoadFrom on the given tag) and push them straight to the
    // engine at startup. Also seeds applied_ so the UI opens in sync.
    void loadAndApply(const st::nbt::Tag& in, wi::RenderPath3D& path, wi::Application& app);

private:
    struct EngineGfx {
        // ── Ambient Occlusion / GI ──────────────────────────────────────────
        int   ao                 = 0;     // 0=Off 1=SSAO 2=HBAO 3=MSAO 4=RTAO
        float aoRange            = 1.0f;
        int   aoSampleCount      = 16;
        float aoPower            = 1.0f;
        bool  ssr                = false;
        int   ssrQuality         = 1;     // 0=Low 1=Medium 2=High
        bool  ssgi               = false;
        float ssgiDepthRejection = 8.0f;
        bool  rtReflections      = false;
        float rtReflectionsRange = 10000.0f;
        int   rtReflectionsQual  = 1;
        float reflectionRoughCut = 0.6f;
        bool  rtDiffuse          = false;
        float rtDiffuseRange     = 10.0f;
        int   rtDiffuseQual      = 1;
        bool  reflections        = true;  // planar reflections

        // ── Shadows ─────────────────────────────────────────────────────────
        bool  shadows            = true;
        int   shadowRes          = 2048;  // 1024 / 2048 / 4096
        int   sssSampleCount     = 16;    // screen space (contact) shadows
        float sssRange           = 1.0f;

        // ── Post processing ─────────────────────────────────────────────────
        bool  fxaa               = false;
        bool  bloom              = true;
        float bloomThreshold     = 1.0f;
        bool  motionBlur         = false;
        float motionBlurStrength = 100.0f;
        bool  dof                = true;
        float dofStrength        = 10.0f;
        bool  sharpen            = false;
        float sharpenAmount      = 0.28f;
        bool  crtFilter          = false;
        bool  eyeAdaption        = false;
        float eyeAdaptionKey     = 0.115f;
        float eyeAdaptionRate    = 1.0f;
        bool  lensFlare          = true;
        bool  lightShafts        = false;
        float lightShaftsStr     = 0.5f;
        float lightShaftsFade    = 3.0f;
        bool  volumeLights       = true;
        bool  chromaticAberr     = false;
        float chromaticAberrAmt  = 2.0f;
        bool  dither             = true;
        bool  outline            = false;
        float outlineThreshold   = 0.2f;
        float outlineThickness   = 1.0f;
        float outlineColor[4]    = { 0, 0, 0, 1 };

        // ── Color / tonemapping ─────────────────────────────────────────────
        int   tonemap            = 1;     // 0=Reinhard 1=ACES
        bool  colorGrading       = true;
        float exposure           = 1.0f;
        float hdrCalibration     = 1.0f;
        float brightness         = 0.0f;
        float contrast           = 1.0f;
        float saturation         = 1.0f;

        // ── Display / upscaling ─────────────────────────────────────────────
        bool  vsync              = true;
        int   msaa               = 1;     // 1 / 2 / 4 / 8
        bool  fsr                = false; // FSR 1.0 spatial
        float fsrSharpness       = 1.0f;
        bool  fsr2               = false; // FSR 2.0 temporal
        int   fsr2Preset         = 0;     // 0=Quality 1=Balanced 2=Performance 3=Ultra Perf
        float fsr2Sharpness      = 0.5f;
        float resolutionScale    = 1.0f;

        // ── Performance / culling ───────────────────────────────────────────
        bool  occlusionCulling   = true;
        bool  sceneUpdate        = true;
        bool  meshBlend          = true;
        bool  visibilityCompute  = false;

        // ── Frame pacing (Application) ──────────────────────────────────────
        bool  framerateLock      = false;
        float targetFrameRate    = 60.0f;
        bool  frameskip          = true;

        bool operator==(const EngineGfx& o) const;
        bool operator!=(const EngineGfx& o) const { return !(*this == o); }
    };

    void readFromEngine(wi::RenderPath3D& path, wi::Application& app); // engine -> snapshots
    void applyToEngine(wi::RenderPath3D& path, wi::Application& app);  // pending_ -> engine
    void drawEngineTab();
    void drawContentTab(st::LensFlare& lensFlare);

    EngineGfx applied_;          // values currently live in the engine
    EngineGfx pending_;          // values being edited in the UI
    bool      initialized_ = false;
    bool      autoApply_   = false; // push edits to the engine every frame

};
