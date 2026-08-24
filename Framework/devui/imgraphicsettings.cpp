#include "imgraphicsettings.h"
#include "io/SettingsManager.h"

bool GraphicsSettings::EngineGfx::operator==(const EngineGfx& o) const {
    return ao == o.ao && aoRange == o.aoRange && aoSampleCount == o.aoSampleCount
        && aoPower == o.aoPower
        && ssr == o.ssr && ssrQuality == o.ssrQuality
        && ssgi == o.ssgi && ssgiDepthRejection == o.ssgiDepthRejection
        && rtReflections == o.rtReflections && rtReflectionsRange == o.rtReflectionsRange
        && rtReflectionsQual == o.rtReflectionsQual && reflectionRoughCut == o.reflectionRoughCut
        && rtDiffuse == o.rtDiffuse && rtDiffuseRange == o.rtDiffuseRange
        && rtDiffuseQual == o.rtDiffuseQual && reflections == o.reflections
        && shadows == o.shadows && shadowRes == o.shadowRes
        && sssSampleCount == o.sssSampleCount && sssRange == o.sssRange
        && fxaa == o.fxaa && bloom == o.bloom && bloomThreshold == o.bloomThreshold
        && motionBlur == o.motionBlur && motionBlurStrength == o.motionBlurStrength
        && dof == o.dof && dofStrength == o.dofStrength
        && sharpen == o.sharpen && sharpenAmount == o.sharpenAmount && crtFilter == o.crtFilter
        && eyeAdaption == o.eyeAdaption && eyeAdaptionKey == o.eyeAdaptionKey
        && eyeAdaptionRate == o.eyeAdaptionRate
        && lensFlare == o.lensFlare && lightShafts == o.lightShafts
        && lightShaftsStr == o.lightShaftsStr && lightShaftsFade == o.lightShaftsFade
        && volumeLights == o.volumeLights
        && chromaticAberr == o.chromaticAberr && chromaticAberrAmt == o.chromaticAberrAmt
        && dither == o.dither && outline == o.outline
        && outlineThreshold == o.outlineThreshold && outlineThickness == o.outlineThickness
        && outlineColor[0] == o.outlineColor[0] && outlineColor[1] == o.outlineColor[1]
        && outlineColor[2] == o.outlineColor[2] && outlineColor[3] == o.outlineColor[3]
        && tonemap == o.tonemap && colorGrading == o.colorGrading
        && exposure == o.exposure && hdrCalibration == o.hdrCalibration
        && brightness == o.brightness && contrast == o.contrast && saturation == o.saturation
        && msaa == o.msaa
        && fsr == o.fsr && fsrSharpness == o.fsrSharpness
        && fsr2 == o.fsr2 && fsr2Preset == o.fsr2Preset && fsr2Sharpness == o.fsr2Sharpness
        && resolutionScale == o.resolutionScale
        && occlusionCulling == o.occlusionCulling && sceneUpdate == o.sceneUpdate
        && meshBlend == o.meshBlend && visibilityCompute == o.visibilityCompute
        && frameskip == o.frameskip;
}

