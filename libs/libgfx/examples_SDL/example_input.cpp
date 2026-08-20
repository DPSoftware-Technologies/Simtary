#include "GFX.h"
#include <iostream>

int main() {
    LinuxGFX gfx("Mouse Input Demo", 800, 600);
    
    int16_t mouseX = 400, mouseY = 300;
    
    // Set up event callback for mouse tracking
    gfx.setEventCallback([&](const GFXInputEvent& event) {
        switch (event.type) {
            case GFXEventType::MOUSE_MOVE:
                mouseX = event.x;
                mouseY = event.y;
                break;
            
            case GFXEventType::MOUSE_BUTTON_DOWN:
                std::cout << "Click at (" << event.x << ", " << event.y << ")\n";
                break;
            
            case GFXEventType::MOUSE_BUTTON_UP:
                std::cout << "Released\n";
                break;
        }
    });
    
    // Main loop
    while (gfx.processEvents()) {
        gfx.fillScreen(GFX_BLACK);
        
        // Draw circle at mouse position
        gfx.fillCircle(mouseX, mouseY, 20, GFX_CYAN);
        
        // Draw text
        gfx.setTextColor(GFX_WHITE);
        gfx.setCursor(10, 10);
        gfx.writeText("Move mouse and click");
        
        gfx.swapBuffers();
    }
    
    return 0;
}
