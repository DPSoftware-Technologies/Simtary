#include "wiSDLInput.h"

#ifdef SDL2
#include <SDL2/SDL.h>

#include "wiUnorderedMap.h"
#include "wiBacklog.h"

#include <algorithm>

namespace wi::input::sdlinput
{
    wi::input::KeyboardState keyboard;
    wi::input::MouseState mouse;

    struct Internal_ControllerState
    {
        Sint32 portID = -1;
        SDL_JoystickID internalID = -1;
        SDL_GameController* controller = nullptr;
        // `Uint16 rumble_l, rumble_r = 0;` only ever initialized rumble_r - the left
        //  motor started on whatever happened to be in the allocation.
        Uint16 rumble_l = 0;
        Uint16 rumble_r = 0;
        int xinput_user = -1; // SDL player index; on Windows this IS the XInput user index
        wi::input::ControllerState state;
    };
    wi::vector<Internal_ControllerState> controllers;
    wi::unordered_map<SDL_JoystickID, size_t> controller_mapped;

    wi::vector<SDL_Event> events;

    // SDL axes are Sint16: the negative end reaches -32768, the positive end only
    //  32767. Dividing both by 32767 overshoots -1 on the way left/down.
    inline float normalize_axis(Sint16 v) {
        const float f = (v < 0) ? ((float)v / 32768.0f) : ((float)v / 32767.0f);
        return std::max(-1.0f, std::min(1.0f, f));
    }

    int to_wicked(const SDL_Scancode &scan, const SDL_Keycode &sym);
    void controller_to_wicked(uint32_t *current, Uint8 button, bool pressed);
    void controller_map_rebuild();

    void AddController(Sint32 id);
    void RemoveController(Sint32 id);
    void RemoveController();

    // Open a game controller by its SDL joystick index and register it. Safe to
    // call for an already-open device: the instance-id is looked up in
    // controller_mapped and a duplicate open is closed and skipped. This is the
    // piece the engine was missing — SDL only emits SDL_CONTROLLERDEVICEADDED on
    // hot-plug, never for pads already connected at process start, so a pad left
    // plugged across an app restart was never opened and read back as 0.
    void AddController(Sint32 joystick_index) {
        if (!SDL_IsGameController(joystick_index))
            return;
        SDL_GameController* gc = SDL_GameControllerOpen(joystick_index);
        if (gc == nullptr)
            return;
        SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
        SDL_JoystickID iid = SDL_JoystickInstanceID(js);
        if (controller_mapped.find(iid) != controller_mapped.end()) {
            SDL_GameControllerClose(gc); // already registered (e.g. via DEVICEADDED)
            return;
        }

        // Reuse a slot a removed pad left behind rather than always appending. The
        //  slot index is the identity wi::input remembers a device by, so it has to
        //  stay put for as long as that device is plugged in.
        size_t slot = controllers.size();
        for (size_t i = 0; i < controllers.size(); ++i) {
            if (controllers[i].controller == nullptr) {
                slot = i;
                break;
            }
        }
        if (slot == controllers.size())
            controllers.emplace_back();

        Internal_ControllerState& controller = controllers[slot];
        controller = Internal_ControllerState();
        controller.controller  = gc;
        controller.portID      = joystick_index;
        controller.internalID  = iid;
        controller.xinput_user = SDL_JoystickGetPlayerIndex(js);
        controller_map_rebuild();
    }

    void Initialize() {
        if(!SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt")){
            wi::backlog::post("[SDL Input] No controller config loaded, add gamecontrollerdb.txt file next to the executable or download it from https://github.com/gabomdq/SDL_GameControllerDB");
        }

        // Enumerate pads already connected at startup. SDL_CONTROLLERDEVICEADDED
        // is only reliable for hot-plug after init; without this scan a controller
        // left plugged across a relaunch stays unregistered and reads 0.
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            AddController(i);
    }

    void ProcessEvent(const SDL_Event &event){
        events.push_back(event);
    }

