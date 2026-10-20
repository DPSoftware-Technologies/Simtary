#pragma once
// UI Editor - the editor panel game UI is laid out in, beside the Game Viewport.
//
// The Game Viewport stays play-only: its clicks belong to the game. This panel shows the
// same game image (the UI is drawn onto it, so what is here is exactly what plays) and owns
// the mouse itself:
//
//   click            select the widget under the cursor - the Button, not the Text in it
//                    (the nearest Button/Toggle/Slider/Document above the hit element;
//                    the element itself when there is none; nothing = deselect)
//   double-click     go one level into the selection (Button -> its Text)
//   ctrl+click       select the deepest element under the cursor at once
//   drag             move it (position); a child placed by a UILayout cannot be dragged
//   drag a handle    resize from that edge/corner, the opposite side stays put
//   wheel            zoom around the cursor;  middle drag / space + drag  pan
//   arrows           nudge 1 unit (shift: 10)
//   drop an image    a UI Image with that texture, on the element under the drop (or
//                    the canvas); drop an .rml file for a UI Document
//
// Shown for the selection: its rect, the eight handles, its pivot, and its anchors as
// four ticks on the parent rect - Unity's Rect tool. Every drag is one undo step. Works in
// Edit and in Play mode (a Play-mode edit is undone at Stop, like everything else).
//
// Main thread, ImGui. No exceptions, no RTTI.

#include "wiECS.h"
#include "wiGraphicsDevice.h"
#include "imgui.h"

namespace wi::scene { struct Scene; }

namespace st {

class EditorHistory;

class UIEditorPanel {
public:
    // `frame` is the game's final image (UI included). `open` is the window close box.
    void Draw (wi::scene::Scene& scene, wi::ecs::Entity& selected, EditorHistory* history,
               const wi::graphics::Texture* frame, bool* open);

    static constexpr const char* kWindowName = "UI Editor";

private:
    enum class Drag { None, Move, Resize, Pan };

    ImVec2 ToScreen (XMFLOAT2 pixel) const;
    XMFLOAT2 ToPixel (ImVec2 screen) const;
    void DrawOverlay (wi::scene::Scene& scene, wi::ecs::Entity selected);
    int HandleAt (wi::scene::Scene& scene, wi::ecs::Entity selected, ImVec2 mouse) const;
    wi::ecs::Entity PickForEdit (wi::scene::Scene& scene, wi::ecs::Entity selected, XMFLOAT2 pixel,
                                 bool deepest, bool drill);
    void ApplyDrag (wi::scene::Scene& scene, wi::ecs::Entity selected, XMFLOAT2 deltaPixels);
    void AcceptDrop (wi::scene::Scene& scene, wi::ecs::Entity& selected, EditorHistory* history);

    float  zoom_ = 0.0f;              // 0 = fit
    ImVec2 pan_ = ImVec2(0, 0);       // offset of the image from centred, screen pixels
    ImVec2 imagePos_ = ImVec2(0, 0);  // this frame
    float  scale_ = 1.0f;             // screen pixels per game pixel this frame
    bool   showRects_ = true;
    bool   snap_ = false;
    float  snapStep_ = 8.0f;

    Drag     drag_ = Drag::None;
    int      handle_ = -1;            // 0..7: tl t tr r br b bl l
    ImVec2   dragStart_ = ImVec2(0, 0);
    XMFLOAT2 startPosition_ = XMFLOAT2(0, 0);
    XMFLOAT2 startSize_ = XMFLOAT2(0, 0);
    bool     dragMoved_ = false;
    wi::ecs::Entity drilled_ = wi::ecs::INVALID_ENTITY;   // a part selected by drilling into its widget
};

} // namespace st
