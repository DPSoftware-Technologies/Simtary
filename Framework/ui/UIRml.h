#pragma once
// UIRml - RmlUi (libs/RmlUi) behind UIDocumentComponent.
//
// One Rml::Context per document entity, sized to the element's rect in pixels, drawn
// inside that rect through render::Draw2D (compiled geometry; the Detroit backend's shaders
// are assets/shaders/StUi{VS,PS}.hlsl). The canvas places the rect; everything inside it is
// RmlUi's: layout, RCSS styling, transitions, animations, data.
//
// What RmlUi sees of the engine:
//   files    through wi::helper::FileRead, so .rml/.rcss/images resolve out of a mounted
//            asset package by the same logical path ("ui/hud.rml") an editor field takes;
//   textures through wi::resourcemanager (any format the engine loads);
//   fonts    the engine's embedded Liberation Sans as the fallback face (family
//            "LiberationSans"), plus whatever a document lists in UIDocument::fonts;
//   log      the backlog, tagged [RmlUi];
//   time     process time.
//
// Sizes: the context's density-independent pixel ratio is the canvas scale, so `dp` units
// follow the canvas the way everything else on it does. Plain `px` are screen pixels.
//
// Events: an `on<event>="name"` attribute in RML (onclick="startGame") is reported to the
// callback set with SetEventCallback - UISystem routes it to the document's eventTarget
// scripts as `func name(GameObject sender, str element)`.
//
// Main thread only. Created on the first document; nothing is initialised for a game that
// never uses one. No exceptions, no RTTI.

#include "wiECS.h"
#include "render/api/Renderer.h"
#include "wiMath.h"

#include <functional>
#include <string>
#include <unordered_set>

union SDL_Event;

namespace st::ui {

struct UIDocumentComponent;

class RmlHost {
public:
    static RmlHost& Get ();

    // Lazily initialises RmlUi. False if it could not (shaders missing - logged once).
    bool Ready ();
    void Shutdown ();

    // Keep one context for `e`: (re)load the source when it changed, follow the rect size.
    void Sync (wi::ecs::Entity e, const UIDocumentComponent& doc, XMINT2 sizePixels, float dpRatio);
    // Drop the contexts of entities no longer in `alive`.
    void Retain (const std::unordered_set<wi::ecs::Entity>& alive);
    // Reload every document from its files (hot reload after editing .rml/.rcss).
    void ReloadAll ();
    bool Has (wi::ecs::Entity e) const;
    // "loaded", "no source", "failed: ..." - for the inspector.
    std::string Status (wi::ecs::Entity e) const;

    // Per frame, after input: RmlUi layout, animation, data bindings.
    void Update ();
    // Draw the document of `e` with its top-left at `offset` (target pixels), cut to
    //	`clip` (x0, y0, x1, y1). Call between draw.Begin() and draw.End().
    void Render (wi::ecs::Entity e, render::Draw2D& draw, XMFLOAT2 offset, XMFLOAT4 clip);

    // Input, in the document's own pixels. Each returns true when RmlUi used it.
    bool PointerMove (wi::ecs::Entity e, XMFLOAT2 local);
    bool PointerButton (wi::ecs::Entity e, bool down);
    bool PointerWheel (wi::ecs::Entity e, float delta);
    void PointerLeave (wi::ecs::Entity e);
    // True while some document has a focused text field (keys go there, not to the game).
    bool WantsKeyboard () const;
    bool HandleSdlEvent (const SDL_Event& event);
    void ClearFocus ();

    // on<event>="handler" fired in the document of `e`.
    using EventCallback = std::function<void(wi::ecs::Entity document, const std::string& handler,
                                             const std::string& element, const std::string& type)>;
    void SetEventCallback (EventCallback callback) { onEvent_ = std::move(callback); }
    // Called from the RmlUi event listener.
    void FireEvent (void* document, const std::string& handler, const std::string& element,
                    const std::string& type);

    // Element access for scripts and game code. False when the document or element is missing.
    bool SetInnerRml (wi::ecs::Entity e, const std::string& id, const std::string& rml);
    bool SetClass (wi::ecs::Entity e, const std::string& id, const std::string& cls, bool on);
    bool SetProperty (wi::ecs::Entity e, const std::string& id, const std::string& name,
                      const std::string& value);
    bool SetAttribute (wi::ecs::Entity e, const std::string& id, const std::string& name,
                       const std::string& value);
    std::string GetValue (wi::ecs::Entity e, const std::string& id) const;

    struct Impl;   // RmlUi state; opaque outside UIRml.cpp

private:
    RmlHost () = default;
    Impl* impl_ = nullptr;
    bool  failed_ = false;
    EventCallback onEvent_;
};

} // namespace st::ui