    void Update()
    {
        mouse.delta_wheel = 0; // Do not accumulate mouse wheel motion delta
        mouse.delta_position = XMFLOAT2(0, 0); // Do not accumulate mouse position delta

        for(auto& event : events){
            switch(event.type){
                // Keyboard events
                case SDL_KEYDOWN:             // Key pressed
                {
                    int converted = to_wicked(event.key.keysym.scancode, event.key.keysym.sym);
                    if (converted >= 0) {
                        keyboard.buttons[converted] = true;
                    }
                    break;
                }
                case SDL_KEYUP:               // Key released
                {
                    int converted = to_wicked(event.key.keysym.scancode, event.key.keysym.sym);
                    if (converted >= 0) {
                        keyboard.buttons[converted] = false;
                    }
                }
                case SDL_TEXTEDITING:         // Keyboard text editing (composition)
                case SDL_TEXTINPUT:           // Keyboard text input
                case SDL_KEYMAPCHANGED:       // Keymap changed due to a system event such as an
                    //     input language or keyboard layout change.
                    break;


                    // mouse events
                case SDL_MOUSEMOTION:          // Mouse moved
                    mouse.position.x = event.motion.x;
                    mouse.position.y = event.motion.y;
                    mouse.delta_position.x += event.motion.xrel;
                    mouse.delta_position.y += event.motion.yrel;
                    break;
                case SDL_MOUSEBUTTONDOWN:      // Mouse button pressed
                    switch(event.button.button){
                        case SDL_BUTTON_LEFT:
                            mouse.left_button_press = true;
                            break;
                        case SDL_BUTTON_RIGHT:
                            mouse.right_button_press = true;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            mouse.middle_button_press = true;
                            break;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:        // Mouse button released
                    switch(event.button.button){
                        case SDL_BUTTON_LEFT:
                            mouse.left_button_press = false;
                            break;
                        case SDL_BUTTON_RIGHT:
                            mouse.right_button_press = false;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            mouse.middle_button_press = false;
                            break;
                    }
                    break;
                case SDL_MOUSEWHEEL:           // Mouse wheel motion
                {
                    float delta = static_cast<float>(event.wheel.y);
                    if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                        delta *= -1;
                    }
                    mouse.delta_wheel += delta;
                    break;
                }


                    // Joystick events
                case SDL_JOYAXISMOTION:          // Joystick axis motion
                case SDL_JOYBALLMOTION:          // Joystick trackball motion
                case SDL_JOYHATMOTION:           // Joystick hat position change
                case SDL_JOYBUTTONDOWN:          // Joystick button pressed
                case SDL_JOYBUTTONUP:            // Joystick button released
                case SDL_JOYDEVICEADDED:         // A new joystick has been inserted into the system
                case SDL_JOYDEVICEREMOVED:       // An opened joystick has been removed
                    break;


                    // Game controller events
                case SDL_CONTROLLERAXISMOTION:          // Game controller axis motion
                {
                    auto controller_get = controller_mapped.find(event.caxis.which);
                    if(controller_get != controller_mapped.end()){
                        // Store RAW only. A radial deadzone needs both axes of a stick
                        //  at once, and SDL delivers X and Y as two separate events, so
                        //  it cannot be applied here - it is applied once per frame
                        //  below, after the whole event batch has landed.
                        //  The old code deadzoned each axis on its own at 0.20 with no
                        //  rescale: that squares off the stick gate, and lets one axis
                        //  drop to 0 while its twin still reports motion.
                        const float raw = normalize_axis(event.caxis.value);
                        wi::input::ControllerState& state = controllers[controller_get->second].state;
                        switch(event.caxis.axis){
                            case SDL_CONTROLLER_AXIS_LEFTX:
                                state.thumbstick_L_raw.x = raw;
                                break;
                            case SDL_CONTROLLER_AXIS_LEFTY:
                                // SDL reports Y down-positive; engine wants L up-positive.
                                state.thumbstick_L_raw.y = -raw;
                                break;
                            case SDL_CONTROLLER_AXIS_RIGHTX:
                                state.thumbstick_R_raw.x = raw;
                                break;
                            case SDL_CONTROLLER_AXIS_RIGHTY:
                                // Engine convention keeps the RIGHT stick down-positive.
                                state.thumbstick_R_raw.y = raw;
                                break;
                            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                                state.trigger_L_raw = std::max(0.0f, raw);
                                break;
                            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                                state.trigger_R_raw = std::max(0.0f, raw);
                                break;
                        }
                    }
                    break;
                }
                case SDL_CONTROLLERBUTTONDOWN:          // Game controller button pressed
                {
                    auto find = controller_mapped.find(event.cbutton.which);
                    if(find != controller_mapped.end()){
                        controller_to_wicked(&controllers[find->second].state.buttons, event.cbutton.button, true);
                    }
                    break;
                }
                case SDL_CONTROLLERBUTTONUP:            // Game controller button released
                {
                    auto find = controller_mapped.find(event.cbutton.which);
                    if(find != controller_mapped.end()){
                        controller_to_wicked(&controllers[find->second].state.buttons, event.cbutton.button, false);
                    }
                    break;
                }
                case SDL_CONTROLLERDEVICEADDED:         // A new Game controller has been inserted into the system
                {
                    // cdevice.which is a joystick index for ADDED. AddController
                    // dedups against pads already opened in Initialize().
                    AddController(event.cdevice.which);
                    break;
                }
                case SDL_CONTROLLERDEVICEREMOVED:       // An opened Game controller has been removed
                {
                    auto find = controller_mapped.find(event.cdevice.which);
                    if(find != controller_mapped.end()){
                        SDL_GameControllerClose(controllers[find->second].controller);
                        // Free the slot IN PLACE. This used to swap the last element
                        //  down and pop, which renumbered every pad after the removed
                        //  one - and wi::input remembers a device by that number, so
                        //  unplugging pad 1 silently handed pad 2's stick to player 1.
                        controllers[find->second] = Internal_ControllerState();
                    }
                    controller_map_rebuild();
                    break;
                }
                case SDL_CONTROLLERDEVICEREMAPPED:      // The controller mapping was updated
                    break;


                    // Touch events
                case SDL_FINGERDOWN:
                case SDL_FINGERUP:
                case SDL_FINGERMOTION:
                    wi::backlog::post("finger!");
                    break;


                    // Gesture events
                case SDL_DOLLARGESTURE:
                case SDL_DOLLARRECORD:
                case SDL_MULTIGESTURE:
                    wi::backlog::post("gesture!");
                    break;
                default:
                    break;
            }
            // Clone all events for use outside the internal code, e.g. main_SDL2.cpp can benefit from this
            //external_events.push_back(event);
        }

        // Deadzone pass. Once per frame, after every axis event for this frame has
        //  landed, so both axes of a stick are deadzoned together and by the same
        //  shared rule every other backend uses.
        for(auto& controller : controllers){
            if(controller.controller == nullptr)
                continue;
            wi::input::ControllerState& state = controller.state;
            state.thumbstick_L = wi::input::ApplyStickDeadzone(state.thumbstick_L_raw);
            state.thumbstick_R = wi::input::ApplyStickDeadzone(state.thumbstick_R_raw);
            state.trigger_L    = wi::input::ApplyTriggerDeadzone(state.trigger_L_raw);
            state.trigger_R    = wi::input::ApplyTriggerDeadzone(state.trigger_R_raw);
        }

        //Update rumble every call
        for(auto& controller : controllers){
            if(controller.controller == nullptr)
                continue;
            SDL_GameControllerRumble(
                controller.controller,
                controller.rumble_l,
                controller.rumble_r,
                60); //Buffer at 60ms
        }

        //Flush away stored events
        events.clear();
    }

