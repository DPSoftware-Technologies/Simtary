#pragma once
// UISystem - lays out, runs and draws the UI components (ui/UIComponents.h) of the scene.
//
// Per frame, from st::App:
//   SetPointer()   where the mouse is on the game image (pixels), from the window or from the
//                  editor's Game Viewport panel; invalid while the game holds the cursor
//   Update()       after the scene update and the scripts, so a value a script set this frame
//                  is the one laid out: anchors -> rects, layout groups, slider/toggle visuals,
//                  then input (hover, press, click, drag) and the events it fires
//   Render()       after the 3D render path: Screen canvases are drawn straight onto the game's
//                  final image (so the HUD is in the Game Viewport, the fullscreen frame and
//                  every screenshot alike), each World canvas into its own texture, which is
//                  bound as the base colour of its plane's material
//
// Input runs only in Play mode (`interactive`); in Edit mode the UI is drawn, frozen, and
// the editor uses Pick() to select elements in the Game Viewport.
//
// Events go two ways:
//   - to every stScript on the element's eventTarget (the element itself when unset), as a
//     function named by the component: Button onClick -> func name(GameObject sender),
//     Toggle -> (sender, bool on), Slider -> (sender, float value),
//     document onclick="name" -> (sender, str element);
//   - to C++ listeners (AddListener) as a UIEvent, and WasClicked() for polling;
//   - to code subscriptions (Subscribe, Taipan UI.On): "click", "value", and the pointer
//     events "enter", "exit", "down", "up" that have no inspector field. A subscription
//     names the target whose stScripts get the call, func handler(GameObject sender[, value]).
//     Enter/exit go to every element the pointer moves into/out of (the hovered element
//     and its ancestors), down to the first element from the hovered one up that subscribes
//     to it, and up to whatever got the down, wherever the pointer is by then.
//
// Main thread only. No exceptions, no RTTI.

#include "ui/UIComponents.h"
#include "wiGraphicsDevice.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

union SDL_Event;

namespace st::ui {

struct Pointer {
    bool     valid = false;          // over the game image and free (not captured by the game)
    XMFLOAT2 pos = XMFLOAT2(0, 0);   // game image pixels, top left = 0,0
    bool     down = false;           // primary button
    float    wheel = 0;
};

struct UIEvent {
    enum class Kind { Click, Toggle, Slider, Document, Enter, Exit, Down, Up };
    Kind            kind = Kind::Click;
    wi::ecs::Entity sender = wi::ecs::INVALID_ENTITY;
    std::string     handler;   // the function name the component names (may be empty)
    std::string     value;     // "true"/"false", the slider value, or the document element id
};

class UISystem {
public:
    static UISystem& Get ();

    void SetPointer (const Pointer& pointer) { pointer_ = pointer; }
    void Update (wi::scene::Scene& scene, float dt, XMUINT2 targetSize, bool interactive);
    // `target` is the game's final image; its resting layout is kept.
    void Render (wi::scene::Scene& scene, const wi::graphics::Texture* target, wi::graphics::CommandList cmd);
    void Shutdown ();

    // The pointer is over an element that takes it - the game should ignore this click.
    bool PointerOverUI () const { return hovered_ != wi::ecs::INVALID_ENTITY; }
    // A document text field has focus - keys belong to it.
    bool WantsKeyboard () const;
    // Keyboard into a focused document. True when consumed.
    bool HandleSdlEvent (const SDL_Event& event);

    bool WasClicked (wi::ecs::Entity e) const;
    // The pointer is over `e` or one of its children / the primary button went down on it.
    bool IsHovered (wi::ecs::Entity e) const;
    bool IsPressed (wi::ecs::Entity e) const;

    // `event` (click, value, enter, exit, down, up) on `element` calls func `handler` on the
    //	stScripts of `target`. False for an unknown event. Subscribing twice is one subscription.
    bool Subscribe (wi::ecs::Entity element, const std::string& event, wi::ecs::Entity target,
                    const std::string& handler);
    // Empty `event` / `handler` match any.
    int  Unsubscribe (wi::ecs::Entity element, const std::string& event, wi::ecs::Entity target,
                      const std::string& handler);
    // Everything `target` subscribed (its script is going away).
    void UnsubscribeTarget (wi::ecs::Entity target);
    int  AddListener (std::function<void(const UIEvent&)> listener);
    void RemoveListener (int handle);

