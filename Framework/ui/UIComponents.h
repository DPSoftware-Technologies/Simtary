#pragma once
// UI components - the Unity-uGUI-shaped half of Simtary's game UI.
//
// A UI is a tree of entities in the scene, edited in the Hierarchy and the Properties panel
// like everything else, and laid out on a CANVAS:
//
//     Canvas            UICanvasComponent + UIRectComponent (the rect IS the screen)
//     └── Panel         UIRectComponent + UIImageComponent
//         ├── Title     UIRectComponent + UITextComponent
//         ├── Play      UIRectComponent + UIImageComponent + UIButtonComponent
//         │   └── Text  UIRectComponent + UITextComponent
//         └── Page      UIRectComponent + UIDocumentComponent   (an RmlUi .rml page)
//
// Parenting is the scene hierarchy (HierarchyComponent). Every UI entity also carries a
// TransformComponent - unused by screen layout, but it is what the Hierarchy panel, re-parent
// and templates expect of an object.
//
// These are REAL component managers, registered into every Scene through
// wi::scene::AddSceneComponentRegistrar (RegisterUIComponents below), so save/load, undo,
// templates, scene merge and the Play-mode snapshot all handle them like an engine component.
// Fields with a leading underscore are runtime state: computed every frame, never saved.
//
// Coordinates are y-DOWN, in canvas units, (0,0) at the top left - the screen's own
// convention, unlike Unity's y-up. Anchors and pivot are fractions of the parent rect:
// (0,0) top left, (1,1) bottom right. The rect of an element is
//
//     width  = (anchorMax.x - anchorMin.x) * parent.width + size.x
//     pivotX = parent.x + lerp(anchorMin.x, anchorMax.x, pivot.x) * parent.width + position.x
//     x      = pivotX - pivot.x * width
//
// which is Unity's RectTransform: equal anchors = fixed size at an anchor point, split
// anchors = stretch with `size` as the extra (negative = inset).
//
// No exceptions, no RTTI.

#include "wiECS.h"
#include "wiArchive.h"
#include "wiResourceManager.h"
#include "wiMath.h"

#include <cstdint>
#include <string>

namespace wi::scene { struct Scene; }

namespace st::ui {

struct UICanvasComponent {
    enum Flags : uint32_t {
        EMPTY  = 0,
        HIDDEN = 1 << 0,
    };
    enum class RenderMode : uint32_t {
        Screen,   // drawn over the game view
        World,    // rendered into a texture shown on this entity's object (a plane)
    };
    enum class ScaleMode : uint32_t {
        ConstantPixel,     // one canvas unit = scaleFactor pixels
        ScaleWithScreen,   // the canvas is referenceResolution, scaled to fit the screen
    };
    uint32_t   _flags = EMPTY;
    RenderMode renderMode = RenderMode::Screen;
    ScaleMode  scaleMode = ScaleMode::ScaleWithScreen;
    XMFLOAT2   referenceResolution = XMFLOAT2(1920, 1080);
    float      match = 0.5f;          // ScaleWithScreen: 0 = fit width, 1 = fit height
    float      scaleFactor = 1.0f;    // ConstantPixel
    int        sortOrder = 0;         // higher draws on top

    // runtime
    float    _scale = 1.0f;             // pixels per canvas unit this frame
    XMFLOAT2 _size = XMFLOAT2(0, 0);    // canvas size in canvas units

    bool IsHidden () const { return _flags & HIDDEN; }
    void SetHidden (bool v) { if (v) _flags |= HIDDEN; else _flags &= ~HIDDEN; }

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

struct UIRectComponent {
    enum Flags : uint32_t {
        EMPTY         = 0,
        HIDDEN        = 1 << 0,   // this element and everything under it
        CLIP_CHILDREN = 1 << 1,   // children are cut to this rect (a mask / scroll area)
        IGNORE_LAYOUT = 1 << 2,   // a parent UILayout leaves this one where its anchors put it
        BLOCK_POINTER = 1 << 3,   // hit-testable even with no image/text: swallows clicks
    };
    uint32_t _flags = EMPTY;
    XMFLOAT2 anchorMin = XMFLOAT2(0.5f, 0.5f);
    XMFLOAT2 anchorMax = XMFLOAT2(0.5f, 0.5f);
    XMFLOAT2 pivot     = XMFLOAT2(0.5f, 0.5f);
    XMFLOAT2 position  = XMFLOAT2(0, 0);       // pivot offset from its anchor point
    XMFLOAT2 size      = XMFLOAT2(160, 40);    // fixed size, or the extra over the anchors
    float    rotation  = 0;                    // radians, around the pivot; this element's own drawing
    XMFLOAT2 scale     = XMFLOAT2(1, 1);       // around the pivot; applies to children too
    int      order     = 0;                    // among siblings: higher draws later (on top)
    float    opacity   = 1.0f;                 // multiplies down the tree (Unity's CanvasGroup alpha)