void GraphicsSettings::readFromEngine(wi::RenderPath3D& path, wi::Application& app) {
    EngineGfx g;
    g.ao                 = (int)path.getAO();
    g.aoRange            = path.getAORange();
    g.aoSampleCount      = (int)path.getAOSampleCount();
    g.aoPower            = path.getAOPower();
    g.ssr                = path.getSSREnabled();
    g.ssrQuality         = (int)path.getSSRQuality();
    g.ssgi               = path.getSSGIEnabled();
    g.ssgiDepthRejection = path.getSSGIDepthRejection();
    g.rtReflections      = path.getRaytracedReflectionEnabled();
    g.rtReflectionsRange = path.getRaytracedReflectionsRange();
    g.rtReflectionsQual  = (int)path.getRaytracedReflectionsQuality();
    g.reflectionRoughCut = path.getReflectionRoughnessCutoff();
    g.rtDiffuse          = path.getRaytracedDiffuseEnabled();
    g.rtDiffuseRange     = path.getRaytracedDiffuseRange();
    g.rtDiffuseQual      = (int)path.getRaytracedDiffuseQuality();
    g.reflections        = path.getReflectionsEnabled();

    g.shadows            = path.getShadowsEnabled();
    g.shadowRes          = applied_.shadowRes; // not queryable from renderer
    g.sssSampleCount     = (int)path.getScreenSpaceShadowSampleCount();
    g.sssRange           = path.getScreenSpaceShadowRange();

    g.fxaa               = path.getFXAAEnabled();
    g.bloom              = path.getBloomEnabled();
    g.bloomThreshold     = path.getBloomThreshold();
    g.motionBlur         = path.getMotionBlurEnabled();
    g.motionBlurStrength = path.getMotionBlurStrength();
    g.dof                = path.getDepthOfFieldEnabled();
    g.dofStrength        = path.getDepthOfFieldStrength();
    g.sharpen            = path.getSharpenFilterEnabled();
    g.sharpenAmount      = path.getSharpenFilterAmount();
    g.crtFilter          = path.getCRTFilterEnabled();
    g.eyeAdaption        = path.getEyeAdaptionEnabled();
    g.eyeAdaptionKey     = path.getEyeAdaptionKey();
    g.eyeAdaptionRate    = path.getEyeAdaptionRate();
    g.lensFlare          = path.getLensFlareEnabled();
    g.lightShafts        = path.getLightShaftsEnabled();
    g.lightShaftsStr     = path.getLightShaftsStrength();
    g.lightShaftsFade    = path.getLightShaftsFadeSpeed();
    g.volumeLights       = path.getVolumeLightsEnabled();
    g.chromaticAberr     = path.getChromaticAberrationEnabled();
    g.chromaticAberrAmt  = path.getChromaticAberrationAmount();
    g.dither             = path.getDitherEnabled();
    g.outline            = path.getOutlineEnabled();
    g.outlineThreshold   = path.getOutlineThreshold();
    g.outlineThickness   = path.getOutlineThickness();
    XMFLOAT4 oc          = path.getOutlineColor();
    g.outlineColor[0] = oc.x; g.outlineColor[1] = oc.y; g.outlineColor[2] = oc.z; g.outlineColor[3] = oc.w;

    g.tonemap            = (int)path.getTonemap();
    g.colorGrading       = path.getColorGradingEnabled();
    g.exposure           = path.getExposure();
    g.hdrCalibration     = path.getHDRCalibration();
    g.brightness         = path.getBrightness();
    g.contrast           = path.getContrast();
    g.saturation         = path.getSaturation();

    g.msaa               = (int)path.getMSAASampleCount();
    g.fsr                = path.getFSREnabled();
    g.fsrSharpness       = path.getFSRSharpness();
    g.fsr2               = path.getFSR2Enabled();
    g.fsr2Preset         = applied_.fsr2Preset; // preset is write-only on the path
    g.fsr2Sharpness      = path.getFSR2Sharpness();
    g.resolutionScale    = path.resolutionScale;

    g.occlusionCulling   = path.getOcclusionCullingEnabled();
    g.sceneUpdate        = path.getSceneUpdateEnabled();
    g.meshBlend          = path.getMeshBlendEnabled();
    g.visibilityCompute  = path.getVisibilityComputeShadingEnabled();

    g.frameskip          = applied_.frameskip;     // no getter

    applied_ = g;
    pending_ = g;
}