    int to_wicked(const SDL_Scancode &scan, const SDL_Keycode &sym) {

        // Scancode Conversion Segment

        if(sym >= SDLK_a && sym <= SDLK_z){ // A to Z
            return (sym - SDLK_a) + CHARACTER_RANGE_START;
        }
        if(sym >= SDLK_0 && sym <= SDLK_9){ // 1 to 9
            return (sym - SDLK_0) + DIGIT_RANGE_START;
        }
        if(scan >= 58 && scan <= 69){ // F1 to F12
			return (scan - 58) + KEYBOARD_BUTTON_F1;
        }
        if(scan >= 79 && scan <= 82){ // Keyboard directional buttons
            return (82 - scan) + KEYBOARD_BUTTON_UP;
        }
        switch(scan){ // Individual scancode key conversion
            case SDL_SCANCODE_SPACE:
                return wi::input::KEYBOARD_BUTTON_SPACE;
            case SDL_SCANCODE_LSHIFT:
                return wi::input::KEYBOARD_BUTTON_LSHIFT;
            case SDL_SCANCODE_RSHIFT:
                return wi::input::KEYBOARD_BUTTON_RSHIFT;
            case SDL_SCANCODE_RETURN:
                return wi::input::KEYBOARD_BUTTON_ENTER;
            case SDL_SCANCODE_ESCAPE:
                return wi::input::KEYBOARD_BUTTON_ESCAPE;
            case SDL_SCANCODE_HOME:
                return wi::input::KEYBOARD_BUTTON_HOME;
            case SDL_SCANCODE_RCTRL:
                return wi::input::KEYBOARD_BUTTON_RCONTROL;
            case SDL_SCANCODE_LCTRL:
                return wi::input::KEYBOARD_BUTTON_LCONTROL;
            case SDL_SCANCODE_DELETE:
                return wi::input::KEYBOARD_BUTTON_DELETE;
            case SDL_SCANCODE_BACKSPACE:
                return wi::input::KEYBOARD_BUTTON_BACKSPACE;
            case SDL_SCANCODE_PAGEDOWN:
                return wi::input::KEYBOARD_BUTTON_PAGEDOWN;
            case SDL_SCANCODE_PAGEUP:
                return wi::input::KEYBOARD_BUTTON_PAGEUP;
            case SDL_SCANCODE_KP_0:
                return wi::input::KEYBOARD_BUTTON_NUMPAD0;
            case SDL_SCANCODE_KP_1:
                return wi::input::KEYBOARD_BUTTON_NUMPAD1;
            case SDL_SCANCODE_KP_2:
                return wi::input::KEYBOARD_BUTTON_NUMPAD2;
            case SDL_SCANCODE_KP_3:
                return wi::input::KEYBOARD_BUTTON_NUMPAD3;
            case SDL_SCANCODE_KP_4:
                return wi::input::KEYBOARD_BUTTON_NUMPAD4;
            case SDL_SCANCODE_KP_5:
                return wi::input::KEYBOARD_BUTTON_NUMPAD5;
            case SDL_SCANCODE_KP_6:
                return wi::input::KEYBOARD_BUTTON_NUMPAD6;
            case SDL_SCANCODE_KP_7:
                return wi::input::KEYBOARD_BUTTON_NUMPAD7;
            case SDL_SCANCODE_KP_8:
                return wi::input::KEYBOARD_BUTTON_NUMPAD8;
            case SDL_SCANCODE_KP_9:
                return wi::input::KEYBOARD_BUTTON_NUMPAD9;
            case SDL_SCANCODE_KP_MULTIPLY:
                return wi::input::KEYBOARD_BUTTON_MULTIPLY;
            case SDL_SCANCODE_KP_PLUS:
                return wi::input::KEYBOARD_BUTTON_ADD;
            case SDL_SCANCODE_SEPARATOR:
                return wi::input::KEYBOARD_BUTTON_SEPARATOR;
            case SDL_SCANCODE_KP_MINUS:
                return wi::input::KEYBOARD_BUTTON_SUBTRACT;
            case SDL_SCANCODE_KP_DECIMAL:
                return wi::input::KEYBOARD_BUTTON_DECIMAL;
            case SDL_SCANCODE_KP_DIVIDE:
                return wi::input::KEYBOARD_BUTTON_DIVIDE;
            case SDL_SCANCODE_INSERT:
                return wi::input::KEYBOARD_BUTTON_INSERT;
            case SDL_SCANCODE_TAB:
                return wi::input::KEYBOARD_BUTTON_TAB;
            case SDL_SCANCODE_GRAVE:
                return wi::input::KEYBOARD_BUTTON_TILDE;
            case SDL_SCANCODE_LALT:
                return wi::input::KEYBOARD_BUTTON_ALT;
            case SDL_SCANCODE_RALT:
                return wi::input::KEYBOARD_BUTTON_ALTGR;
            default:
                break;
        }


        // Keycode Conversion Segment

        if(sym >= 91 && sym <= 126){
            return sym;
        }

        return -1;

    }