    // runtime, written by the layout pass
    XMFLOAT4        _rect = XMFLOAT4(0, 0, 0, 0);       // x, y, w, h in canvas units
    XMFLOAT4        _pixels = XMFLOAT4(0, 0, 0, 0);     // the same in target pixels
    XMFLOAT4        _clip = XMFLOAT4(0, 0, 0, 0);       // pixel scissor in force (x0, y0, x1, y1)
    float           _opacity = 1.0f;
    float           _scale = 1.0f;                      // pixels per unit incl. inherited rect scale
    wi::ecs::Entity _canvas = wi::ecs::INVALID_ENTITY;
    bool            _visible = false;
    bool            _laidOut = false;                   // placed by a parent UILayout this frame

    bool IsHidden () const { return _flags & HIDDEN; }
    void SetHidden (bool v) { if (v) _flags |= HIDDEN; else _flags &= ~HIDDEN; }

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

struct UIImageComponent {
    enum Flags : uint32_t {
        EMPTY           = 0,
        PRESERVE_ASPECT = 1 << 0,
        RAYCAST_TARGET  = 1 << 1,   // takes the pointer (blocks what is under it)
        FILL_REVERSE    = 1 << 2,   // Filled types grow from the other end
    };
    enum class Type : uint32_t {
        Simple,            // the texture stretched over the rect
        Sliced,            // 9-slice: `border` (texture pixels) keeps the corners unscaled
        FilledHorizontal,  // health bar: fillAmount of the width
        FilledVertical,
    };
    enum class Gradient : uint32_t { None, Horizontal, Vertical };

    uint32_t    _flags = RAYCAST_TARGET;
    std::string textureName;                     // "" = a solid colour
    XMFLOAT4    color = XMFLOAT4(1, 1, 1, 1);
    Type        type = Type::Simple;
    XMFLOAT4    border = XMFLOAT4(0, 0, 0, 0);    // Sliced: left, top, right, bottom in texture pixels
    float       borderScale = 1.0f;               // Sliced: corner size on screen per texture pixel
    float       fillAmount = 1.0f;
    float       cornerRadius = 0;                 // rounded corners, canvas units
    Gradient    gradient = Gradient::None;
    XMFLOAT4    gradientColor = XMFLOAT4(0, 0, 0, 1);

    // runtime
    wi::Resource _texture;
    std::string  _loaded;                         // textureName _texture was loaded for
    XMFLOAT4     _tint = XMFLOAT4(1, 1, 1, 1);    // set by a UIButton driving this graphic

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

struct UITextComponent {
    enum Flags : uint32_t {
        EMPTY          = 0,
        WRAP           = 1 << 0,   // wrap at the rect width
        SHADOW         = 1 << 1,
        RAYCAST_TARGET = 1 << 2,
    };
    enum class Align : uint32_t { Start, Center, End };   // left/top, centre, right/bottom

    uint32_t    _flags = WRAP;
    std::string text = "Text";
    std::string fontName;                         // .ttf / .otf asset; "" = the engine font
    float       size = 32;                        // line height, canvas units
    XMFLOAT4    color = XMFLOAT4(1, 1, 1, 1);
    Align       hAlign = Align::Center;
    Align       vAlign = Align::Center;
    XMFLOAT4    shadowColor = XMFLOAT4(0, 0, 0, 0.6f);
    XMFLOAT2    shadowOffset = XMFLOAT2(2, 2);    // canvas units
    float       bolden = 0;                       // 0..1, also an outline when SHADOW is on
    float       softness = 0;                     // 0..1
    XMFLOAT2    spacing = XMFLOAT2(0, 0);         // extra character / line spacing, canvas units