void GraphicsSettings::applyToEngine(wi::RenderPath3D& path, wi::Application& app) {
    const EngineGfx& g = pending_;

    // AO / GI
    path.setAO((wi::RenderPath3D::AO)g.ao);
    path.setAORange(g.aoRange);
    path.setAOSampleCount((uint32_t)g.aoSampleCount);
    path.setAOPower(g.aoPower);
    path.setSSREnabled(g.ssr);
    path.setSSRQuality((wi::renderer::PostProcessQuality)g.ssrQuality);
    path.setSSGIEnabled(g.ssgi);
    path.setSSGIDepthRejection(g.ssgiDepthRejection);
    path.setRaytracedReflectionsEnabled(g.rtReflections);
    path.setRaytracedReflectionsRange(g.rtReflectionsRange);
    path.setRaytracedReflectionsQuality((wi::renderer::PostProcessQuality)g.rtReflectionsQual);
    path.setReflectionRoughnessCutoff(g.reflectionRoughCut);
    path.setRaytracedDiffuseEnabled(g.rtDiffuse);
    path.setRaytracedDiffuseRange(g.rtDiffuseRange);
    path.setRaytracedDiffuseQuality((wi::renderer::PostProcessQuality)g.rtDiffuseQual);
    path.setReflectionsEnabled(g.reflections);

    // Shadows
    path.setShadowsEnabled(g.shadows);
    path.setScreenSpaceShadowSampleCount((uint32_t)g.sssSampleCount);
    path.setScreenSpaceShadowRange(g.sssRange);

    // Post processing
    path.setFXAAEnabled(g.fxaa);
    path.setBloomEnabled(g.bloom);
    path.setBloomThreshold(g.bloomThreshold);
    path.setMotionBlurEnabled(g.motionBlur);
    path.setMotionBlurStrength(g.motionBlurStrength);
    path.setDepthOfFieldEnabled(g.dof);
    path.setDepthOfFieldStrength(g.dofStrength);
    path.setSharpenFilterEnabled(g.sharpen);
    path.setSharpenFilterAmount(g.sharpenAmount);
    path.setCRTFilterEnabled(g.crtFilter);
    path.setEyeAdaptionEnabled(g.eyeAdaption);
    path.setEyeAdaptionKey(g.eyeAdaptionKey);
    path.setEyeAdaptionRate(g.eyeAdaptionRate);
    path.setLensFlareEnabled(g.lensFlare);
    path.setLightShaftsEnabled(g.lightShafts);
    path.setLightShaftsStrength(g.lightShaftsStr);
    path.setLightShaftsFadeSpeed(g.lightShaftsFade);
    path.setVolumeLightsEnabled(g.volumeLights);
    path.setChromaticAberrationEnabled(g.chromaticAberr);
    path.setChromaticAberrationAmount(g.chromaticAberrAmt);
    path.setDitherEnabled(g.dither);
    path.setOutlineEnabled(g.outline);
    path.setOutlineThreshold(g.outlineThreshold);
    path.setOutlineThickness(g.outlineThickness);
    path.setOutlineColor(XMFLOAT4(g.outlineColor[0], g.outlineColor[1], g.outlineColor[2], g.outlineColor[3]));

    // Color / tonemapping
    path.setTonemap((wi::renderer::Tonemap)g.tonemap);
    path.setColorGradingEnabled(g.colorGrading);
    path.setExposure(g.exposure);
    path.setHDRCalibration(g.hdrCalibration);
    path.setBrightness(g.brightness);
    path.setContrast(g.contrast);
    path.setSaturation(g.saturation);

    // Upscaling (FSR1/FSR2). FSR2 sharpness must be set before any preset apply.
    path.setFSREnabled(g.fsr);
    path.setFSRSharpness(g.fsrSharpness);
    path.setFSR2Enabled(g.fsr2);
    path.setFSR2Sharpness(g.fsr2Sharpness);

    // Performance / culling
    path.setOcclusionCullingEnabled(g.occlusionCulling);
    path.setSceneUpdateEnabled(g.sceneUpdate);
    path.setMeshBlendEnabled(g.meshBlend);
    path.setVisibilityComputeShadingEnabled(g.visibilityCompute);

    // Global renderer / display state.
    wi::renderer::SetShadowProps2D(g.shadowRes);

    // Frame pacing (Application).
    app.setFrameSkip(g.frameskip);

    // Anything that changes internal render-target size/sample-count needs a
    // buffer reallocation. FSR2 preset also drives resolutionScale internally.
    bool resize = false;
    if (g.msaa != applied_.msaa) { path.setMSAASampleCount((uint32_t)g.msaa); resize = true; }

    if (g.fsr2) {
        // FSR2 preset owns the resolution scale + sampler lod bias.
        if (g.fsr2Preset != applied_.fsr2Preset || g.fsr2 != applied_.fsr2) {
            path.setFSR2Preset((wi::RenderPath3D::FSR2_Preset)g.fsr2Preset);
            resize = true;
        }
    } else if (g.resolutionScale != applied_.resolutionScale || g.fsr2 != applied_.fsr2) {
        path.resolutionScale = g.resolutionScale;
        resize = true;
    }
    if (resize) path.ResizeBuffers();

    applied_ = pending_;
}