    // Fire an element as if clicked (tests, the editor bridge, gamepad "submit").
    bool Click (wi::scene::Scene& scene, wi::ecs::Entity e);

    // Topmost visible element under `pixel` on a Screen canvas, pointer target or not (the
    //	editor selects with it). INVALID_ENTITY when none.
    wi::ecs::Entity Pick (wi::scene::Scene& scene, XMFLOAT2 pixel) const;
    XMUINT2 TargetSize () const { return target_; }
    uint64_t Frame () const { return frame_; }
    // Visible elements this frame, in draw order.
    const std::vector<wi::ecs::Entity>& DrawOrder () const { return order_; }

private:
    UISystem () = default;

    struct Ctx;
    void Gather (wi::scene::Scene& scene);
    void ApplyWidgets (wi::scene::Scene& scene);
    void LayoutCanvas (wi::scene::Scene& scene, wi::ecs::Entity canvas);
    void LayoutNode (Ctx& ctx, wi::ecs::Entity e, const XMFLOAT4& parentRect, XMFLOAT2 k,
                     float opacity, const XMFLOAT4& clip, bool root);
    void Arrange (Ctx& ctx, wi::ecs::Entity e, XMFLOAT2 k);
    void Resources (wi::scene::Scene& scene);
    void Interact (wi::scene::Scene& scene, float dt, bool interactive);
    void Visuals (wi::scene::Scene& scene, float dt, bool interactive);
    void Documents_ (wi::scene::Scene& scene, bool interactive);
    bool IsTarget (wi::scene::Scene& scene, wi::ecs::Entity e) const;
    wi::ecs::Entity Selectable (wi::scene::Scene& scene, wi::ecs::Entity e) const;
    void SetSlider (wi::scene::Scene& scene, wi::ecs::Entity e, XMFLOAT2 pixel);
    void Dispatch (wi::scene::Scene& scene, UIEvent::Kind kind, wi::ecs::Entity sender,
                   const std::string& handler, wi::ecs::Entity target, const std::string& value,
                   const void* args, int argc);
    struct Sub {
        wi::ecs::Entity element;
        UIEvent::Kind   kind;
        wi::ecs::Entity target;
        std::string     handler;
    };
    bool HasSub (wi::ecs::Entity element, UIEvent::Kind kind) const;
    // Listeners + subscriptions for an event that has no inspector field (and the extra
    //	subscription half of the ones that do).
    void Notify (wi::scene::Scene& scene, UIEvent::Kind kind, wi::ecs::Entity sender,
                 const std::string& value, const void* args, int argc, bool toListeners);
    void PointerEvents (wi::scene::Scene& scene, bool pressedEdge, bool releasedEdge);
    void DrawElement (wi::scene::Scene& scene, wi::ecs::Entity e, wi::graphics::CommandList cmd,
                      XMUINT2 targetSize);
    void RenderWorldCanvases (wi::scene::Scene& scene, wi::graphics::CommandList cmd);

    Pointer  pointer_;
    bool     prevDown_ = false;
    XMUINT2  target_ = XMUINT2(0, 0);
    uint64_t frame_ = 0;
    bool     interactive_ = false;

    std::unordered_map<wi::ecs::Entity, std::vector<wi::ecs::Entity>> children_;
    std::vector<wi::ecs::Entity> roots_;           // canvases, by sortOrder
    std::vector<wi::ecs::Entity> order_;           // visible elements, draw order
    std::unordered_map<wi::ecs::Entity, bool> worldElement_;   // element -> lives on a World canvas

    wi::ecs::Entity hovered_ = wi::ecs::INVALID_ENTITY;      // pointer target under the pointer
    wi::ecs::Entity hoveredSel_ = wi::ecs::INVALID_ENTITY;   // its button/toggle/slider
    wi::ecs::Entity pressed_ = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity pressedDoc_ = wi::ecs::INVALID_ENTITY;
    std::vector<wi::ecs::Entity> hoverChain_;                // hovered_ and its ancestors, deepest first
    wi::ecs::Entity downElement_ = wi::ecs::INVALID_ENTITY;  // got "down", gets "up"
    std::vector<Sub> subs_;

    std::unordered_map<wi::ecs::Entity, wi::graphics::Texture> worldTargets_;
    std::vector<std::pair<int, std::function<void(const UIEvent&)>>> listeners_;
    int nextListener_ = 1;
    std::unordered_set<std::string> warned_;
    wi::scene::Scene* scene_ = nullptr;   // the scene document events are routed into
};

} // namespace st::ui