    // runtime
    int         _style = 0;                       // wi::font style id
    std::string _loadedFont;
    bool        _fontLoaded = false;
    XMFLOAT4    _tint = XMFLOAT4(1, 1, 1, 1);

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

// Selectable state and colour tint, plus a click. A UIToggle or UISlider on the same entity
// uses it for its look; on its own it is a Button.
struct UIButtonComponent {
    enum Flags : uint32_t {
        EMPTY        = 0,
        INTERACTABLE = 1 << 0,
    };
    enum class State : uint32_t { Normal, Hover, Pressed, Disabled };

    uint32_t        _flags = INTERACTABLE;
    wi::ecs::Entity targetGraphic = wi::ecs::INVALID_ENTITY;   // tinted image/text; none = this entity
    XMFLOAT4        normalColor   = XMFLOAT4(1, 1, 1, 1);
    XMFLOAT4        hoverColor    = XMFLOAT4(0.92f, 0.92f, 0.92f, 1);
    XMFLOAT4        pressedColor  = XMFLOAT4(0.75f, 0.75f, 0.75f, 1);
    XMFLOAT4        disabledColor = XMFLOAT4(0.6f, 0.6f, 0.6f, 0.5f);
    float           fadeDuration  = 0.08f;
    // Called on click: a function of that name in every stScript on eventTarget (this
    //	entity when unset), as `func <name>(GameObject sender)`.
    std::string     onClick;
    wi::ecs::Entity eventTarget = wi::ecs::INVALID_ENTITY;

    // runtime
    State    _state = State::Normal;
    XMFLOAT4 _color = XMFLOAT4(1, 1, 1, 1);
    uint64_t _clickFrame = 0;                     // UISystem frame of the last click

    bool IsInteractable () const { return _flags & INTERACTABLE; }
    void SetInteractable (bool v) { if (v) _flags |= INTERACTABLE; else _flags &= ~INTERACTABLE; }

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

struct UIToggleComponent {
    enum Flags : uint32_t {
        EMPTY        = 0,
        IS_ON        = 1 << 0,
        INTERACTABLE = 1 << 1,
    };
    uint32_t        _flags = IS_ON | INTERACTABLE;
    wi::ecs::Entity graphic = wi::ecs::INVALID_ENTITY;   // shown while on (the checkmark)
    // `func <name>(GameObject sender, bool on)` on eventTarget's stScripts (this entity when unset).
    std::string     onValueChanged;
    wi::ecs::Entity eventTarget = wi::ecs::INVALID_ENTITY;

    bool IsOn () const { return _flags & IS_ON; }
    void SetOn (bool v) { if (v) _flags |= IS_ON; else _flags &= ~IS_ON; }

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

struct UISliderComponent {
    enum Flags : uint32_t {
        EMPTY         = 0,
        INTERACTABLE  = 1 << 0,
        WHOLE_NUMBERS = 1 << 1,
    };
    enum class Direction : uint32_t { LeftToRight, RightToLeft, BottomToTop, TopToBottom };

    uint32_t        _flags = INTERACTABLE;
    Direction       direction = Direction::LeftToRight;
    float           minValue = 0;
    float           maxValue = 1;
    float           value = 0.5f;
    wi::ecs::Entity fillRect = wi::ecs::INVALID_ENTITY;     // its anchors are set to the value
    wi::ecs::Entity handleRect = wi::ecs::INVALID_ENTITY;   // its anchor point follows the value
    // `func <name>(GameObject sender, float value)` on eventTarget's stScripts.
    std::string     onValueChanged;
    wi::ecs::Entity eventTarget = wi::ecs::INVALID_ENTITY;

    // runtime
    bool _dragging = false;

    float Normalized () const {
        const float range = maxValue - minValue;
        return range != 0 ? wi::math::saturate((value - minValue) / range) : 0.0f;
    }

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

// Lays the direct children out in a row, a column or a grid, instead of their anchors.
struct UILayoutComponent {
    enum Flags : uint32_t {
        EMPTY         = 0,
        EXPAND_WIDTH  = 1 << 0,   // children fill the cross axis / share the main axis
        EXPAND_HEIGHT = 1 << 1,
        FIT_WIDTH     = 1 << 2,   // this rect grows to its content
        FIT_HEIGHT    = 1 << 3,
        REVERSE       = 1 << 4,
    };
    enum class Type : uint32_t { Horizontal, Vertical, Grid };