void GraphicsSettings::SaveTo(st::nbt::Tag& out) const {
    const EngineGfx& g = applied_; // persist what is actually live in the engine

    out.putInt   ("ao", g.ao);
    out.putFloat ("aoRange", g.aoRange);
    out.putInt   ("aoSampleCount", g.aoSampleCount);
    out.putFloat ("aoPower", g.aoPower);
    out.putBool  ("ssr", g.ssr);
    out.putInt   ("ssrQuality", g.ssrQuality);
    out.putBool  ("ssgi", g.ssgi);
    out.putFloat ("ssgiDepthRejection", g.ssgiDepthRejection);
    out.putBool  ("rtReflections", g.rtReflections);
    out.putFloat ("rtReflectionsRange", g.rtReflectionsRange);
    out.putInt   ("rtReflectionsQual", g.rtReflectionsQual);
    out.putFloat ("reflectionRoughCut", g.reflectionRoughCut);
    out.putBool  ("rtDiffuse", g.rtDiffuse);
    out.putFloat ("rtDiffuseRange", g.rtDiffuseRange);
    out.putInt   ("rtDiffuseQual", g.rtDiffuseQual);
    out.putBool  ("reflections", g.reflections);

    out.putBool  ("shadows", g.shadows);
    out.putInt   ("shadowRes", g.shadowRes);
    out.putInt   ("sssSampleCount", g.sssSampleCount);
    out.putFloat ("sssRange", g.sssRange);

    out.putBool  ("fxaa", g.fxaa);
    out.putBool  ("bloom", g.bloom);
    out.putFloat ("bloomThreshold", g.bloomThreshold);
    out.putBool  ("motionBlur", g.motionBlur);
    out.putFloat ("motionBlurStrength", g.motionBlurStrength);
    out.putBool  ("dof", g.dof);
    out.putFloat ("dofStrength", g.dofStrength);
    out.putBool  ("sharpen", g.sharpen);
    out.putFloat ("sharpenAmount", g.sharpenAmount);
    out.putBool  ("crtFilter", g.crtFilter);
    out.putBool  ("eyeAdaption", g.eyeAdaption);
    out.putFloat ("eyeAdaptionKey", g.eyeAdaptionKey);
    out.putFloat ("eyeAdaptionRate", g.eyeAdaptionRate);
    out.putBool  ("lensFlare", g.lensFlare);
    out.putBool  ("lightShafts", g.lightShafts);
    out.putFloat ("lightShaftsStr", g.lightShaftsStr);
    out.putFloat ("lightShaftsFade", g.lightShaftsFade);
    out.putBool  ("volumeLights", g.volumeLights);
    out.putBool  ("chromaticAberr", g.chromaticAberr);
    out.putFloat ("chromaticAberrAmt", g.chromaticAberrAmt);
    out.putBool  ("dither", g.dither);
    out.putBool  ("outline", g.outline);
    out.putFloat ("outlineThreshold", g.outlineThreshold);
    out.putFloat ("outlineThickness", g.outlineThickness);
    out.putFloat ("outlineColorR", g.outlineColor[0]);
    out.putFloat ("outlineColorG", g.outlineColor[1]);
    out.putFloat ("outlineColorB", g.outlineColor[2]);
    out.putFloat ("outlineColorA", g.outlineColor[3]);

    out.putInt   ("tonemap", g.tonemap);
    out.putBool  ("colorGrading", g.colorGrading);
    out.putFloat ("exposure", g.exposure);
    out.putFloat ("hdrCalibration", g.hdrCalibration);
    out.putFloat ("brightness", g.brightness);
    out.putFloat ("contrast", g.contrast);
    out.putFloat ("saturation", g.saturation);

    out.putInt   ("msaa", g.msaa);
    out.putBool  ("fsr", g.fsr);
    out.putFloat ("fsrSharpness", g.fsrSharpness);
    out.putBool  ("fsr2", g.fsr2);
    out.putInt   ("fsr2Preset", g.fsr2Preset);
    out.putFloat ("fsr2Sharpness", g.fsr2Sharpness);
    out.putFloat ("resolutionScale", g.resolutionScale);

    out.putBool  ("occlusionCulling", g.occlusionCulling);
    out.putBool  ("sceneUpdate", g.sceneUpdate);
    out.putBool  ("meshBlend", g.meshBlend);
    out.putBool  ("visibilityCompute", g.visibilityCompute);

    out.putBool  ("frameskip", g.frameskip);
}