    void controller_to_wicked(uint32_t *current, Uint8 button, bool pressed){
        uint32_t btnenum;
        switch(button){
            case SDL_CONTROLLER_BUTTON_DPAD_UP: btnenum = wi::input::GAMEPAD_BUTTON_UP; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: btnenum = wi::input::GAMEPAD_BUTTON_LEFT; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: btnenum = wi::input::GAMEPAD_BUTTON_DOWN; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: btnenum = wi::input::GAMEPAD_BUTTON_RIGHT; break;
            case SDL_CONTROLLER_BUTTON_X: btnenum = wi::input::GAMEPAD_BUTTON_1; break;
            case SDL_CONTROLLER_BUTTON_A: btnenum = wi::input::GAMEPAD_BUTTON_2; break;
            case SDL_CONTROLLER_BUTTON_B: btnenum = wi::input::GAMEPAD_BUTTON_3; break;
            case SDL_CONTROLLER_BUTTON_Y: btnenum = wi::input::GAMEPAD_BUTTON_4; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: btnenum = wi::input::GAMEPAD_BUTTON_5; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: btnenum = wi::input::GAMEPAD_BUTTON_6; break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: btnenum = wi::input::GAMEPAD_BUTTON_7; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: btnenum = wi::input::GAMEPAD_BUTTON_8; break;
            case SDL_CONTROLLER_BUTTON_BACK: btnenum = wi::input::GAMEPAD_BUTTON_9; break;
            case SDL_CONTROLLER_BUTTON_START: btnenum = wi::input::GAMEPAD_BUTTON_10; break;
            default: assert(0); return;
        }
        btnenum = 1 << (btnenum - wi::input::GAMEPAD_RANGE_START - 1);
        if(pressed){
            *current |= btnenum;
        }else{
            *current &= ~btnenum;
        }
    }