    uint32_t _flags = EMPTY;
    Type     type = Type::Vertical;
    XMFLOAT4 padding = XMFLOAT4(0, 0, 0, 0);   // left, top, right, bottom
    XMFLOAT2 spacing = XMFLOAT2(8, 8);
    uint32_t childAlignment = 0;               // 0..8: upper-left, upper-centre ... lower-right
    XMFLOAT2 cellSize = XMFLOAT2(100, 100);    // Grid
    int      columns = 0;                      // Grid: 0 = as many as fit

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

// An RmlUi document (.rml + .rcss) drawn inside this element's rect. The page's own layout,
// styling and animation are RmlUi's; the rect is placed by the canvas like any element.
struct UIDocumentComponent {
    enum Flags : uint32_t {
        EMPTY       = 0,
        INTERACTIVE = 1 << 0,   // gets the pointer and the keyboard
    };
    uint32_t    _flags = INTERACTIVE;
    std::string source;      // "ui/hud.rml"
    std::string fonts;       // extra font files, ';' separated ("fonts/a.ttf;fonts/b.otf")
    // A document event `onclick="name"` (or any on<event>="name") calls
    //	`func <name>(GameObject sender, str element)` on eventTarget's stScripts.
    wi::ecs::Entity eventTarget = wi::ecs::INVALID_ENTITY;

    void Serialize (wi::Archive& archive, wi::ecs::EntitySerializer& seri);
};

// ------------------------------------------------------------------------- managers ---

constexpr const char* kCanvases  = "st::ui::canvases";
constexpr const char* kRects     = "st::ui::rects";
constexpr const char* kImages    = "st::ui::images";
constexpr const char* kTexts     = "st::ui::texts";
constexpr const char* kButtons   = "st::ui::buttons";
constexpr const char* kToggles   = "st::ui::toggles";
constexpr const char* kSliders   = "st::ui::sliders";
constexpr const char* kLayouts   = "st::ui::layouts";
constexpr const char* kDocuments = "st::ui::documents";

// Registers the managers on `scene` if it has none yet. Installed for every Scene by
//	InstallUIComponents(); called directly only for a scene built before that.
void RegisterUIComponents (wi::scene::Scene& scene);
// Once, at startup (st::Run): every Scene from now on - and the global one - gets them.
void InstallUIComponents ();

// The managers of `scene`. Never null after RegisterUIComponents.
wi::ecs::ComponentManager<UICanvasComponent>&   Canvases (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UIRectComponent>&     Rects (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UIImageComponent>&    Images (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UITextComponent>&     Texts (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UIButtonComponent>&   Buttons (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UIToggleComponent>&   Toggles (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UISliderComponent>&   Sliders (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UILayoutComponent>&   Layouts (wi::scene::Scene& scene);
wi::ecs::ComponentManager<UIDocumentComponent>& Documents (wi::scene::Scene& scene);

// ------------------------------------------------------------------------ creation ---
// What the editor's Create > UI menu, the bridge and scripts use. Each makes a named entity
// with a transform and a UIRect (plus its element), parented under `parent` when given;
// with no parent a Screen canvas is found or made first, so an element is never orphaned.

enum class Preset : uint32_t {
    Canvas, WorldCanvas, Panel, Image, Text, Button, Toggle, Slider, Document,
    VerticalLayout, HorizontalLayout, Grid,
};
const char* PresetName (Preset preset);
// The canvas `e` belongs to (itself when it is one); INVALID_ENTITY when none.
wi::ecs::Entity FindCanvas (wi::scene::Scene& scene, wi::ecs::Entity e);
// The first Screen canvas of the scene, made when there is none and `create` is set.
wi::ecs::Entity FindOrCreateScreenCanvas (wi::scene::Scene& scene, bool create);
// Creates the preset. Returns its root entity; `created` (optional) gets every entity made.
wi::ecs::Entity Create (wi::scene::Scene& scene, Preset preset, wi::ecs::Entity parent,
                        const std::string& name = "", wi::vector<wi::ecs::Entity>* created = nullptr);

} // namespace st::ui