void GraphicsSettings::LoadFrom(const st::nbt::Tag& in) {
    EngineGfx g; // start from struct defaults so any missing key keeps its default

    g.ao                 = in.getInt   ("ao", g.ao);
    g.aoRange            = in.getFloat ("aoRange", g.aoRange);
    g.aoSampleCount      = in.getInt   ("aoSampleCount", g.aoSampleCount);
    g.aoPower            = in.getFloat ("aoPower", g.aoPower);
    g.ssr                = in.getBool  ("ssr", g.ssr);
    g.ssrQuality         = in.getInt   ("ssrQuality", g.ssrQuality);
    g.ssgi               = in.getBool  ("ssgi", g.ssgi);
    g.ssgiDepthRejection = in.getFloat ("ssgiDepthRejection", g.ssgiDepthRejection);
    g.rtReflections      = in.getBool  ("rtReflections", g.rtReflections);
    g.rtReflectionsRange = in.getFloat ("rtReflectionsRange", g.rtReflectionsRange);
    g.rtReflectionsQual  = in.getInt   ("rtReflectionsQual", g.rtReflectionsQual);
    g.reflectionRoughCut = in.getFloat ("reflectionRoughCut", g.reflectionRoughCut);
    g.rtDiffuse          = in.getBool  ("rtDiffuse", g.rtDiffuse);
    g.rtDiffuseRange     = in.getFloat ("rtDiffuseRange", g.rtDiffuseRange);
    g.rtDiffuseQual      = in.getInt   ("rtDiffuseQual", g.rtDiffuseQual);
    g.reflections        = in.getBool  ("reflections", g.reflections);

    g.shadows            = in.getBool  ("shadows", g.shadows);
    g.shadowRes          = in.getInt   ("shadowRes", g.shadowRes);
    g.sssSampleCount     = in.getInt   ("sssSampleCount", g.sssSampleCount);
    g.sssRange           = in.getFloat ("sssRange", g.sssRange);

    g.fxaa               = in.getBool  ("fxaa", g.fxaa);
    g.bloom              = in.getBool  ("bloom", g.bloom);
    g.bloomThreshold     = in.getFloat ("bloomThreshold", g.bloomThreshold);
    g.motionBlur         = in.getBool  ("motionBlur", g.motionBlur);
    g.motionBlurStrength = in.getFloat ("motionBlurStrength", g.motionBlurStrength);
    g.dof                = in.getBool  ("dof", g.dof);
    g.dofStrength        = in.getFloat ("dofStrength", g.dofStrength);
    g.sharpen            = in.getBool  ("sharpen", g.sharpen);
    g.sharpenAmount      = in.getFloat ("sharpenAmount", g.sharpenAmount);
    g.crtFilter          = in.getBool  ("crtFilter", g.crtFilter);
    g.eyeAdaption        = in.getBool  ("eyeAdaption", g.eyeAdaption);
    g.eyeAdaptionKey     = in.getFloat ("eyeAdaptionKey", g.eyeAdaptionKey);
    g.eyeAdaptionRate    = in.getFloat ("eyeAdaptionRate", g.eyeAdaptionRate);
    g.lensFlare          = in.getBool  ("lensFlare", g.lensFlare);
    g.lightShafts        = in.getBool  ("lightShafts", g.lightShafts);
    g.lightShaftsStr     = in.getFloat ("lightShaftsStr", g.lightShaftsStr);
    g.lightShaftsFade    = in.getFloat ("lightShaftsFade", g.lightShaftsFade);
    g.volumeLights       = in.getBool  ("volumeLights", g.volumeLights);
    g.chromaticAberr     = in.getBool  ("chromaticAberr", g.chromaticAberr);
    g.chromaticAberrAmt  = in.getFloat ("chromaticAberrAmt", g.chromaticAberrAmt);
    g.dither             = in.getBool  ("dither", g.dither);
    g.outline            = in.getBool  ("outline", g.outline);
    g.outlineThreshold   = in.getFloat ("outlineThreshold", g.outlineThreshold);
    g.outlineThickness   = in.getFloat ("outlineThickness", g.outlineThickness);
    g.outlineColor[0]    = in.getFloat ("outlineColorR", g.outlineColor[0]);
    g.outlineColor[1]    = in.getFloat ("outlineColorG", g.outlineColor[1]);
    g.outlineColor[2]    = in.getFloat ("outlineColorB", g.outlineColor[2]);
    g.outlineColor[3]    = in.getFloat ("outlineColorA", g.outlineColor[3]);

    g.tonemap            = in.getInt   ("tonemap", g.tonemap);
    g.colorGrading       = in.getBool  ("colorGrading", g.colorGrading);
    g.exposure           = in.getFloat ("exposure", g.exposure);
    g.hdrCalibration     = in.getFloat ("hdrCalibration", g.hdrCalibration);
    g.brightness         = in.getFloat ("brightness", g.brightness);
    g.contrast           = in.getFloat ("contrast", g.contrast);
    g.saturation         = in.getFloat ("saturation", g.saturation);

    g.msaa               = in.getInt   ("msaa", g.msaa);
    g.fsr                = in.getBool  ("fsr", g.fsr);
    g.fsrSharpness       = in.getFloat ("fsrSharpness", g.fsrSharpness);
    g.fsr2               = in.getBool  ("fsr2", g.fsr2);
    g.fsr2Preset         = in.getInt   ("fsr2Preset", g.fsr2Preset);
    g.fsr2Sharpness      = in.getFloat ("fsr2Sharpness", g.fsr2Sharpness);
    g.resolutionScale    = in.getFloat ("resolutionScale", g.resolutionScale);

    g.occlusionCulling   = in.getBool  ("occlusionCulling", g.occlusionCulling);
    g.sceneUpdate        = in.getBool  ("sceneUpdate", g.sceneUpdate);
    g.meshBlend          = in.getBool  ("meshBlend", g.meshBlend);
    g.visibilityCompute  = in.getBool  ("visibilityCompute", g.visibilityCompute);

    g.frameskip          = in.getBool  ("frameskip", g.frameskip);

    pending_ = g; // stage the loaded values; caller applies (or Apply commits them)
}