    // Rebuild controller mappings for fast array access
    void controller_map_rebuild(){
        controller_mapped.clear();
        for(int index = 0; index < (int)controllers.size(); ++index){
            if(controllers[index].controller == nullptr)
                continue; // freed slot, kept so the indices after it do not shift
            controller_mapped.insert({controllers[index].internalID, (size_t)index});
        }
    }

    void GetKeyboardState(wi::input::KeyboardState* state) {
        *state = keyboard;
    }
    void GetMouseState(wi::input::MouseState* state) {
        *state = mouse;
    }

    int GetMaxControllerCount() { return (int)controllers.size(); }
    bool GetControllerState(wi::input::ControllerState* state, int index) {
        if(index >= 0 && index < (int)controllers.size() && controllers[index].controller != nullptr){
            if (state != nullptr)
            {
                *state = controllers[index].state;
            }
            return true;
        }
        return false;
    }
    int GetControllerXInputUserIndex(int index) {
        if(index >= 0 && index < (int)controllers.size() && controllers[index].controller != nullptr)
            return controllers[index].xinput_user;
        return -1;
    }
    void SetControllerFeedback(const wi::input::ControllerFeedback& data, int index) {
        if(index >= 0 && index < (int)controllers.size() && controllers[index].controller != nullptr){
#ifdef SDL2_FEATURE_CONTROLLER_LED
            SDL_GameControllerSetLED(
                controllers[index].controller,
                data.led_color.getR(),
                data.led_color.getG(),
                data.led_color.getB());
#endif
            controllers[index].rumble_l = (Uint16)floor(data.vibration_left * 0xFFFF);
            controllers[index].rumble_r = (Uint16)floor(data.vibration_right * 0xFFFF);
        }
    }
}
#else
namespace wi::input::sdlinput
{
    void Initialize() {}
    void Update() {}
    void GetKeyboardState(wi::input::KeyboardState* state) {}
    void GetMouseState(wi::input::MouseState* state) {}
    int GetMaxControllerCount() { return 0; }
    bool GetControllerState(wi::input::ControllerState* state, int index) { return false; }
    int GetControllerXInputUserIndex(int index) { return -1; }
    void SetControllerFeedback(const wi::input::ControllerFeedback& data, int index) {}
}
#endif // _WIN32
