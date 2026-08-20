# GFX Library SDL Support - Usage Example

## Overview
The GFX library now supports SDL for desktop development when compiled with `-DGFXSDL`. This enables cross-platform graphics rendering with mouse input support.

## Building with SDL Support

### CMakeLists.txt Configuration
```cmake
# Add SDL2 dependency
find_package(SDL2 REQUIRED)

# Add this to your target
target_link_libraries(your_app PRIVATE SDL2::SDL2)

# Enable SDL support
target_compile_definitions(your_app PRIVATE GFXSDL)
```

## Basic Usage

### Creating an SDL Window
```cpp
#include "GFX.h"

int main() {
    // Create a window with title, width, and height
    LinuxGFX gfx("My App", 1024, 768);
    
    // Draw something
    gfx.fillScreen(GFX_BLACK);
    gfx.fillCircle(512, 384, 50, GFX_RED);
    
    // Update display
    gfx.swapBuffers();
    
    // Event loop
    while (gfx.processEvents()) {
        // Handle input and draw each frame
        gfx.fillScreen(GFX_BLACK);
        gfx.drawRect(100, 100, 200, 150, GFX_YELLOW);
        gfx.swapBuffers();
    }
    
    return 0;
}
```

## Event Handling with Callbacks

### Setting Up an Event Callback
```cpp
#include "GFX.h"
#include <iostream>

int main() {
    LinuxGFX gfx("Input Demo", 1024, 768);
    
    // Define event callback
    gfx.setEventCallback([](const GFXInputEvent& event) {
        switch (event.type) {
            case GFXEventType::MOUSE_MOVE:
                std::cout << "Mouse moved to: (" << event.x << ", " << event.y << ")\n";
                break;
            
            case GFXEventType::MOUSE_BUTTON_DOWN:
                std::cout << "Button pressed at: (" << event.x << ", " << event.y 
                         << "), Button: " << (int)event.button << "\n";
                break;
            
            case GFXEventType::MOUSE_BUTTON_UP:
                std::cout << "Button released at: (" << event.x << ", " << event.y 
                         << "), Button: " << (int)event.button << "\n";
                break;
        }
    });
    
    // Main loop
    while (gfx.processEvents()) {
        int16_t mouseX, mouseY;
        gfx.getMousePosition(mouseX, mouseY);
        
        gfx.fillScreen(GFX_BLACK);
        gfx.fillCircle(mouseX, mouseY, 20, GFX_CYAN);
        gfx.swapBuffers();
    }
    
    return 0;
}
```

## Event Types

### GFXEventType Enum
- `MOUSE_MOVE` - Mouse cursor moved
- `MOUSE_BUTTON_DOWN` - Mouse button pressed (x, y: position, button: 0=left, 1=middle, 2=right)
- `MOUSE_BUTTON_UP` - Mouse button released (x, y: position, button: 0=left, 1=middle, 2=right)

### GFXInputEvent Structure
```cpp
struct GFXInputEvent {
    GFXEventType type;      // Event type
    int16_t x;              // Mouse X coordinate
    int16_t y;              // Mouse Y coordinate
    uint8_t button;         // Mouse button (0-2)
};
```

## API Methods

### SDL-Specific Methods
```cpp
// Process pending events
// Returns false when quit is requested
bool processEvents();

// Register an event callback
void setEventCallback(const GFXEventCallback& callback);

// Get current mouse position
void getMousePosition(int16_t& x, int16_t& y) const;
```

## Framebuffer Backend (Original)

When compiled without `-DGFXSDL`, the library uses the Linux framebuffer:
```cpp
LinuxGFX gfx("/dev/fb0");  // or gfx() for default
```

## Example: Interactive Drawing

```cpp
#include "GFX.h"

int main() {
    LinuxGFX gfx("Drawing App", 1024, 768);
    gfx.fillScreen(GFX_WHITE);
    
    int16_t lastX = -1, lastY = -1;
    bool drawing = false;
    
    gfx.setEventCallback([&](const GFXInputEvent& event) {
        if (event.type == GFXEventType::MOUSE_BUTTON_DOWN) {
            drawing = true;
            lastX = event.x;
            lastY = event.y;
        }
        else if (event.type == GFXEventType::MOUSE_BUTTON_UP) {
            drawing = false;
        }
        else if (event.type == GFXEventType::MOUSE_MOVE && drawing && lastX >= 0) {
            gfx.drawLine(lastX, lastY, event.x, event.y, GFX_BLACK);
            lastX = event.x;
            lastY = event.y;
        }
    });
    
    while (gfx.processEvents()) {
        gfx.swapBuffers();
    }
    
    return 0;
}
```

## Notes

- SDL windows are created at runtime with configurable dimensions
- Mouse coordinates are provided as-is from SDL (top-left is 0,0)
- All drawing functions work identically on both backends (SDL and framebuffer)
- When GFXSDL is defined, framebuffer functions are disabled
- The library handles multi-buffer swapping for both backends
- SDL_Quit() is called automatically in the destructor