void GraphicsSettings::loadAndApply(const st::nbt::Tag& in, wi::RenderPath3D& path, wi::Application& app) {
    // Seed applied_ from the current engine so applyToEngine's change-detection
    // (MSAA / FSR2 / resolutionScale trigger a buffer realloc only on change) has a
    // correct baseline, then stage + push the saved values.
    readFromEngine(path, app);
    LoadFrom(in);
    applyToEngine(path, app); // commits pending_ -> engine, sets applied_ = pending_
    initialized_ = true;      // render() should not re-read from the engine and clobber this
}

void GraphicsSettings::drawEngineTab() {
    EngineGfx& g = pending_;

    if (ImGui::CollapsingHeader("Ambient Occlusion & GI", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* aoItems[] = { "Disabled", "SSAO", "HBAO", "MSAO", "RTAO" };
        ImGui::Combo("Ambient Occlusion", &g.ao, aoItems, IM_ARRAYSIZE(aoItems));
        if (g.ao != 0) {
            ImGui::SliderFloat("AO Range",  &g.aoRange, 0.1f, 10.0f);
            ImGui::SliderInt("AO Samples",  &g.aoSampleCount, 1, 64);
            ImGui::SliderFloat("AO Power",  &g.aoPower, 0.25f, 8.0f);
        }

        const char* q[] = { "Low", "Medium", "High" };
        ImGui::Checkbox("Screen Space Reflections (SSR)", &g.ssr);
        if (g.ssr) ImGui::Combo("SSR Quality", &g.ssrQuality, q, IM_ARRAYSIZE(q));
        ImGui::Checkbox("Screen Space GI (SSGI)", &g.ssgi);
        if (g.ssgi) ImGui::SliderFloat("SSGI Depth Rejection", &g.ssgiDepthRejection, 0.0f, 50.0f);

        ImGui::Checkbox("Raytraced Reflections", &g.rtReflections);
        if (g.rtReflections) {
            ImGui::Combo("RT Reflections Quality", &g.rtReflectionsQual, q, IM_ARRAYSIZE(q));
            ImGui::SliderFloat("RT Reflections Range", &g.rtReflectionsRange, 1.0f, 10000.0f, "%.0f");
            ImGui::SliderFloat("Reflection Roughness Cutoff", &g.reflectionRoughCut, 0.0f, 1.0f);
        }
        ImGui::Checkbox("Raytraced Diffuse GI", &g.rtDiffuse);
        if (g.rtDiffuse) {
            ImGui::Combo("RT Diffuse Quality", &g.rtDiffuseQual, q, IM_ARRAYSIZE(q));
            ImGui::SliderFloat("RT Diffuse Range", &g.rtDiffuseRange, 1.0f, 1000.0f, "%.0f");
        }
        ImGui::Checkbox("Planar Reflections", &g.reflections);
    }

    if (ImGui::CollapsingHeader("Shadows")) {
        ImGui::Checkbox("Enable Shadows", &g.shadows);
        const char* resItems[] = { "1024", "2048", "4096" };
        int resIdx = g.shadowRes >= 4096 ? 2 : (g.shadowRes >= 2048 ? 1 : 0);
        if (ImGui::Combo("Shadow Resolution", &resIdx, resItems, IM_ARRAYSIZE(resItems)))
            g.shadowRes = (resIdx == 2) ? 4096 : (resIdx == 1 ? 2048 : 1024);
        ImGui::SliderInt("Contact Shadow Samples", &g.sssSampleCount, 0, 64);
        ImGui::SliderFloat("Contact Shadow Range", &g.sssRange, 0.0f, 10.0f);
    }

    if (ImGui::CollapsingHeader("Post Processing")) {
        ImGui::Checkbox("FXAA", &g.fxaa);

        ImGui::Checkbox("Bloom", &g.bloom);
        if (g.bloom) ImGui::SliderFloat("Bloom Threshold", &g.bloomThreshold, 0.0f, 10.0f);

        ImGui::Checkbox("Motion Blur", &g.motionBlur);
        if (g.motionBlur) ImGui::SliderFloat("Motion Blur Strength", &g.motionBlurStrength, 0.0f, 1000.0f, "%.0f");

        ImGui::Checkbox("Depth of Field", &g.dof);
        if (g.dof) ImGui::SliderFloat("DoF Strength", &g.dofStrength, 0.0f, 100.0f);

        ImGui::Checkbox("Sharpen Filter", &g.sharpen);
        if (g.sharpen) ImGui::SliderFloat("Sharpen Amount", &g.sharpenAmount, 0.0f, 2.0f);
        ImGui::Checkbox("CRT Filter", &g.crtFilter);

        ImGui::Checkbox("Eye Adaption", &g.eyeAdaption);
        if (g.eyeAdaption) {
            ImGui::SliderFloat("Eye Adaption Key",  &g.eyeAdaptionKey, 0.0f, 1.0f);
            ImGui::SliderFloat("Eye Adaption Rate", &g.eyeAdaptionRate, 0.0f, 10.0f);
        }

        ImGui::Checkbox("Lens Flare", &g.lensFlare);

        ImGui::Checkbox("Light Shafts", &g.lightShafts);
        if (g.lightShafts) {
            ImGui::SliderFloat("Light Shafts Strength",  &g.lightShaftsStr, 0.0f, 2.0f);
            ImGui::SliderFloat("Light Shafts Fade Speed", &g.lightShaftsFade, 0.0f, 10.0f);
        }

        ImGui::Checkbox("Volumetric Lights", &g.volumeLights);

        ImGui::Checkbox("Chromatic Aberration", &g.chromaticAberr);
        if (g.chromaticAberr) ImGui::SliderFloat("Chromatic Aberration Amount", &g.chromaticAberrAmt, 0.0f, 10.0f);

        ImGui::Checkbox("Dither", &g.dither);

        ImGui::Checkbox("Outline", &g.outline);
        if (g.outline) {
            ImGui::SliderFloat("Outline Threshold", &g.outlineThreshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Outline Thickness", &g.outlineThickness, 0.0f, 4.0f);
            ImGui::ColorEdit4("Outline Color", g.outlineColor);
        }
    }

    if (ImGui::CollapsingHeader("Color / Tonemapping")) {
        const char* tmItems[] = { "Reinhard", "ACES" };
        ImGui::Combo("Tonemap", &g.tonemap, tmItems, IM_ARRAYSIZE(tmItems));
        ImGui::Checkbox("Color Grading", &g.colorGrading);
        ImGui::SliderFloat("Exposure",        &g.exposure,       0.0f, 4.0f);
        ImGui::SliderFloat("HDR Calibration", &g.hdrCalibration, 0.0f, 4.0f);
        ImGui::SliderFloat("Brightness",      &g.brightness,     0.0f, 1.0f);
        ImGui::SliderFloat("Contrast",        &g.contrast,       0.0f, 4.0f);
        ImGui::SliderFloat("Saturation",      &g.saturation,     0.0f, 4.0f);
    }

    if (ImGui::CollapsingHeader("Display & Upscaling")) {
        const char* msaaItems[] = { "Off", "2x", "4x", "8x" };
        int msaaIdx = g.msaa >= 8 ? 3 : (g.msaa >= 4 ? 2 : (g.msaa >= 2 ? 1 : 0));
        if (ImGui::Combo("MSAA", &msaaIdx, msaaItems, IM_ARRAYSIZE(msaaItems)))
            g.msaa = (msaaIdx == 3) ? 8 : (msaaIdx == 2 ? 4 : (msaaIdx == 1 ? 2 : 1));

        ImGui::Checkbox("FSR 1.0 (spatial)", &g.fsr);
        if (g.fsr) ImGui::SliderFloat("FSR Sharpness", &g.fsrSharpness, 0.0f, 2.0f);

        ImGui::Checkbox("FSR 2.0 (temporal)", &g.fsr2);
        if (g.fsr2) {
            const char* presets[] = { "Quality", "Balanced", "Performance", "Ultra Performance" };
            ImGui::Combo("FSR2 Preset", &g.fsr2Preset, presets, IM_ARRAYSIZE(presets));
            ImGui::SliderFloat("FSR2 Sharpness", &g.fsr2Sharpness, 0.0f, 1.0f);
        }

        ImGui::BeginDisabled(g.fsr2); // FSR2 preset controls the scale
        ImGui::SliderFloat("Resolution Scale", &g.resolutionScale, 0.25f, 2.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::TextDisabled("(MSAA / FSR2 / resolution scale reallocate buffers on Apply)");
    }

    if (ImGui::CollapsingHeader("Performance / Culling")) {
        ImGui::Checkbox("Occlusion Culling", &g.occlusionCulling);
        // No "Scene Update" checkbox: st::App::Initialize turns the path's scene update
        // off on purpose (scenes own it) and re-enabling it double-updates the scene.
        ImGui::Checkbox("Mesh Blend", &g.meshBlend);
        ImGui::Checkbox("Visibility Shading in Compute", &g.visibilityCompute);
    }

    if (ImGui::CollapsingHeader("Frame Pacing", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Frame Skip (fixed-step catch-up)", &g.frameskip);
        ImGui::TextDisabled("V-Sync and the frame cap are on the Display tab.");
    }
}

void GraphicsSettings::drawDisplayTab(st::DisplaySettings& display, wi::Application& app) {
    // Owned by st::DisplaySettings, not by the Engine snapshot: it has its own
    // Apply/Revert because changing a resolution has to be confirmed, and a game
    // renders this very panel in its own options menu.
    ImGui::TextDisabled("Video options. A game can render this same panel from its own menu.");
    ImGui::Spacing();
    display.GUI(app);
}

void GraphicsSettings::drawContentTab(st::LensFlare& lensFlare, st::ProjectorSystem& projectors) {
    // Applies live — Apply/Reset at the bottom of the window only govern the Engine
    // tab's snapshot, so say so rather than let the buttons imply otherwise.
    ImGui::TextDisabled("Applies immediately (Apply/Reset only affect the Engine tab).");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Procedural Lens Flare", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Distinct from the Engine tab's "Lens Flare", which toggles the engine's own
        // texture-billboard flare driven by LightComponent::lensFlareNames.
        ImGui::TextDisabled("The framework's own screen-space flare (assets/shaders/StLensFlare*).");
        ImGui::Spacing();
        lensFlare.GUI();
    }

    if (ImGui::CollapsingHeader("Projectors", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Square/rectangular image projection (assets/shaders/StProjectorCS.hlsl).
        // The list itself is scene content - this only edits what is registered.
        ImGui::TextDisabled("Framework projectors: square light projection with projector optics.");
        ImGui::Spacing();
        projectors.GUI();
    }
}

void GraphicsSettings::render(bool* isopen, wi::RenderPath3D& path, wi::Application& app,
                              st::LensFlare& lensFlare, st::DisplaySettings& display,
                              st::ProjectorSystem& projectors) {
    if (!initialized_) {
        readFromEngine(path, app);
        initialized_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2(440, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Settings", isopen)) {
        ImGui::End();
        return;
    }

    // Tabs fill the window minus the button row at the bottom.
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##gfx_tabs", ImVec2(0, -footer), false);
    if (ImGui::BeginTabBar("##gfx_tabbar")) {
        if (ImGui::BeginTabItem("Display")) { drawDisplayTab(display, app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Engine"))  { drawEngineTab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Content")) { drawContentTab(lensFlare, projectors); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::Separator();

    const bool dirty = (pending_ != applied_);
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Reset")) {
        pending_ = applied_; // discard unapplied edits
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        applyToEngine(path, app); // commits pending_ and sets applied_ = pending_
        // Persist to options.stad so the choice survives a restart.
        SaveTo(st::SettingsManager::Get().GraphicsTag());
        st::SettingsManager::Get().Save();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Checkbox("Auto Apply", &autoApply_);

    // Auto Apply: push edits to the engine as soon as they happen. applyToEngine
    // only triggers a buffer realloc when a size/sample setting actually changed,
    // so a static frame costs nothing.
    if (autoApply_ && dirty) {
        applyToEngine(path, app);
    } else if (dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "unapplied changes");
    }

    ImGui::End();
}
